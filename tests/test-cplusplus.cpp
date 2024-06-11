#include "libubus.h"

// Ensure UBUS_* macros can be used from C++

static int handler(ubus_context *, ubus_object *, ubus_request_data *, const char *, blob_attr *)
{
	return 0;
}

enum {
	HELLO_ID,
	HELLO_MSG,
};

constexpr blobmsg_policy hello_policy[] = {
	[HELLO_ID] = { .name = "id", .type = BLOBMSG_TYPE_INT32 },
	[HELLO_MSG] = { .name = "msg", .type = BLOBMSG_TYPE_STRING },
};

constexpr ubus_method test_methods[] = {
	UBUS_METHOD("hello1", handler, hello_policy),
	UBUS_METHOD_TAG("hello2", handler, hello_policy, UBUS_TAG_ADMIN | UBUS_TAG_PRIVATE),
	UBUS_METHOD_MASK("hello3", handler, hello_policy, 0),
	UBUS_METHOD_NOARG("hello4", handler),
	UBUS_METHOD_TAG_NOARG("hello5", handler, UBUS_TAG_STATUS),
};

constexpr ubus_object_type test_object_type = UBUS_OBJECT_TYPE("test", test_methods);

int main()
{
	(void) test_object_type;
	return 0;
}
