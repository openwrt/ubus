/*
 * Copyright (C) 2012 Jo-Philipp Wich <jow@openwrt.org>
 * Copyright (C) 2012 John Crispin <blogic@openwrt.org>
 * Copyright (C) 2016 Iain Fraser <iainf@netduma.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <lauxlib.h>
#include <lua.h>

#define MODNAME		"ubus"
#define METANAME	MODNAME ".meta"

static lua_State *state;

struct ubus_lua_connection {
	int timeout;
	struct blob_buf buf;
	struct ubus_context *ctx;
};

struct ubus_lua_object {
	struct ubus_object o;
	int r;
	int rsubscriber;
};

struct ubus_lua_event {
	struct ubus_event_handler e;
	int r;
};

struct ubus_lua_subscriber {
	struct ubus_subscriber s;
	int rnotify;
	int rremove;
};

static int
ubus_lua_parse_blob(lua_State *L, struct blob_attr *attr, bool table);

static int
ubus_lua_parse_blob_array(lua_State *L, struct blob_attr *attr, size_t len, bool table)
{
	int rv;
	int idx = 1;
	size_t rem = len;
	struct blob_attr *pos;

	lua_newtable(L);

	__blob_for_each_attr(pos, attr, rem)
	{
		rv = ubus_lua_parse_blob(L, pos, table);

		if (rv > 1)
			lua_rawset(L, -3);
		else if (rv > 0)
			lua_rawseti(L, -2, idx++);
	}

	return 1;
}

static int
ubus_lua_parse_blob(lua_State *L, struct blob_attr *attr, bool table)
{
	int len;
	int off = 0;
	void *data;

	if (!blobmsg_check_attr(attr, false))
		return 0;

	if (table && blobmsg_name(attr)[0])
	{
		lua_pushstring(L, blobmsg_name(attr));
		off++;
	}

	data = blobmsg_data(attr);
	len = blobmsg_data_len(attr);

	switch (blob_id(attr))
	{
	case BLOBMSG_TYPE_BOOL:
		lua_pushboolean(L, *(uint8_t *)data);
		break;

	case BLOBMSG_TYPE_INT16:
		lua_pushinteger(L, be16_to_cpu(*(uint16_t *)data));
		break;

	case BLOBMSG_TYPE_INT32:
		lua_pushinteger(L, be32_to_cpu(*(uint32_t *)data));
		break;

	case BLOBMSG_TYPE_INT64:
		lua_pushnumber(L, (double) be64_to_cpu(*(uint64_t *)data));
		break;

	case BLOBMSG_TYPE_DOUBLE:
		{
			union {
				double d;
				uint64_t u64;
			} v;
			v.u64 = be64_to_cpu(*(uint64_t *)data);
			lua_pushnumber(L, v.d);
		}
		break;

	case BLOBMSG_TYPE_STRING:
		lua_pushstring(L, data);
		break;

	case BLOBMSG_TYPE_ARRAY:
		ubus_lua_parse_blob_array(L, data, len, false);
		break;

	case BLOBMSG_TYPE_TABLE:
		ubus_lua_parse_blob_array(L, data, len, true);
		break;

	default:
		lua_pushnil(L);
		break;
	}

	return off + 1;
}


static bool
ubus_lua_format_blob_is_array(lua_State *L)
{
	lua_Integer prv = 0;
	lua_Integer cur = 0;

	/* Find out whether table is array-like */
	for (lua_pushnil(L); lua_next(L, -2); lua_pop(L, 1))
	{
#ifdef LUA_TINT
		if (lua_type(L, -2) != LUA_TNUMBER && lua_type(L, -2) != LUA_TINT)
#else
		if (lua_type(L, -2) != LUA_TNUMBER)
#endif
		{
			lua_pop(L, 2);
			return false;
		}

		cur = lua_tointeger(L, -2);

		if ((cur - 1) != prv)
		{
			lua_pop(L, 2);
			return false;
		}

		prv = cur;
	}

	return true;
}

static int
ubus_lua_format_blob_array(lua_State *L, struct blob_buf *b, bool table);

static int
ubus_lua_format_blob(lua_State *L, struct blob_buf *b, bool table)
{
	void *c;
	bool rv = true;
	const char *key = table ? lua_tostring(L, -2) : NULL;

	switch (lua_type(L, -1))
	{
	case LUA_TBOOLEAN:
		blobmsg_add_u8(b, key, (uint8_t)lua_toboolean(L, -1));
		break;

#ifdef LUA_TINT
	case LUA_TINT:
#endif
	case LUA_TNUMBER:
		if ((uint64_t)lua_tonumber(L, -1) > INT_MAX)
			blobmsg_add_u64(b, key, (uint64_t)lua_tonumber(L, -1));
		else
			blobmsg_add_u32(b, key, (uint32_t)lua_tointeger(L, -1));
		break;

	case LUA_TSTRING:
	case LUA_TUSERDATA:
	case LUA_TLIGHTUSERDATA:
		blobmsg_add_string(b, key, lua_tostring(L, -1));
		break;

	case LUA_TTABLE:
		if (ubus_lua_format_blob_is_array(L))
		{
			c = blobmsg_open_array(b, key);
			rv = ubus_lua_format_blob_array(L, b, false);
			blobmsg_close_array(b, c);
		}
		else
		{
			c = blobmsg_open_table(b, key);
			rv = ubus_lua_format_blob_array(L, b, true);
			blobmsg_close_table(b, c);
		}
		break;

	default:
		rv = false;
		break;
	}

	return rv;
}

static int
ubus_lua_format_blob_array(lua_State *L, struct blob_buf *b, bool table)
{
	for (lua_pushnil(L); lua_next(L, -2); lua_pop(L, 1))
	{
		if (!ubus_lua_format_blob(L, b, table))
		{
			lua_pop(L, 1);
			return false;
		}
	}

	return true;
}


static int
ubus_lua_connect(lua_State *L)
{
	struct ubus_lua_connection *c;
	const char *sockpath = luaL_optstring(L, 1, NULL);
	int timeout = luaL_optint(L, 2, 30);

	if ((c = lua_newuserdata(L, sizeof(*c))) != NULL &&
		(c->ctx = ubus_connect(sockpath)) != NULL)
	{
		ubus_add_uloop(c->ctx);
		c->timeout = timeout;
		memset(&c->buf, 0, sizeof(c->buf));
		luaL_getmetatable(L, METANAME);
		lua_setmetatable(L, -2);
		return 1;
	}

	/* NB: no errors from ubus_connect() yet */
	lua_pushnil(L);
	lua_pushinteger(L, UBUS_STATUS_UNKNOWN_ERROR);
	return 2;
}


static void
ubus_lua_objects_cb(struct ubus_context *c, struct ubus_object_data *o, void *p)
{
	lua_State *L = (lua_State *)p;

	lua_pushstring(L, o->path);
	lua_rawseti(L, -2, lua_objlen(L, -2) + 1);
}

static int
ubus_lua_objects(lua_State *L)
{
	int rv;
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);
	const char *path = (lua_gettop(L) >= 2) ? luaL_checkstring(L, 2) : NULL;

	lua_newtable(L);
	rv = ubus_lookup(c->ctx, path, ubus_lua_objects_cb, L);

	if (rv != UBUS_STATUS_OK)
	{
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushinteger(L, rv);
		return 2;
	}

	return 1;
}

static int
ubus_method_handler(struct ubus_context *ctx, struct ubus_object *obj,
		struct ubus_request_data *req, const char *method,
		struct blob_attr *msg)
{
	struct ubus_lua_object *o = container_of(obj, struct ubus_lua_object, o);
	int rv = 0;

	lua_getglobal(state, "__ubus_cb");
	lua_rawgeti(state, -1, o->r);
	lua_getfield(state, -1, method);
	lua_remove(state, -2);
	lua_remove(state, -2);

	if (lua_isfunction(state, -1)) {
		lua_pushlightuserdata(state, req);
		if (!msg)
			lua_pushnil(state);
		else
			ubus_lua_parse_blob_array(state, blob_data(msg), blob_len(msg), true);
		lua_call(state, 2, 1);
		if (lua_isnumber(state, -1))
			rv = lua_tonumber(state, -1);
	}

	lua_pop(state, 1);

	return rv;
}

static int lua_gettablelen(lua_State *L, int index)
{
	int cnt = 0;

	lua_pushnil(L);
	index -= 1;
	while (lua_next(L, index) != 0) {
		cnt++;
		lua_pop(L, 1);
	}

	return cnt;
}

static int ubus_lua_reply(lua_State *L)
{
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);
	struct ubus_request_data *req;

	luaL_checktype(L, 3, LUA_TTABLE);
	blob_buf_init(&c->buf, 0);

	if (!ubus_lua_format_blob_array(L, &c->buf, true))
	{
		lua_pushnil(L);
		lua_pushinteger(L, UBUS_STATUS_INVALID_ARGUMENT);
		return 2;
	}

	req = lua_touserdata(L, 2);
	ubus_send_reply(c->ctx, req, c->buf.head);

	return 0;
}

static int ubus_lua_defer_request(lua_State *L)
{
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);
	struct ubus_request_data *req = lua_touserdata(L, 2);
	struct ubus_request_data *new_req = lua_newuserdata(L, sizeof(struct ubus_request_data));
	ubus_defer_request(c->ctx, req, new_req);

	return 1;
}

static int ubus_lua_complete_deferred_request(lua_State *L)
{
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);
	struct ubus_request_data *req = lua_touserdata(L, 2);
	int ret = luaL_checkinteger(L, 3);
	ubus_complete_deferred_request(c->ctx, req, ret);

	return 0;
}

static int ubus_lua_load_methods(lua_State *L, struct ubus_method *m)
{
	struct blobmsg_policy *p;
	int plen;
	int pidx = 0;

	/* get the function pointer */
	lua_pushinteger(L, 1);
	lua_gettable(L, -2);

	/* get the policy table */
	lua_pushinteger(L, 2);
	lua_gettable(L, -3);

	/* check if the method table is valid */
	if ((lua_type(L, -2) != LUA_TFUNCTION) ||
			(lua_type(L, -1) != LUA_TTABLE) ||
			lua_objlen(L, -1)) {
		lua_pop(L, 2);
		return 1;
	}

	/* store function pointer */
	lua_pushvalue(L, -2);
	lua_setfield(L, -6, lua_tostring(L, -5));

	m->name = lua_tostring(L, -4);
	m->handler = ubus_method_handler;

	plen = lua_gettablelen(L, -1);

	/* exit if policy table is empty */
	if (!plen) {
		lua_pop(L, 2);
		return 0;
	}

	/* setup the policy pointers */
	p = calloc(plen, sizeof(struct blobmsg_policy));
	if (!p)
		return 1;

	m->policy = p;
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		int val = lua_tointeger(L, -1);

		/* check if the policy is valid */
		if ((lua_type(L, -2) != LUA_TSTRING) ||
				(lua_type(L, -1) != LUA_TNUMBER) ||
				(val < 0) ||
				(val > BLOBMSG_TYPE_LAST)) {
			lua_pop(L, 1);
			continue;
		}
		p[pidx].name = lua_tostring(L, -2);
		p[pidx].type = val;
		lua_pop(L, 1);
		pidx++;
	}

	m->n_policy = pidx;
	lua_pop(L, 2);

	return 0;
}

static void
ubus_new_sub_cb(struct ubus_context *ctx, struct ubus_object *obj)
{
	struct ubus_lua_object *luobj;

	luobj = container_of(obj, struct ubus_lua_object, o);

	lua_getglobal(state, "__ubus_cb_publisher");
	lua_rawgeti(state, -1, luobj->rsubscriber);
	lua_remove(state, -2);

	if (lua_isfunction(state, -1)) {
		lua_pushnumber(state, luobj->o.has_subscribers );
		lua_call(state, 1, 0);
	} else {
		lua_pop(state, 1);
	}
}

static void
ubus_lua_load_newsub_cb( lua_State *L, struct ubus_lua_object *obj )
{
	/* keep ref to func */
	lua_getglobal(L, "__ubus_cb_publisher");
	lua_pushvalue(L, -2);
	obj->rsubscriber = luaL_ref(L, -2);
	lua_pop(L, 1);

	/* real callback */
	obj->o.subscribe_cb = ubus_new_sub_cb;
	return;
}

static struct ubus_object* ubus_lua_load_object(lua_State *L)
{
	struct ubus_lua_object *obj = NULL;
	int mlen = lua_gettablelen(L, -1);
	struct ubus_method *m = NULL;
	int midx = 0;

	/* setup object pointers */
	obj = calloc(1, sizeof(struct ubus_lua_object));
	if (!obj)
		return NULL;

	obj->o.name = lua_tostring(L, -2);

	/* setup method pointers */
	if (mlen > 0) {
		m = calloc(mlen, sizeof(struct ubus_method));
		if (!m) {
			free(obj);
			return NULL;
		}
	}
	obj->o.methods = m;

	/* setup type pointers */
	obj->o.type = calloc(1, sizeof(struct ubus_object_type));
	if (!obj->o.type) {
		free(m);
		free(obj);
		return NULL;
	}

	obj->o.type->name = lua_tostring(L, -2);
	obj->o.type->id = 0;
	obj->o.type->methods = obj->o.methods;

	/* create the callback lookup table */
	lua_createtable(L, 1, 0);
	lua_getglobal(L, "__ubus_cb");
	lua_pushvalue(L, -2);
	obj->r = luaL_ref(L, -2);
	lua_pop(L, 1);

	/* scan each method */
	lua_pushnil(L);
	while (lua_next(L, -3) != 0) {
                /* check if its the subscriber notification callback */
                if( lua_type( L, -2 ) == LUA_TSTRING &&
                                lua_type( L, -1 ) == LUA_TFUNCTION ){
                  if( !strcmp( lua_tostring( L, -2 ), "__subscriber_cb" ) )
                          ubus_lua_load_newsub_cb( L, obj );
                }

		/* check if it looks like a method */
		if ((lua_type(L, -2) != LUA_TSTRING) ||
				(lua_type(L, -1) != LUA_TTABLE) ||
				!lua_objlen(L, -1)) {
			lua_pop(L, 1);
			continue;
		}

		if (!ubus_lua_load_methods(L, &m[midx]))
			midx++;
		lua_pop(L, 1);
	}

	obj->o.type->n_methods = obj->o.n_methods = midx;

	/* pop the callback table */
	lua_pop(L, 1);

	return &obj->o;
}

static int ubus_lua_add(lua_State *L)
{
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);

	/* verify top level object */
	if (!lua_istable(L, 2)) {
		lua_pushstring(L, "you need to pass a table");
		return lua_error(L);
	}

	/* scan each object */
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		struct ubus_object *obj = NULL;

		/* check if the object has a table of methods */
		if ((lua_type(L, -2) == LUA_TSTRING) && (lua_type(L, -1) == LUA_TTABLE)) {
			obj = ubus_lua_load_object(L);

			if (obj){
				ubus_add_object(c->ctx, obj);

                                /* allow future reference of ubus obj */
				lua_pushstring(state,"__ubusobj");
				lua_pushlightuserdata(state, obj);
				lua_settable(state,-3);
                        }
		}
		lua_pop(L, 1);
	}

	return 0;
}

static int
ubus_lua_notify( lua_State *L )
{
	struct ubus_lua_connection *c;
	struct ubus_object *obj;
	const char* method;

	c = luaL_checkudata(L, 1, METANAME);
	method = luaL_checkstring(L, 3);
	luaL_checktype(L, 4, LUA_TTABLE);

	if( !lua_islightuserdata( L, 2 ) ){
		lua_pushfstring( L, "Invald 2nd parameter, expected ubus obj ref" );
		return lua_error( L );
	}
	obj = lua_touserdata( L, 2 );

	/* create parameters from table */
	blob_buf_init(&c->buf, 0);
	if( !ubus_lua_format_blob_array( L, &c->buf, true ) ){
		lua_pushfstring( L, "Invalid 4th parameter, expected table of arguments" );
		return lua_error( L );
	}

	ubus_notify( c->ctx, obj, method, c->buf.head, -1 );
	return 0;
}

static void
ubus_lua_signatures_cb(struct ubus_context *c, struct ubus_object_data *o, void *p)
{
	lua_State *L = (lua_State *)p;

	if (!o->signature)
		return;

	ubus_lua_parse_blob_array(L, blob_data(o->signature), blob_len(o->signature), true);
}

static int
ubus_lua_signatures(lua_State *L)
{
	int rv;
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);
	const char *path = luaL_checkstring(L, 2);

	rv = ubus_lookup(c->ctx, path, ubus_lua_signatures_cb, L);

	if (rv != UBUS_STATUS_OK)
	{
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushinteger(L, rv);
		return 2;
	}

	return 1;
}


static void
ubus_lua_call_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	lua_State *L = (lua_State *)req->priv;

	if (!msg && L)
		lua_pushnil(L);

	if (msg && L)
		ubus_lua_parse_blob_array(L, blob_data(msg), blob_len(msg), true);
}

static int
ubus_lua_call(lua_State *L)
{
	int rv, top;
	uint32_t id;
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);
	const char *path = luaL_checkstring(L, 2);
	const char *func = luaL_checkstring(L, 3);

	luaL_checktype(L, 4, LUA_TTABLE);
	blob_buf_init(&c->buf, 0);

	if (!ubus_lua_format_blob_array(L, &c->buf, true))
	{
		lua_pushnil(L);
		lua_pushinteger(L, UBUS_STATUS_INVALID_ARGUMENT);
		return 2;
	}

	rv = ubus_lookup_id(c->ctx, path, &id);

	if (rv)
	{
		lua_pushnil(L);
		lua_pushinteger(L, rv);
		return 2;
	}

	top = lua_gettop(L);
	rv = ubus_invoke(c->ctx, id, func, c->buf.head, ubus_lua_call_cb, L, c->timeout * 1000);

	if (rv != UBUS_STATUS_OK)
	{
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushinteger(L, rv);
		return 2;
	}

	return lua_gettop(L) - top;
}

/*
 * Asynchronous invoke.
 *
 * The synchronous ubus_invoke() above runs its own event loop until the answer
 * arrives, so a caller inside uloop stalls everything else it is running —
 * timers, sockets, other callbacks — for as long as the peer takes. That is
 * fine for a script and wrong for a daemon: one 'ubus call file exec sleep 5'
 * freezes the whole process for five seconds.
 *
 * ubus_invoke_async() hands the request to the uloop the connection is already
 * attached to (ubus_add_uloop in ubus_lua_connect) and returns immediately.
 * What libubus does not provide is a deadline: a peer that never answers keeps
 * the request on the pending list forever. So each call carries its own uloop
 * timeout, and on expiry the request is aborted and the callback is told.
 *
 * ubus_abort_request() does not run complete_cb — it only unlinks the request —
 * so exactly one of the two paths frees the state, guarded by ->done.
 */

struct ubus_lua_async_call {
	struct ubus_request req;
	struct uloop_timeout timeout;
	struct ubus_context *ctx;
	int cb_ref;
	int result_ref;
	int fd;
	bool have_fd;
	bool done;
};

static void
ubus_lua_async_finish(struct ubus_lua_async_call *call, int status)
{
	lua_State *L = state;

	if (call->done)
		return;
	call->done = true;
	uloop_timeout_cancel(&call->timeout);

	lua_getglobal(L, "__ubus_cb_async");
	lua_rawgeti(L, -1, call->cb_ref);
	lua_remove(L, -2);

	if (lua_isfunction(L, -1)) {
		if (call->result_ref != LUA_NOREF) {
			lua_getglobal(L, "__ubus_cb_async");
			lua_rawgeti(L, -1, call->result_ref);
			lua_remove(L, -2);
		} else {
			lua_pushnil(L);
		}
		lua_pushinteger(L, status);
		/*
		 * Third value rather than a second callback: Lua discards extra
		 * arguments, so every existing cb(result, status) keeps working
		 * unchanged and the ones that want the descriptor take a third
		 * parameter. nil where there was none.
		 */
		if (call->have_fd)
			lua_pushinteger(L, call->fd);
		else
			lua_pushnil(L);

		/*
		 * pcall, not call: this runs from a uloop callback, and an error
		 * thrown out of here would longjmp past libubus' own bookkeeping
		 * and take the process with it.
		 */
		if (lua_pcall(L, 3, 0, 0) != 0) {
			fprintf(stderr, "ubus: async callback failed: %s\n",
				lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	} else {
		lua_pop(L, 1);

		/*
		 * Nobody to hand it to. libubus would have closed it had we not
		 * taken it, so not closing it here is a descriptor leaked for the
		 * life of the process.
		 */
		if (call->have_fd && call->fd >= 0)
			close(call->fd);
	}

	lua_getglobal(L, "__ubus_cb_async");
	luaL_unref(L, -1, call->cb_ref);
	if (call->result_ref != LUA_NOREF)
		luaL_unref(L, -1, call->result_ref);
	lua_pop(L, 1);

	free(call);
}

static void
ubus_lua_async_data_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct ubus_lua_async_call *call =
		container_of(req, struct ubus_lua_async_call, req);
	lua_State *L = state;

	if (!msg)
		return;

	/* a second reply would leak the first */
	lua_getglobal(L, "__ubus_cb_async");
	if (call->result_ref != LUA_NOREF)
		luaL_unref(L, -1, call->result_ref);
	ubus_lua_parse_blob_array(L, blob_data(msg), blob_len(msg), true);
	call->result_ref = luaL_ref(L, -2);
	lua_pop(L, 1);
}

/*
 * A peer may answer with a file descriptor rather than with data — 'log read'
 * with stream:true is the case this exists for, and libubus already carries
 * the mechanism (libubus.h ubus_fd_handler_t, req->fd_cb).
 *
 * ubus_process_req_msg() calls this on UBUS_MSG_STATUS, before the completion
 * runs, and closes the descriptor itself if nobody claims it. Claiming it
 * therefore means owning it: from here on it is ours to hand to Lua, and Lua's
 * to close.
 */
static void
ubus_lua_async_fd_cb(struct ubus_request *req, int fd)
{
	struct ubus_lua_async_call *call =
		container_of(req, struct ubus_lua_async_call, req);

	/* one descriptor per request; a second would leak the first */
	if (call->have_fd && call->fd >= 0)
		close(call->fd);

	call->fd = fd;
	call->have_fd = true;
}

static void
ubus_lua_async_complete_cb(struct ubus_request *req, int ret)
{
	ubus_lua_async_finish(container_of(req, struct ubus_lua_async_call, req), ret);
}

static void
ubus_lua_async_timeout_cb(struct uloop_timeout *t)
{
	struct ubus_lua_async_call *call =
		container_of(t, struct ubus_lua_async_call, timeout);

	ubus_abort_request(call->ctx, &call->req);
	ubus_lua_async_finish(call, UBUS_STATUS_TIMEOUT);
}

/*
 * conn:call_async(object, method, params, callback [, timeout_seconds])
 *
 * Returns true when the request is on its way. The callback is invoked exactly
 * once, from uloop, with (result_or_nil, status); status is UBUS_STATUS_OK on
 * success and UBUS_STATUS_TIMEOUT when the deadline passed.
 */
static int
ubus_lua_call_async(lua_State *L)
{
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);
	const char *path = luaL_checkstring(L, 2);
	const char *func = luaL_checkstring(L, 3);
	struct ubus_lua_async_call *call;
	uint32_t id;
	int timeout;
	int rv;

	luaL_checktype(L, 4, LUA_TTABLE);
	luaL_checktype(L, 5, LUA_TFUNCTION);
	timeout = luaL_optint(L, 6, c->timeout);

	blob_buf_init(&c->buf, 0);
	lua_pushvalue(L, 4);
	if (!ubus_lua_format_blob_array(L, &c->buf, true)) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushinteger(L, UBUS_STATUS_INVALID_ARGUMENT);
		return 2;
	}
	lua_pop(L, 1);

	rv = ubus_lookup_id(c->ctx, path, &id);
	if (rv) {
		lua_pushnil(L);
		lua_pushinteger(L, rv);
		return 2;
	}

	call = calloc(1, sizeof(*call));
	if (!call) {
		lua_pushnil(L);
		lua_pushinteger(L, UBUS_STATUS_UNKNOWN_ERROR);
		return 2;
	}
	call->ctx = c->ctx;
	call->result_ref = LUA_NOREF;
	call->fd = -1;

	rv = ubus_invoke_async(c->ctx, id, func, c->buf.head, &call->req);
	if (rv != UBUS_STATUS_OK) {
		free(call);
		lua_pushnil(L);
		lua_pushinteger(L, rv);
		return 2;
	}

	/* the callback is anchored only after the invoke can no longer fail */
	lua_getglobal(L, "__ubus_cb_async");
	lua_pushvalue(L, 5);
	call->cb_ref = luaL_ref(L, -2);
	lua_pop(L, 1);

	call->req.data_cb = ubus_lua_async_data_cb;
	call->req.fd_cb = ubus_lua_async_fd_cb;
	call->req.complete_cb = ubus_lua_async_complete_cb;
	call->timeout.cb = ubus_lua_async_timeout_cb;
	if (timeout > 0)
		uloop_timeout_set(&call->timeout, timeout * 1000);

	ubus_complete_request_async(c->ctx, &call->req);

	lua_pushboolean(L, 1);
	return 1;
}


/*
 * ubus.blob_decode(buffer) — one blobmsg record out of a byte string.
 *
 * What comes down the descriptor is not text. 'log read' with stream:true
 * writes blob attributes, each one length prefixed and already carrying its
 * fields — measured on an access point, a record is 116 bytes holding msg, id,
 * priority and source. That is better than the rendered syslog line, which
 * would have to be picked apart again with a regular expression; it is only
 * unusable from Lua, which has no idea what a blob is.
 *
 * Framing belongs here because the format does; buffering stays in Lua, where
 * a growing string is the whole of it. Returns the table and how many bytes it
 * consumed, so the caller can cut them off the front and look again — or nil
 * and 'incomplete' when the record has not all arrived yet, which on a stream
 * is the ordinary case and not an error.
 */
static int
ubus_lua_blob_decode(lua_State *L)
{
	int idx = lua_isstring(L, 1) ? 1 : 2;
	size_t len = 0;
	const char *buf = luaL_checklstring(L, idx, &len);
	const struct blob_attr *attr = (const struct blob_attr *)buf;
	size_t need;

	if (len < sizeof(struct blob_attr)) {
		lua_pushnil(L);
		lua_pushstring(L, "incomplete");
		return 2;
	}
	need = blob_pad_len(attr);
	if (need < sizeof(struct blob_attr)) {
		lua_pushnil(L);
		lua_pushstring(L, "invalid");
		return 2;
	}
	if (len < need) {
		lua_pushnil(L);
		lua_pushstring(L, "incomplete");
		return 2;
	}

	ubus_lua_parse_blob_array(L, blob_data(attr), blob_len(attr), true);
	lua_pushinteger(L, (lua_Integer)need);

	return 2;
}

/*
 * ubus.read_fd(fd [, bytes]) — read from a descriptor a callback was handed.
 *
 * Lua on an OpenWrt access point has no way to do this on its own. io.* wants a
 * FILE*, and posix, nixio and lfs are on none of the devices in this fleet —
 * checked, not assumed; luasocket is there and cannot wrap a descriptor
 * either. Without this the fd delivered above is a number nobody can use.
 *
 * Never blocks. The descriptor is put into non-blocking mode on first use and
 * left that way, which is what any reader driven by uloop wants: uloop says
 * readable, this returns what is there, and a spurious wakeup costs an EAGAIN
 * rather than a stalled daemon.
 *
 * Returns the bytes read; nil, 'eof' when the writer has closed; nil, 'again'
 * when there was nothing to take. Those three are different answers and a
 * caller that treats them alike will either spin or stop early.
 */
static int
ubus_lua_read_fd(lua_State *L)
{
	char buf[4096];
	int idx = lua_isnumber(L, 1) ? 1 : 2;
	int fd = luaL_checkint(L, idx);
	int want = luaL_optint(L, idx + 1, (int)sizeof(buf));
	int flags;
	ssize_t n;

	if (fd < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "bad descriptor");
		return 2;
	}
	if (want < 1)
		want = 1;
	if (want > (int)sizeof(buf))
		want = (int)sizeof(buf);

	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0 && !(flags & O_NONBLOCK))
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	do {
		n = read(fd, buf, (size_t)want);
	} while (n < 0 && errno == EINTR);

	if (n > 0) {
		lua_pushlstring(L, buf, (size_t)n);
		return 1;
	}
	lua_pushnil(L);
	if (n == 0)
		lua_pushstring(L, "eof");
	else if (errno == EAGAIN || errno == EWOULDBLOCK)
		lua_pushstring(L, "again");
	else
		lua_pushstring(L, strerror(errno));

	return 2;
}

/*
 * ubus.close_fd(fd) — or conn:close_fd(fd), the same table is both.
 *
 * A descriptor handed to a callback is the callback's to close, and Lua has no
 * way to do it: io.* wants a FILE*, and posix and nixio are not on every
 * device. Without this the only way to release one is to end the process, which
 * for a daemon reading the log is not a way at all.
 */
static int
ubus_lua_close_fd(lua_State *L)
{
	int idx = lua_isnumber(L, 1) ? 1 : 2;
	int fd = luaL_checkint(L, idx);

	if (fd < 0) {
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, close(fd) == 0);

	return 1;
}

static void
ubus_event_handler(struct ubus_context *ctx, struct ubus_event_handler *ev,
			const char *type, struct blob_attr *msg)
{
	struct ubus_lua_event *listener = container_of(ev, struct ubus_lua_event, e);

	lua_getglobal(state, "__ubus_cb_event");
	lua_rawgeti(state, -1, listener->r);
	lua_remove(state, -2);

	if (lua_isfunction(state, -1)) {
		ubus_lua_parse_blob_array(state, blob_data(msg), blob_len(msg), true);
		lua_call(state, 1, 0);
	} else {
		lua_pop(state, 1);
	}
}

static struct ubus_event_handler*
ubus_lua_load_event(lua_State *L)
{
	struct ubus_lua_event* event = NULL;

	event = calloc(1, sizeof(struct ubus_lua_event));
	if (!event)
		return NULL;

	event->e.cb = ubus_event_handler;

	/* update the he callback lookup table */
	lua_getglobal(L, "__ubus_cb_event");
	lua_pushvalue(L, -2);
	event->r = luaL_ref(L, -2);
	lua_setfield(L, -1, lua_tostring(L, -3));

	return &event->e;
}

static int
ubus_lua_listen(lua_State *L) {
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);

	/* verify top level object */
	luaL_checktype(L, 2, LUA_TTABLE);

	/* scan each object */
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		struct ubus_event_handler *listener;

		/* check if the key is a string and the value is a method */
		if ((lua_type(L, -2) == LUA_TSTRING) && (lua_type(L, -1) == LUA_TFUNCTION)) {
			listener = ubus_lua_load_event(L);
			if(listener != NULL) {
				ubus_register_event_handler(c->ctx, listener, lua_tostring(L, -2));
			}
		}
		lua_pop(L, 1);
	}
	return 0;
}

static void
ubus_sub_remove_handler(struct ubus_context *ctx, struct ubus_subscriber *s,
                            uint32_t id)
{
	struct ubus_lua_subscriber *sub;

	sub = container_of(s, struct ubus_lua_subscriber, s);

	lua_getglobal(state, "__ubus_cb_subscribe");
	lua_rawgeti(state, -1, sub->rremove);
	lua_remove(state, -2);

	if (lua_isfunction(state, -1)) {
		lua_call(state, 0, 0);
	} else {
		lua_pop(state, 1);
	}
}

static int
ubus_sub_notify_handler(struct ubus_context *ctx, struct ubus_object *obj,
            struct ubus_request_data *req, const char *method,
            struct blob_attr *msg)
{
	struct ubus_subscriber *s;
	struct ubus_lua_subscriber *sub;

	s = container_of(obj, struct ubus_subscriber, obj);
	sub = container_of(s, struct ubus_lua_subscriber, s);

	lua_getglobal(state, "__ubus_cb_subscribe");
	lua_rawgeti(state, -1, sub->rnotify);
	lua_remove(state, -2);

	if (lua_isfunction(state, -1)) {
		if( msg ){
			ubus_lua_parse_blob_array(state, blob_data(msg), blob_len(msg), true);
		} else {
			lua_pushnil(state);
		}
		lua_pushstring(state, method);
		lua_call(state, 2, 0);
	} else {
		lua_pop(state, 1);
	}

	return 0;
}



static int
ubus_lua_do_subscribe( struct ubus_context *ctx, lua_State *L, const char* target,
                        int idxnotify, int idxremove )
{
	uint32_t id;
	int status;
	struct ubus_lua_subscriber *sub;

	if( ( status = ubus_lookup_id( ctx, target, &id ) ) ){
		lua_pushfstring( L, "Unable find target, status=%d", status );
		return lua_error( L );
	}

	sub = calloc( 1, sizeof( struct ubus_lua_subscriber ) );
	if( !sub ){
		lua_pushstring( L, "Out of memory" );
		return lua_error( L );
	}

	if( idxnotify ){
		lua_getglobal(L, "__ubus_cb_subscribe");
		lua_pushvalue(L, idxnotify);
		sub->rnotify = luaL_ref(L, -2);
		lua_pop(L, 1);
		sub->s.cb = ubus_sub_notify_handler;
	}

	if( idxremove ){
		lua_getglobal(L, "__ubus_cb_subscribe");
		lua_pushvalue(L, idxremove);
		sub->rremove = luaL_ref(L, -2);
		lua_pop(L, 1);
		sub->s.remove_cb = ubus_sub_remove_handler;
	}

	if( ( status = ubus_register_subscriber( ctx, &sub->s ) ) ){
		lua_pushfstring( L, "Failed to register subscriber, status=%d", status );
		return lua_error( L );
	}

	if( ( status = ubus_subscribe( ctx, &sub->s, id) ) ){
		lua_pushfstring( L, "Failed to register subscriber, status=%d", status );
		return lua_error( L );
	}

	return 0;
}

static int
ubus_lua_subscribe(lua_State *L) {
	int idxnotify, idxremove, stackstart;
	struct ubus_lua_connection *c;
	const char* target;

	idxnotify = idxremove = 0;
	stackstart = lua_gettop( L );


	c = luaL_checkudata(L, 1, METANAME);
	target = luaL_checkstring(L, 2);
	luaL_checktype(L, 3, LUA_TTABLE);


	lua_pushstring( L, "notify");
	lua_gettable( L, 3 );
	if( lua_type( L, -1 ) == LUA_TFUNCTION ){
		idxnotify = lua_gettop( L );
	} else {
		lua_pop( L, 1 );
	}

	lua_pushstring( L, "remove");
	lua_gettable( L, 3 );
	if( lua_type( L, -1 ) == LUA_TFUNCTION ){
		idxremove = lua_gettop( L );
	} else {
		lua_pop( L, 1 );
	}

	if( idxnotify )
		ubus_lua_do_subscribe( c->ctx, L, target, idxnotify, idxremove );

	if( lua_gettop( L ) > stackstart )
		lua_pop( L, lua_gettop( L ) - stackstart );

	return 0;
}

static int
ubus_lua_send(lua_State *L)
{
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);
	const char *event = luaL_checkstring(L, 2);

	if (*event == 0)
		return luaL_argerror(L, 2, "no event name");

	// Event content convert to ubus form
	luaL_checktype(L, 3, LUA_TTABLE);
	blob_buf_init(&c->buf, 0);

	if (!ubus_lua_format_blob_array(L, &c->buf, true)) {
		lua_pushnil(L);
		lua_pushinteger(L, UBUS_STATUS_INVALID_ARGUMENT);
		return 2;
	}

	// Send the event
	ubus_send_event(c->ctx, event, c->buf.head);

	return 0;
}



static int
ubus_lua__gc(lua_State *L)
{
	struct ubus_lua_connection *c = luaL_checkudata(L, 1, METANAME);

	blob_buf_free(&c->buf);
	if (c->ctx != NULL)
	{
		ubus_free(c->ctx);
		memset(c, 0, sizeof(*c));
	}

	return 0;
}

static const luaL_Reg ubus[] = {
	{ "connect", ubus_lua_connect },
	{ "objects", ubus_lua_objects },
	{ "add", ubus_lua_add },
	{ "notify", ubus_lua_notify },
	{ "reply", ubus_lua_reply },
	{ "defer_request", ubus_lua_defer_request },
	{ "complete_deferred_request", ubus_lua_complete_deferred_request },
	{ "signatures", ubus_lua_signatures },
	{ "call", ubus_lua_call },
	{ "call_async", ubus_lua_call_async },
	{ "blob_decode", ubus_lua_blob_decode },
	{ "read_fd", ubus_lua_read_fd },
	{ "close_fd", ubus_lua_close_fd },
	{ "close", ubus_lua__gc },
	{ "listen", ubus_lua_listen },
	{ "send", ubus_lua_send },
	{ "subscribe", ubus_lua_subscribe },
	{ "__gc", ubus_lua__gc },
	{ NULL, NULL },
};

/* avoid missing prototype warning */
int luaopen_ubus(lua_State *L);

int
luaopen_ubus(lua_State *L)
{
	/* create metatable */
	luaL_newmetatable(L, METANAME);

	/* metatable.__index = metatable */
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");

	/* fill metatable */
	luaL_register(L, NULL, ubus);
	lua_pop(L, 1);

	/* create module */
	luaL_register(L, MODNAME, ubus);

	/* set some enum defines */
	lua_pushinteger(L, BLOBMSG_TYPE_ARRAY);
	lua_setfield(L, -2, "ARRAY");
	lua_pushinteger(L, BLOBMSG_TYPE_TABLE);
	lua_setfield(L, -2, "TABLE");
	lua_pushinteger(L, BLOBMSG_TYPE_STRING);
	lua_setfield(L, -2, "STRING");
	lua_pushinteger(L, BLOBMSG_TYPE_INT64);
	lua_setfield(L, -2, "INT64");
	lua_pushinteger(L, BLOBMSG_TYPE_INT32);
	lua_setfield(L, -2, "INT32");
	lua_pushinteger(L, BLOBMSG_TYPE_INT16);
	lua_setfield(L, -2, "INT16");
	lua_pushinteger(L, BLOBMSG_TYPE_INT8);
	lua_setfield(L, -2, "INT8");
	lua_pushinteger(L, BLOBMSG_TYPE_DOUBLE);
	lua_setfield(L, -2, "DOUBLE");
	lua_pushinteger(L, BLOBMSG_TYPE_BOOL);
	lua_setfield(L, -2, "BOOLEAN");

	/* used in our callbacks */
	state = L;

	/* create the callback table */
	lua_createtable(L, 1, 0);
	lua_setglobal(L, "__ubus_cb");

	/* create the event table */
	lua_createtable(L, 1, 0);
	lua_setglobal(L, "__ubus_cb_event");

	/* create the subscriber table */
	lua_createtable(L, 1, 0);
	lua_setglobal(L, "__ubus_cb_subscribe");

	/* create the publisher table - notifications of new subs */
	lua_createtable(L, 1, 0);
	lua_setglobal(L, "__ubus_cb_publisher");

	/* create the async call table - callbacks and their pending results */
	lua_createtable(L, 1, 0);
	lua_setglobal(L, "__ubus_cb_async");
	return 0;
}
