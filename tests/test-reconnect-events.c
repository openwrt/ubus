#define _GNU_SOURCE
#include <poll.h>
#include <string.h>

#include "libubus.h"
#include "test-ubusd.h"

struct counted_handler {
	struct ubus_event_handler ev;
	int count_old;
	int count_new;
	int count_other;
};

static void counted_cb(struct ubus_context *ctx, struct ubus_event_handler *ev,
		       const char *type, struct blob_attr *msg)
{
	struct counted_handler *h = container_of(ev, struct counted_handler, ev);

	if (!strcmp(type, "test.old"))
		h->count_old++;
	else if (!strcmp(type, "test.new"))
		h->count_new++;
	else
		h->count_other++;
}

static void counted_init(struct counted_handler *h)
{
	memset(h, 0, sizeof(*h));
	h->ev.cb = counted_cb;
}

static void events_read(struct ubus_context *ctx)
{
	struct pollfd pfd = { .fd = ctx->sock.fd, .events = POLLIN };

	while (poll(&pfd, 1, 200) > 0)
		ubus_handle_event(ctx);
}

/* Another client sends the event, and ctx reads what ubusd passes on. */
static void event_send(struct ubus_context *ctx, const char *type)
{
	struct ubus_context *tx = ubus_connect(sock_path);
	struct blob_buf b = {};

	CHECK(tx);
	if (!tx)
		return;

	blob_buf_init(&b, 0);
	CHECK(!ubus_send_event(tx, type, b.head));
	ubus_free(tx);
	blob_buf_free(&b);

	events_read(ctx);
}

static void ubusd_restart(struct ubus_context *ctx)
{
	ubusd_kill();
	ubusd_start();
	CHECK(!ubus_reconnect(ctx, sock_path));
}

/* A handler with two patterns gets both events after a reconnect. */
static void test_patterns_after_reconnect(void)
{
	struct counted_handler h;
	struct ubus_context *ctx;

	counted_init(&h);
	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_register_event_handler(ctx, &h.ev, "test.old"));
	CHECK(!ubus_register_event_handler(ctx, &h.ev, "test.new"));
	CHECK(!ubus_register_event_handler(ctx, &h.ev, "test.new"));

	ubusd_restart(ctx);
	CHECK(h.ev.obj.id);
	event_send(ctx, "test.old");
	event_send(ctx, "test.new");
	event_send(ctx, "test.other");
	CHECK(h.count_old == 1);
	CHECK(h.count_new == 1);
	CHECK(!h.count_other);

	CHECK(!ubus_unregister_event_handler(ctx, &h.ev));
	ubus_free(ctx);
	ubusd_kill();
}

/*
 * The handler object is removed, then added again by the caller and given
 * another pattern. A reconnect registers only the new pattern.
 */
static void test_removed_handler_patterns_dropped(void)
{
	struct counted_handler h;
	struct ubus_context *ctx;

	counted_init(&h);
	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_register_event_handler(ctx, &h.ev, "test.old"));
	CHECK(!ubus_unregister_event_handler(ctx, &h.ev));
	CHECK(!h.ev.obj.id);

	CHECK(!ubus_add_object(ctx, &h.ev.obj));
	CHECK(!ubus_register_event_handler(ctx, &h.ev, "test.new"));

	ubusd_restart(ctx);
	event_send(ctx, "test.old");
	event_send(ctx, "test.new");
	CHECK(!h.count_old);
	CHECK(h.count_new == 1);

	CHECK(!ubus_unregister_event_handler(ctx, &h.ev));
	ubus_free(ctx);
	ubusd_kill();
}

/*
 * The remove of the handler object finds the connection lost. The object is
 * forgotten with its pattern, and a later reconnect does not register it.
 */
static void test_lost_handler_patterns_dropped(void)
{
	struct counted_handler h;
	struct ubus_context *ctx;

	counted_init(&h);
	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_register_event_handler(ctx, &h.ev, "test.old"));
	ubusd_kill();
	CHECK(ubus_unregister_event_handler(ctx, &h.ev) ==
	      UBUS_STATUS_INVALID_ARGUMENT);
	CHECK(!h.ev.obj.id);
	CHECK(!ctx->objects.count);

	ubusd_start();
	CHECK(!ubus_reconnect(ctx, sock_path));
	CHECK(!ubus_add_object(ctx, &h.ev.obj));
	CHECK(!ubus_register_event_handler(ctx, &h.ev, "test.new"));

	ubusd_restart(ctx);
	event_send(ctx, "test.old");
	event_send(ctx, "test.new");
	CHECK(!h.count_old);
	CHECK(h.count_new == 1);

	CHECK(!ubus_unregister_event_handler(ctx, &h.ev));
	ubus_free(ctx);
	ubusd_kill();
}

/*
 * Two contexts of one process each hold a handler. ubus_free() of one
 * context leaves the patterns of the other, and a context that is shut down
 * and connected again starts with no patterns.
 */
static void test_contexts_apart(void)
{
	static struct ubus_context reused;
	struct counted_handler h1, h2, h3;
	struct ubus_context *ctx1, *ctx2;

	counted_init(&h1);
	counted_init(&h2);
	counted_init(&h3);
	ubusd_start();
	ctx1 = ubus_connect(sock_path);
	ctx2 = ubus_connect(sock_path);
	CHECK(ctx1 && ctx2);
	if (!ctx1 || !ctx2)
		return;

	CHECK(!ubus_register_event_handler(ctx1, &h1.ev, "test.old"));
	CHECK(!ubus_register_event_handler(ctx2, &h2.ev, "test.new"));
	ubus_free(ctx2);

	CHECK(!ubus_connect_ctx(&reused, sock_path));
	CHECK(!ubus_register_event_handler(&reused, &h3.ev, "test.old"));
	ubus_shutdown(&reused);
	CHECK(!ubus_connect_ctx(&reused, sock_path));
	h3.ev.obj.id = 0;
	CHECK(!ubus_add_object(&reused, &h3.ev.obj));
	CHECK(!ubus_register_event_handler(&reused, &h3.ev, "test.new"));

	ubusd_kill();
	ubusd_start();
	CHECK(!ubus_reconnect(ctx1, sock_path));
	CHECK(!ubus_reconnect(&reused, sock_path));

	event_send(ctx1, "test.old");
	events_read(&reused);
	event_send(ctx1, "test.new");
	events_read(&reused);
	CHECK(h1.count_old == 1);
	CHECK(!h1.count_new);
	CHECK(!h2.count_old && !h2.count_new);
	CHECK(!h3.count_old);
	CHECK(h3.count_new == 1);

	CHECK(!ubus_unregister_event_handler(ctx1, &h1.ev));
	CHECK(!ubus_unregister_event_handler(&reused, &h3.ev));
	ubus_free(ctx1);
	ubus_shutdown(&reused);
	ubusd_kill();
}

static int new_obj_count;

static bool auto_new_obj_cb(struct ubus_context *ctx, struct ubus_subscriber *sub,
			    const char *path)
{
	if (!strcmp(path, "test.late"))
		new_obj_count++;

	return false;
}

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
	UBUS_OBJECT_TYPE("test_reconnect_events", test_methods);

/*
 * The new ubusd holds the path of a named handler already, so the reconnect
 * cannot add it again. The handler loses its pattern with its id.
 */
static void test_failed_readd_patterns_dropped(void)
{
	struct ubus_object holder = {
		.name = "test.handler",
		.type = &test_type,
		.methods = test_methods,
		.n_methods = ARRAY_SIZE(test_methods),
	};
	struct counted_handler h;
	struct ubus_context *ctx, *tx;

	counted_init(&h);
	h.ev.obj.name = "test.handler";
	h.ev.obj.type = &test_type;
	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_register_event_handler(ctx, &h.ev, "test.old"));

	ubusd_kill();
	ubusd_start();
	tx = ubus_connect(sock_path);
	CHECK(tx);
	if (!tx)
		return;

	CHECK(!ubus_add_object(tx, &holder));
	CHECK(!ubus_reconnect(ctx, sock_path));
	CHECK(!h.ev.obj.id);
	CHECK(!ctx->objects.count);
	CHECK(!ubus_remove_object(tx, &holder));
	ubus_free(tx);

	CHECK(!ubus_add_object(ctx, &h.ev.obj));
	CHECK(!ubus_register_event_handler(ctx, &h.ev, "test.new"));

	ubusd_restart(ctx);
	event_send(ctx, "test.old");
	event_send(ctx, "test.new");
	CHECK(!h.count_old);
	CHECK(h.count_new == 1);

	CHECK(!ubus_unregister_event_handler(ctx, &h.ev));
	ubus_free(ctx);
	ubusd_kill();
}

/* An auto subscriber learns of an object added after a reconnect. */
static void test_auto_subscriber_after_reconnect(void)
{
	struct ubus_subscriber sub = { .new_obj_cb = auto_new_obj_cb };
	struct ubus_object late = {
		.name = "test.late",
		.type = &test_type,
		.methods = test_methods,
		.n_methods = ARRAY_SIZE(test_methods),
	};
	struct pollfd pfd = { .events = POLLIN };
	struct ubus_context *ctx, *tx;

	ubusd_start();
	ctx = ubus_connect(sock_path);
	CHECK(ctx);
	if (!ctx)
		return;

	CHECK(!ubus_register_subscriber(ctx, &sub));
	ubusd_restart(ctx);
	CHECK(ctx->auto_subscribe_event_handler.obj.id);

	new_obj_count = 0;
	tx = ubus_connect(sock_path);
	CHECK(tx);
	if (tx) {
		CHECK(!ubus_add_object(tx, &late));
		pfd.fd = ctx->sock.fd;
		while (poll(&pfd, 1, 200) > 0)
			ubus_handle_event(ctx);
		ubus_free(tx);
	}
	CHECK(new_obj_count == 1);

	ubus_free(ctx);
	ubusd_kill();
}

int main(int argc, char **argv)
{
	int ret;

	ret = test_ubusd_init(argc, argv);
	if (ret)
		return ret;

	test_patterns_after_reconnect();
	test_removed_handler_patterns_dropped();
	test_lost_handler_patterns_dropped();
	test_contexts_apart();
	test_failed_readd_patterns_dropped();
	test_auto_subscriber_after_reconnect();

	return test_ubusd_done();
}
