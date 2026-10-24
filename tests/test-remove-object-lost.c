#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libubox/usock.h>

#include "libubus.h"
#include "test-ubusd.h"

static int dummy_handler(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	return 0;
}

static const struct ubus_method test_methods[] = {
	UBUS_METHOD_NOARG("dummy", dummy_handler),
};

static struct ubus_object_type test_type =
	UBUS_OBJECT_TYPE("test_remove_lost", test_methods);

static struct ubus_object_type other_type =
	UBUS_OBJECT_TYPE("test_remove_lost_other", test_methods);

static struct ubus_object *object_new(const char *name)
{
	struct ubus_object *obj = calloc(1, sizeof(*obj));

	if (!obj)
		exit(2);

	obj->name = name;
	obj->type = &test_type;
	obj->methods = test_methods;
	obj->n_methods = ARRAY_SIZE(test_methods);

	return obj;
}

/*
 * The send of the remove request fails. A reconnect to a new ubusd must not
 * register the removed object again: the caller freed it.
 */
static void test_send_failed_then_reconnect(void)
{
	struct ubus_object *removed = object_new("test.removed");
	struct ubus_object *never_added = object_new("test.never");
	struct ubus_object *kept = object_new("test.kept");
	struct ubus_context *ctx;
	int ret;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, removed));
	CHECK(!ubus_add_object(ctx, kept));
	CHECK(test_type.id);

	ubusd_kill();

	ret = ubus_remove_object(ctx, removed);
	CHECK(ret == UBUS_STATUS_INVALID_ARGUMENT);
	CHECK(ctx->sock.eof);
	CHECK(!removed->id);
	CHECK(ctx->objects.count == 1);
	CHECK(!test_type.id);
	free(removed);

	CHECK(ubus_remove_object(ctx, never_added));
	CHECK(ctx->objects.count == 1);
	free(never_added);

	ubusd_start();
	CHECK(!ubus_reconnect(ctx, sock_path));
	CHECK(kept->id);
	CHECK(ctx->objects.count == 1);
	CHECK(object_visible("test.kept"));
	CHECK(!object_visible("test.removed"));

	CHECK(!ubus_remove_object(ctx, kept));
	ubus_free(ctx);
	free(kept);
	ubusd_kill();
}

/*
 * ubusd goes away while the remove request waits for its answer. An object
 * of the same type on a new context and a new ubusd must be visible. The type
 * id goes with the connection, also where another object of the type stays.
 */
static void test_answer_lost_then_new_context(void)
{
	struct ubus_object *removed = object_new("test.removed");
	struct ubus_object *kept = object_new("test.kept");
	struct ubus_object *added = object_new("test.added");
	struct itimerval timer = { .it_value = { .tv_usec = 200 * 1000 } };
	struct ubus_context *ctx;
	int ret;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, removed));
	CHECK(!ubus_add_object(ctx, kept));
	CHECK(test_type.id);

	kill(ubusd_pid, SIGSTOP);
	signal(SIGALRM, ubusd_kill_alarm);
	setitimer(ITIMER_REAL, &timer, NULL);

	ret = ubus_remove_object(ctx, removed);
	CHECK(ret == UBUS_STATUS_CONNECTION_FAILED);
	CHECK(!removed->id);
	CHECK(ctx->objects.count == 1);
	CHECK(!test_type.id);
	ubusd_reap();
	ubus_free(ctx);
	free(removed);
	free(kept);

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, added));
	CHECK(object_visible("test.added"));

	CHECK(!ubus_remove_object(ctx, added));
	ubus_free(ctx);
	free(added);
	ubusd_kill();
}

static int reconnect_ret;
static bool reconnect_ubusd_start;

static void reconnect_connection_lost(struct ubus_context *ctx)
{
	ubusd_reap();
	if (reconnect_ubusd_start)
		ubusd_start();

	reconnect_ret = ubus_reconnect(ctx, sock_path);
}

static int remove_with_reconnect(struct ubus_context *ctx,
				 struct ubus_object *obj, bool ubusd_restart)
{
	struct itimerval timer = { .it_value = { .tv_usec = 200 * 1000 } };

	reconnect_ret = -1;
	reconnect_ubusd_start = ubusd_restart;
	ctx->connection_lost = reconnect_connection_lost;

	kill(ubusd_pid, SIGSTOP);
	signal(SIGALRM, ubusd_kill_alarm);
	setitimer(ITIMER_REAL, &timer, NULL);

	return ubus_remove_object(ctx, obj);
}

/*
 * The connection_lost callback reconnects to a new ubusd while the remove
 * waits. The new ubusd must not get the removed object, and the type keeps
 * the id that the new ubusd gave it for the remaining object.
 */
static void test_answer_lost_reconnect_in_callback(void)
{
	struct ubus_object *removed = object_new("test.removed");
	struct ubus_object *kept = object_new("test.kept");
	struct ubus_context *ctx;
	uint32_t old_type_id;
	int ret;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, removed));
	CHECK(!ubus_add_object(ctx, kept));
	old_type_id = test_type.id;

	ret = remove_with_reconnect(ctx, removed, true);
	CHECK(!reconnect_ret);
	CHECK(ret == UBUS_STATUS_TIMEOUT);
	CHECK(!ctx->sock.eof);
	CHECK(!removed->id);
	CHECK(ctx->objects.count == 1);
	CHECK(kept->id);
	CHECK(test_type.id && test_type.id != old_type_id);
	CHECK(object_visible("test.kept"));
	CHECK(!object_visible("test.removed"));
	free(removed);

	ubusd_kill();
	ubusd_start();
	CHECK(!ubus_reconnect(ctx, sock_path));
	CHECK(ctx->objects.count == 1);
	CHECK(object_visible("test.kept"));
	CHECK(!object_visible("test.removed"));

	CHECK(!ubus_remove_object(ctx, kept));
	ubus_free(ctx);
	free(kept);
	ubusd_kill();
}

/*
 * The connection_lost callback fails to reconnect while the remove waits. A
 * later reconnect must not register the removed object again. Neither an
 * object of the same type nor one of another type keeps the type id of the
 * removed object, as no ubusd knows that id any more.
 */
static void test_answer_lost_reconnect_failed_in_callback(void)
{
	struct ubus_object *removed = object_new("test.removed");
	struct ubus_object *same = object_new("test.same");
	struct ubus_object *other = object_new("test.other");
	struct ubus_context *ctx;
	int ret;

	other->type = &other_type;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, removed));
	CHECK(!ubus_add_object(ctx, same));
	CHECK(!ubus_add_object(ctx, other));
	CHECK(test_type.id);

	ret = remove_with_reconnect(ctx, removed, false);
	CHECK(reconnect_ret == UBUS_STATUS_CONNECTION_FAILED);
	CHECK(ret == UBUS_STATUS_TIMEOUT);
	CHECK(!ctx->sock.eof);
	CHECK(!removed->id);
	CHECK(ctx->objects.count == 2);
	CHECK(!test_type.id);
	free(removed);

	ubusd_start();
	CHECK(!ubus_reconnect(ctx, sock_path));
	CHECK(ctx->objects.count == 2);
	CHECK(object_visible("test.same"));
	CHECK(object_visible("test.other"));
	CHECK(!object_visible("test.removed"));

	CHECK(!ubus_remove_object(ctx, same));
	CHECK(!ubus_remove_object(ctx, other));
	ubus_free(ctx);
	free(same);
	free(other);
	ubusd_kill();
}

static struct ubus_object *nested_obj;
static int nested_ret;

static void nested_remove_complete(struct ubus_request *req, int ret)
{
	nested_ret = ubus_remove_object(req->ctx, nested_obj);
}

static void nested_remove_connection_lost(struct ubus_context *ctx)
{
	ubusd_reap();
	nested_ret = ubus_remove_object(ctx, nested_obj);
}

/*
 * The answer to an async request waits in the socket, so the next wait of
 * the context reads it and its complete_cb removes the object again.
 */
static void nested_remove_arm(struct ubus_context *ctx, struct ubus_object *obj,
			      struct ubus_request *req)
{
	static struct blob_buf b;

	nested_obj = obj;
	nested_ret = -1;

	blob_buf_init(&b, 0);
	CHECK(!ubus_invoke_async(ctx, UBUS_SYSTEM_OBJECT_EVENT, "send", b.head, req));
	req->complete_cb = nested_remove_complete;
	ubus_complete_request_async(ctx, req);
	usleep(200 * 1000);
}

/*
 * A handler that runs during the wait of a remove removes the same object
 * again. ubusd handled the first remove, so the second one gets NOT_FOUND.
 */
static void test_nested_remove(void)
{
	struct ubus_object *removed = object_new("test.removed");
	struct ubus_object *kept = object_new("test.kept");
	struct ubus_request req;
	struct ubus_context *ctx;
	int ret;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, removed));
	CHECK(!ubus_add_object(ctx, kept));

	nested_remove_arm(ctx, removed, &req);
	ret = ubus_remove_object(ctx, removed);
	CHECK(!ret);
	CHECK(nested_ret == UBUS_STATUS_NOT_FOUND);
	CHECK(!removed->id);
	CHECK(ctx->objects.count == 1);
	CHECK(test_type.id);
	CHECK(object_visible("test.kept"));
	CHECK(!object_visible("test.removed"));
	free(removed);

	CHECK(!ubus_remove_object(ctx, kept));
	CHECK(!ctx->objects.count);
	ubus_free(ctx);
	free(kept);
	ubusd_kill();
}

/*
 * ubusd refuses both removes of a nested remove. The object goes back into
 * ctx->objects once.
 */
static void test_nested_remove_refused(void)
{
	struct ubus_object *refused = object_new("test.refused");
	struct ubus_object *kept = object_new("test.kept");
	struct ubus_request req;
	struct ubus_context *ctx;
	int ret;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, kept));

	/* an id that ubusd does not know, so that it refuses the remove */
	refused->id = kept->id ^ 0x40000000;
	refused->avl.key = &refused->id;
	CHECK(!avl_insert(&ctx->objects, &refused->avl));

	nested_remove_arm(ctx, refused, &req);
	ret = ubus_remove_object(ctx, refused);
	CHECK(ret == UBUS_STATUS_NOT_FOUND);
	CHECK(nested_ret == UBUS_STATUS_NOT_FOUND);
	CHECK(refused->id == (kept->id ^ 0x40000000));
	CHECK(ctx->objects.count == 2);
	CHECK(avl_find(&ctx->objects, &refused->id) == &refused->avl);
	CHECK(avl_find(&ctx->objects, &kept->id) == &kept->avl);

	avl_delete(&ctx->objects, &refused->avl);
	CHECK(!ubus_remove_object(ctx, kept));
	CHECK(!ctx->objects.count);
	ubus_free(ctx);
	free(refused);
	free(kept);
	ubusd_kill();
}

/*
 * The connection_lost callback removes the object that the remove waits for.
 * The send fails, and libubus forgets the object.
 */
static void test_nested_remove_connection_lost(void)
{
	struct ubus_object *removed = object_new("test.removed");
	struct itimerval timer = { .it_value = { .tv_usec = 200 * 1000 } };
	struct ubus_context *ctx;
	int ret;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, removed));

	nested_obj = removed;
	nested_ret = -1;
	ctx->connection_lost = nested_remove_connection_lost;

	kill(ubusd_pid, SIGSTOP);
	signal(SIGALRM, ubusd_kill_alarm);
	setitimer(ITIMER_REAL, &timer, NULL);

	ret = ubus_remove_object(ctx, removed);
	CHECK(ret == UBUS_STATUS_CONNECTION_FAILED);
	CHECK(nested_ret == UBUS_STATUS_INVALID_ARGUMENT);
	CHECK(!removed->id);
	CHECK(!ctx->objects.count);
	CHECK(!test_type.id);
	free(removed);

	ubus_free(ctx);
}

/* An error answer on a live connection leaves the object registered. */
static void test_error_answer_keeps_object(void)
{
	struct ubus_object *obj = object_new("test.kept");
	struct ubus_object *never_added = object_new("test.never");
	struct ubus_context *ctx;
	uint32_t id;
	int ret;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, obj));
	id = obj->id;

	/* The tree holds one node, so changing its key keeps it consistent. */
	obj->id = id ^ 0x40000000;
	ret = ubus_remove_object(ctx, obj);
	CHECK(ret == UBUS_STATUS_NOT_FOUND);
	CHECK(!ctx->sock.eof);
	CHECK(obj->id == (id ^ 0x40000000));
	CHECK(ctx->objects.count == 1);
	CHECK(test_type.id);

	CHECK(ubus_remove_object(ctx, never_added) == UBUS_STATUS_NOT_FOUND);
	CHECK(ctx->objects.count == 1);

	obj->id = id;
	CHECK(!ubus_remove_object(ctx, obj));
	CHECK(!ctx->objects.count);
	ubus_free(ctx);
	free(never_added);
	free(obj);
	ubusd_kill();
}

/*
 * On a live connection ubusd frees the type with its last object, and the
 * type id goes with it.
 */
static void test_live_remove_type_id(void)
{
	struct ubus_object *first = object_new("test.first");
	struct ubus_object *last = object_new("test.last");
	struct ubus_context *ctx;
	uint32_t type_id;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, first));
	CHECK(!ubus_add_object(ctx, last));
	type_id = test_type.id;
	CHECK(type_id);

	CHECK(!ubus_remove_object(ctx, first));
	CHECK(!first->id);
	CHECK(ctx->objects.count == 1);
	CHECK(test_type.id == type_id);

	CHECK(!ubus_remove_object(ctx, last));
	CHECK(!last->id);
	CHECK(!ctx->objects.count);
	CHECK(!test_type.id);

	ubus_free(ctx);
	free(first);
	free(last);
	ubusd_kill();
}

/*
 * The context is freed while it still holds an object. ubusd drops the
 * object and its type with the connection, so an object of the same type
 * on a new context must be visible.
 */
static void test_free_with_object_then_new_context(void)
{
	struct ubus_object *held = object_new("test.held");
	struct ubus_object *added = object_new("test.added");
	struct ubus_context *ctx;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, held));
	CHECK(test_type.id);

	ubus_free(ctx);

	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, added));
	CHECK(test_type.id);
	CHECK(object_visible("test.added"));

	CHECK(!ubus_remove_object(ctx, added));
	ubus_free(ctx);
	free(added);
	free(held);
	ubusd_kill();
}

/*
 * ubusd refuses a type id it does not know, and libubus registers the type
 * again from its signature. Another refusal keeps the type id.
 */
static void test_unknown_type_id(void)
{
	struct ubus_object *obj = object_new("test.unknown_type");
	struct ubus_object *dup = object_new("test.unknown_type");
	struct ubus_context *ctx;
	uint32_t type_id;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	test_type.id = 0x7ffffffe;
	CHECK(!ubus_add_object(ctx, obj));
	CHECK(obj->id);
	CHECK(test_type.id && test_type.id != 0x7ffffffe);
	CHECK(object_visible("test.unknown_type"));

	type_id = test_type.id;
	CHECK(ubus_add_object(ctx, dup) == UBUS_STATUS_INVALID_ARGUMENT);
	CHECK(!dup->id);
	CHECK(test_type.id == type_id);

	CHECK(!ubus_remove_object(ctx, obj));
	ubus_free(ctx);
	free(dup);
	free(obj);
	ubusd_kill();
}

/*
 * ubusd refuses the unknown type id, and then refuses the add with the
 * signature for a duplicate path. The add returns the second refusal.
 */
static void test_unknown_type_id_retry_refused(void)
{
	struct ubus_object *obj = object_new("test.retry_refused");
	struct ubus_object *dup = object_new("test.retry_refused");
	struct ubus_context *ctx;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, obj));
	CHECK(test_type.id);

	test_type.id = 0x7ffffffe;
	CHECK(ubus_add_object(ctx, dup) == UBUS_STATUS_INVALID_ARGUMENT);
	CHECK(!dup->id);
	CHECK(!test_type.id);
	CHECK(ctx->objects.count == 1);
	CHECK(object_visible("test.retry_refused"));

	CHECK(!ubus_remove_object(ctx, obj));
	ubus_free(ctx);
	free(dup);
	free(obj);
	ubusd_kill();
}

static const struct ubus_method unnamed_methods[] = {
	UBUS_METHOD_NOARG("", dummy_handler),
};

static struct ubus_object_type unnamed_type =
	UBUS_OBJECT_TYPE("test_remove_lost_unnamed", unnamed_methods);

/*
 * ubusd cannot register a type whose signature holds a method without a
 * name. The add fails, and no object is left that a lookup cannot find.
 */
static void test_signature_refused(void)
{
	struct ubus_object *obj = object_new("test.unnamed");
	struct ubus_context *ctx;
	uint32_t id;

	obj->type = &unnamed_type;
	obj->methods = unnamed_methods;
	obj->n_methods = ARRAY_SIZE(unnamed_methods);

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(ubus_add_object(ctx, obj) == UBUS_STATUS_INVALID_ARGUMENT);
	CHECK(!obj->id);
	CHECK(!unnamed_type.id);
	CHECK(!ctx->objects.count);
	CHECK(ubus_lookup_id(ctx, "test.unnamed", &id) == UBUS_STATUS_NOT_FOUND);

	ubus_free(ctx);
	free(obj);
	ubusd_kill();
}

/*
 * ubusd refuses the remove of an object that another context of the same
 * process registered. Neither tree changes.
 */
static void test_remove_other_context(void)
{
	struct ubus_object *obj = object_new("test.other_ctx");
	struct ubus_object *kept = object_new("test.kept");
	struct ubus_context *ctx_a, *ctx_b;
	uint32_t id;

	ubusd_start();
	ctx_a = ubus_connect(sock_path);
	ctx_b = ubus_connect(sock_path);
	CHECK(ctx_a && ctx_b);
	if (!ctx_a || !ctx_b)
		return;

	CHECK(!ubus_add_object(ctx_a, obj));
	CHECK(!ubus_add_object(ctx_b, kept));
	id = obj->id;

	CHECK(ubus_remove_object(ctx_b, obj) == UBUS_STATUS_PERMISSION_DENIED);
	CHECK(obj->id == id);
	CHECK(ctx_a->objects.count == 1);
	CHECK(avl_find(&ctx_a->objects, &obj->id) == &obj->avl);
	CHECK(ctx_b->objects.count == 1);
	CHECK(avl_find(&ctx_b->objects, &kept->id) == &kept->avl);
	CHECK(object_visible("test.other_ctx"));

	CHECK(!ubus_remove_object(ctx_a, obj));
	CHECK(!ctx_a->objects.count);
	CHECK(!ubus_remove_object(ctx_b, kept));
	ubus_free(ctx_a);
	ubus_free(ctx_b);
	free(obj);
	free(kept);
	ubusd_kill();
}

/*
 * Another node in ctx->objects holds the id of the object that is removed.
 * The remove must leave that node alone.
 */
static void test_remove_id_collision(void)
{
	struct ubus_object *kept = object_new("test.kept");
	struct ubus_object *holder = object_new("test.holder");
	struct ubus_object *removed = object_new("test.removed");
	struct ubus_context *ctx;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, kept));

	/* an id that ubusd does not know, so that it refuses the remove */
	holder->id = kept->id ^ 0x40000000;
	holder->avl.key = &holder->id;
	CHECK(!avl_insert(&ctx->objects, &holder->avl));
	removed->id = holder->id;

	CHECK(ubus_remove_object(ctx, removed) == UBUS_STATUS_NOT_FOUND);
	CHECK(removed->id == holder->id);
	CHECK(ctx->objects.count == 2);
	CHECK(avl_find(&ctx->objects, &holder->id) == &holder->avl);
	CHECK(avl_find(&ctx->objects, &kept->id) == &kept->avl);

	avl_delete(&ctx->objects, &holder->avl);
	CHECK(!ubus_remove_object(ctx, kept));
	CHECK(!ctx->objects.count);
	ubus_free(ctx);
	free(kept);
	free(holder);
	free(removed);
	ubusd_kill();
}

static void refusal_then_eof_complete(struct ubus_request *req, int ret)
{
	usleep(200 * 1000);
	ubusd_kill();
	ubus_handle_event(req->ctx);
	ubus_handle_event(req->ctx);
}

/*
 * A callback during the wait reads ubusd's refusal of the remove, and then
 * the connection ends. The wait ends with CONNECTION_FAILED, and libubus
 * forgets the object as on a lost connection.
 */
static void test_refusal_then_eof(void)
{
	struct ubus_object *kept = object_new("test.kept");
	struct ubus_object *refused = object_new("test.refused");
	struct ubus_request req;
	struct ubus_context *ctx;
	static struct blob_buf b;
	int ret;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_add_object(ctx, kept));

	/* an id that ubusd does not know, so that it refuses the remove */
	refused->id = kept->id ^ 0x40000000;
	refused->avl.key = &refused->id;
	CHECK(!avl_insert(&ctx->objects, &refused->avl));

	blob_buf_init(&b, 0);
	CHECK(!ubus_invoke_async(ctx, UBUS_SYSTEM_OBJECT_EVENT, "send", b.head, &req));
	req.complete_cb = refusal_then_eof_complete;
	ubus_complete_request_async(ctx, &req);
	usleep(200 * 1000);

	ret = ubus_remove_object(ctx, refused);
	CHECK(ret == UBUS_STATUS_CONNECTION_FAILED);
	CHECK(ctx->sock.eof);
	CHECK(!refused->id);
	CHECK(ctx->objects.count == 1);
	CHECK(avl_find(&ctx->objects, &kept->id) == &kept->avl);
	CHECK(!test_type.id);

	ubus_free(ctx);
	free(kept);
	free(refused);
}

static int queued_reconnect_ret;

static int queued_reconnect_handler(struct ubus_context *ctx,
				    struct ubus_object *obj,
				    struct ubus_request_data *req,
				    const char *method, struct blob_attr *msg)
{
	ubusd_kill();
	ubusd_start();
	queued_reconnect_ret = ubus_reconnect(ctx, sock_path);
	return 0;
}

static const struct ubus_method reconnect_methods[] = {
	UBUS_METHOD_NOARG("reconnect", queued_reconnect_handler),
};

static struct ubus_object_type reconnect_type =
	UBUS_OBJECT_TYPE("test_remove_lost_reconnect", reconnect_methods);

/*
 * ubusd refuses the remove. An INVOKE queued during the wait runs at the end
 * of the wait and reconnects to a new ubusd, which must get the object.
 */
static void test_refused_remove_reconnect_in_queued_handler(void)
{
	struct ubus_object *trigger = object_new("test.trigger");
	struct ubus_object *refused = object_new("test.refused");
	struct ubus_context *ctx, *tx;
	struct ubus_request req;
	static struct blob_buf b;
	uint32_t old_id;
	int ret;

	trigger->type = &reconnect_type;
	trigger->methods = reconnect_methods;
	trigger->n_methods = ARRAY_SIZE(reconnect_methods);

	ubusd_start();
	ctx = ubus_connect(sock_path);
	tx = ubus_connect(sock_path);
	CHECK(ctx && tx);
	if (!ctx || !tx)
		return;

	CHECK(!ubus_add_object(ctx, trigger));

	/* an id that ubusd does not know, so that it refuses the remove */
	refused->id = trigger->id ^ 0x40000000;
	refused->avl.key = &refused->id;
	CHECK(!avl_insert(&ctx->objects, &refused->avl));
	old_id = refused->id;

	queued_reconnect_ret = -1;
	blob_buf_init(&b, 0);
	CHECK(!ubus_invoke_async(tx, trigger->id, "reconnect", b.head, &req));
	usleep(200 * 1000);

	ret = ubus_remove_object(ctx, refused);
	CHECK(ret == UBUS_STATUS_NOT_FOUND);
	CHECK(!queued_reconnect_ret);
	CHECK(refused->id && refused->id != old_id);
	CHECK(avl_find(&ctx->objects, &refused->id) == &refused->avl);
	CHECK(ctx->objects.count == 2);
	CHECK(object_visible("test.refused"));
	CHECK(object_visible("test.trigger"));

	ubus_free(tx);
	CHECK(!ubus_remove_object(ctx, refused));
	CHECK(!ubus_remove_object(ctx, trigger));
	CHECK(!ctx->objects.count);
	ubus_free(ctx);
	free(trigger);
	free(refused);
	ubusd_kill();
}

static void fake_ubusd_send(int fd, uint8_t type, uint16_t seq, uint32_t peer,
			    struct blob_attr *msg)
{
	struct ubus_msghdr hdr = {
		.type = type,
		.seq = seq,
		.peer = peer,
	};
	struct iovec iov[2] = {
		{ .iov_base = &hdr, .iov_len = sizeof(hdr) },
		{ .iov_base = msg, .iov_len = blob_raw_len(msg) },
	};
	ssize_t len = iov[0].iov_len + iov[1].iov_len;

	/* libubus reads the header and the first attribute in one read() */
	if (writev(fd, iov, 2) != len)
		_exit(3);
}

static bool fake_ubusd_read_full(int fd, void *buf, size_t len)
{
	ssize_t ret;

	while (len) {
		ret = read(fd, buf, len);
		if (ret <= 0)
			return false;

		buf = (char *) buf + ret;
		len -= ret;
	}

	return true;
}

static struct blob_attr *fake_ubusd_read(int fd, struct ubus_msghdr *hdr)
{
	static uint32_t data[UBUS_MAX_MSGLEN / sizeof(uint32_t)];
	struct blob_attr *msg = (struct blob_attr *) data;
	size_t len;

	if (!fake_ubusd_read_full(fd, hdr, sizeof(*hdr)) ||
	    !fake_ubusd_read_full(fd, msg, sizeof(*msg)))
		return NULL;

	len = blob_raw_len(msg);
	if (len < sizeof(*msg) || len > sizeof(data))
		return NULL;

	if (!fake_ubusd_read_full(fd, msg + 1, len - sizeof(*msg)))
		return NULL;

	return msg;
}

static void fake_ubusd_invoke(int fd, uint32_t objid, const char *method)
{
	struct blob_buf b = {};

	blob_buf_init(&b, 0);
	blob_put_int32(&b, UBUS_ATTR_OBJID, objid);
	blob_put_string(&b, UBUS_ATTR_METHOD, method);
	blob_put(&b, UBUS_ATTR_DATA, NULL, 0);
	fake_ubusd_send(fd, UBUS_MSG_INVOKE, 0, 0, b.head);
	blob_buf_free(&b);
}

static void fake_ubusd_conn(int fd, uint32_t *next_id, bool invoke,
			    bool remove_data)
{
	struct blob_buf b = {};
	struct ubus_msghdr hdr;
	struct blob_attr *msg;
	uint32_t id;

	blob_buf_init(&b, 0);
	fake_ubusd_send(fd, UBUS_MSG_HELLO, 0, cpu_to_be32(0x12345678), b.head);

	while ((msg = fake_ubusd_read(fd, &hdr))) {
		if (hdr.type == UBUS_MSG_STATUS)
			continue;

		if (hdr.type == UBUS_MSG_ADD_OBJECT) {
			id = (*next_id)++;
			blob_buf_init(&b, 0);
			blob_put_int32(&b, UBUS_ATTR_OBJID, id);
			blob_put_int32(&b, UBUS_ATTR_OBJTYPE, 0x2000);
			fake_ubusd_send(fd, UBUS_MSG_DATA, hdr.seq, hdr.peer, b.head);

			if (invoke)
				fake_ubusd_invoke(fd, id, "remove");
			invoke = false;
		}

		/* the request holds the id that the answer must name */
		if (hdr.type == UBUS_MSG_REMOVE_OBJECT && remove_data)
			fake_ubusd_send(fd, UBUS_MSG_DATA, hdr.seq, hdr.peer, msg);

		blob_buf_init(&b, 0);
		blob_put_int32(&b, UBUS_ATTR_STATUS, 0);
		fake_ubusd_send(fd, UBUS_MSG_STATUS, hdr.seq, hdr.peer, b.head);
	}

	blob_buf_free(&b);
}

/*
 * A ubusd that gives an id to every add and answers every request with
 * status 0. With remove_data, it answers a remove with the id of the object,
 * as ubusd does; else with no DATA. On each connection after the first, it
 * calls the "remove" method of the first object added there before that add
 * completes. The seq and peer fields go back unchanged, in network byte
 * order.
 */
static void fake_ubusd_serve(int listen_fd, bool remove_data)
{
	uint32_t next_id = 0x1000;
	bool invoke = false;
	int fd;

	while ((fd = accept(listen_fd, NULL, NULL)) >= 0) {
		fake_ubusd_conn(fd, &next_id, invoke, remove_data);
		close(fd);
		invoke = true;
	}

	_exit(3);
}

static pid_t fake_ubusd_start(bool remove_data)
{
	int listen_fd;
	pid_t pid;

	unlink(sock_path);
	listen_fd = usock(USOCK_UNIX | USOCK_SERVER, sock_path, NULL);
	if (listen_fd < 0)
		return -1;

	pid = fork();
	if (pid < 0)
		exit(2);

	if (!pid) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		fake_ubusd_serve(listen_fd, remove_data);
	}

	close(listen_fd);

	return pid;
}

static void fake_ubusd_kill(pid_t pid)
{
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	unlink(sock_path);
}

/*
 * ubusd answers the remove with status 0 but with no id. libubus keeps the
 * object registered and returns NO_DATA.
 */
static void test_remove_answer_without_data(void)
{
	struct ubus_object *obj = object_new("test.no_data");
	struct ubus_context *ctx;
	pid_t pid;

	pid = fake_ubusd_start(false);
	CHECK(pid > 0);
	if (pid < 0)
		return;

	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (ctx) {
		CHECK(!ubus_add_object(ctx, obj));
		CHECK(obj->id == 0x1000);

		CHECK(ubus_remove_object(ctx, obj) == UBUS_STATUS_NO_DATA);
		CHECK(obj->id == 0x1000);
		CHECK(ctx->objects.count == 1);
		CHECK(avl_find(&ctx->objects, &obj->id) == &obj->avl);
		CHECK(test_type.id == 0x2000);

		avl_delete(&ctx->objects, &obj->avl);
		ubus_free(ctx);
	}

	fake_ubusd_kill(pid);
	test_type.id = 0;
	free(obj);
}

static struct ubus_object *readd_victim;
static uint32_t readd_victim_id;
static int readd_remove_ret;

static int readd_remove_handler(struct ubus_context *ctx,
				struct ubus_object *obj,
				struct ubus_request_data *req,
				const char *method, struct blob_attr *msg)
{
	readd_victim_id = readd_victim->id;
	readd_remove_ret = ubus_remove_object(ctx, readd_victim);
	return 0;
}

static const struct ubus_method readd_methods[] = {
	UBUS_METHOD_NOARG("remove", readd_remove_handler),
};

static struct ubus_object_type readd_type =
	UBUS_OBJECT_TYPE("test_remove_lost_readd", readd_methods);

/*
 * The new ubusd calls an object while the reconnect adds the objects again,
 * and the handler removes another object. The handler runs once every object
 * is back, and the object it removed stays removed.
 */
static void test_reconnect_handler_removes_object(void)
{
	struct ubus_object *trigger = object_new("test.trigger");
	struct ubus_object *victim = object_new("test.victim");
	struct ubus_context *ctx;
	pid_t pid;

	trigger->type = &readd_type;
	trigger->methods = readd_methods;
	trigger->n_methods = ARRAY_SIZE(readd_methods);

	pid = fake_ubusd_start(true);
	CHECK(pid > 0);
	if (pid < 0)
		return;

	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (ctx) {
		/* the fake gives ascending ids, so the reconnect adds trigger first */
		CHECK(!ubus_add_object(ctx, trigger));
		CHECK(!ubus_add_object(ctx, victim));

		readd_victim = victim;
		readd_victim_id = 0;
		readd_remove_ret = -1;
		CHECK(!ubus_reconnect(ctx, sock_path));
		CHECK(readd_victim_id);
		CHECK(!readd_remove_ret);
		CHECK(!victim->id);
		CHECK(ctx->objects.count == 1);
		CHECK(avl_find(&ctx->objects, &trigger->id) == &trigger->avl);

		ubus_free(ctx);
	}

	fake_ubusd_kill(pid);
	test_type.id = 0;
	readd_type.id = 0;
	free(trigger);
	free(victim);
}

int main(int argc, char **argv)
{
	int ret;

	ret = test_ubusd_init(argc, argv);
	if (ret)
		return ret;

	test_live_remove_type_id();
	test_error_answer_keeps_object();
	test_send_failed_then_reconnect();
	test_answer_lost_then_new_context();
	test_answer_lost_reconnect_in_callback();
	test_answer_lost_reconnect_failed_in_callback();
	test_nested_remove();
	test_nested_remove_refused();
	test_nested_remove_connection_lost();
	test_free_with_object_then_new_context();
	test_unknown_type_id();
	test_unknown_type_id_retry_refused();
	test_signature_refused();
	test_remove_other_context();
	test_remove_id_collision();
	test_refusal_then_eof();
	test_refused_remove_reconnect_in_queued_handler();
	test_remove_answer_without_data();
	test_reconnect_handler_removes_object();

	return test_ubusd_done();
}
