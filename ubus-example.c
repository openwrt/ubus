#include <unistd.h>

#include "libubus.h"

static struct ubus_context *ctx;
struct blob_buf b;

static const struct ubus_signature test_object_sig[] = {
	UBUS_METHOD_START("hello"),
	  UBUS_ARRAY("test"),
		UBUS_TABLE_START(NULL),
		  UBUS_FIELD(INT32, "id"),
		  UBUS_FIELD(STRING, "msg"),
		UBUS_TABLE_END(),
	UBUS_METHOD_END(),
};

static struct ubus_object_type test_object_type =
	UBUS_OBJECT_TYPE("test", test_object_sig);

enum {
	HELLO_ID,
	HELLO_MSG,
	HELLO_LAST
};

static const struct blobmsg_policy hello_policy[] = {
	[HELLO_ID] = { .name = "id", .type = BLOBMSG_TYPE_INT32 },
	[HELLO_MSG] = { .name = "msg", .type = BLOBMSG_TYPE_STRING },
};

static int test_hello(struct ubus_context *ctx, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
	struct blob_attr *tb[HELLO_LAST];
	char *msgstr = "(unknown)";
	char *strbuf;

	blobmsg_parse(hello_policy, ARRAY_SIZE(hello_policy), tb, blob_data(msg), blob_len(msg));

	if (tb[HELLO_MSG])
		msgstr = blobmsg_data(tb[HELLO_MSG]);

	blob_buf_init(&b, 0);
	strbuf = blobmsg_alloc_string_buffer(&b, "message", 64 + strlen(obj->name) + strlen(msgstr));
	sprintf(strbuf, "%s: Hello, world: %s", obj->name, msgstr);
	blobmsg_add_string_buffer(&b);
	ubus_send_reply(ctx, req, b.head);
	return 0;
}

static const struct ubus_method test_methods[] = {
	{ .name = "hello", .handler = test_hello },
};

static struct ubus_object test_object = {
	.name = "test",
	.type = &test_object_type,
	.methods = test_methods,
	.n_methods = ARRAY_SIZE(test_methods),
};

static struct ubus_object test_object2 = {
	.name = "test2",
	.type = &test_object_type,
	.methods = test_methods,
	.n_methods = ARRAY_SIZE(test_methods),
};

int main(int argc, char **argv)
{
	const char *progname, *ubus_socket = NULL;
	int ret = 0;
	int ch;

	progname = argv[0];

	while ((ch = getopt(argc, argv, "s:")) != -1) {
		switch (ch) {
		case 's':
			ubus_socket = optarg;
			break;
		default:
			break;
		}
	}

	argc -= optind;
	argv += optind;

	ctx = ubus_connect(ubus_socket);
	if (!ctx) {
		fprintf(stderr, "Failed to connect to ubus\n");
		return -1;
	}

	fprintf(stderr, "Connected as ID 0x%08x\n", ctx->local_id);

	fprintf(stderr, "Publishing object\n");
	ret = ubus_add_object(ctx, &test_object);
	if (ret)
		fprintf(stderr, "Failed to add_object object: %s\n", ubus_strerror(ret));
	else {
		fprintf(stderr, "Object ID: %08x\n", test_object.id);
		fprintf(stderr, "Object Type ID: %08x\n", test_object.type->id);
	}

	fprintf(stderr, "Publishing object\n");
	ret = ubus_add_object(ctx, &test_object2);
	if (ret)
		fprintf(stderr, "Failed to add_object object: %s\n", ubus_strerror(ret));
	else {
		fprintf(stderr, "Object ID: %08x\n", test_object2.id);
		fprintf(stderr, "Object Type ID: %08x\n", test_object2.type->id);
	}
	uloop_init();
	ubus_add_uloop(ctx);
	uloop_run();
	uloop_done();

	ubus_free(ctx);
	return 0;
}