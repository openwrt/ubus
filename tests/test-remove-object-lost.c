#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libubus.h"

#define TEST_SKIPPED	77

static const char *ubusd_bin;
static char sock_path[PATH_MAX];
static volatile pid_t ubusd_pid;
static int failures;

#define CHECK(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "%s:%d: check failed: %s\n",		\
			__FILE__, __LINE__, #cond);			\
		failures++;						\
	}								\
} while (0)

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

static void ubusd_start(void)
{
	struct ubus_context *probe;
	int i;

	unlink(sock_path);
	ubusd_pid = fork();
	if (ubusd_pid < 0)
		exit(2);

	if (!ubusd_pid) {
		/* a test that crashes must not leave ubusd behind */
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		execl(ubusd_bin, ubusd_bin, "-s", sock_path, NULL);
		_exit(127);
	}

	for (i = 0; i < 100; i++) {
		probe = ubus_connect(sock_path);
		if (probe) {
			ubus_free(probe);
			return;
		}
		usleep(50 * 1000);
	}

	fprintf(stderr, "ubusd did not start\n");
	exit(2);
}

static void ubusd_reap(void)
{
	waitpid(ubusd_pid, NULL, 0);
	ubusd_pid = 0;
	unlink(sock_path);
}

static void ubusd_kill(void)
{
	kill(ubusd_pid, SIGKILL);
	ubusd_reap();
}

static void ubusd_kill_alarm(int sig)
{
	kill(ubusd_pid, SIGKILL);
}

static bool object_visible(const char *name)
{
	struct ubus_context *ctx = ubus_connect(sock_path);
	uint32_t id = 0;
	int ret;

	if (!ctx)
		return false;

	/* ubusd answers OK with no id for an object that has no type */
	ret = ubus_lookup_id(ctx, name, &id);
	ubus_free(ctx);

	return !ret && id;
}

static bool proc_file_write(const char *path, const char *data)
{
	ssize_t len = strlen(data);
	bool ret;
	int fd;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return false;

	ret = write(fd, data, len) == len;
	close(fd);

	return ret;
}

/*
 * ubusd applies its ACLs to every client that is not root, and it loads only
 * ACL files that root owns.
 */
static bool userns_enter(void)
{
	uid_t uid = getuid();
	gid_t gid = getgid();
	char map[32];

	if (!uid)
		return true;

	if (unshare(CLONE_NEWUSER))
		return false;

	snprintf(map, sizeof(map), "0 %u 1", (unsigned int) uid);
	if (!proc_file_write("/proc/self/uid_map", map) ||
	    !proc_file_write("/proc/self/setgroups", "deny"))
		return false;

	snprintf(map, sizeof(map), "0 %u 1", (unsigned int) gid);

	return proc_file_write("/proc/self/gid_map", map);
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
 * of the same type on a new context and a new ubusd must be visible.
 */
static void test_answer_lost_then_new_context(void)
{
	struct ubus_object *removed = object_new("test.removed");
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
	CHECK(test_type.id);

	kill(ubusd_pid, SIGSTOP);
	signal(SIGALRM, ubusd_kill_alarm);
	setitimer(ITIMER_REAL, &timer, NULL);

	ret = ubus_remove_object(ctx, removed);
	CHECK(ret == UBUS_STATUS_CONNECTION_FAILED);
	CHECK(!removed->id);
	CHECK(!ctx->objects.count);
	CHECK(!test_type.id);
	ubusd_reap();
	ubus_free(ctx);
	free(removed);

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

/* An error answer on a live connection leaves the object registered. */
static void test_error_answer_keeps_object(void)
{
	struct ubus_object *obj = object_new("test.kept");
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

	obj->id = id;
	CHECK(!ubus_remove_object(ctx, obj));
	CHECK(!ctx->objects.count);
	ubus_free(ctx);
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

int main(int argc, char **argv)
{
	char dir_template[] = "/tmp/ubus-test-XXXXXX";
	char *dir;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <ubusd>\n", argv[0]);
		return 2;
	}

	if (!userns_enter()) {
		fprintf(stderr, "not root and no user namespace, skipped\n");
		return TEST_SKIPPED;
	}

	ubusd_bin = argv[1];
	signal(SIGPIPE, SIG_IGN);

	dir = mkdtemp(dir_template);
	if (!dir)
		return 2;

	snprintf(sock_path, sizeof(sock_path), "%s/ubus.sock", dir);

	test_live_remove_type_id();
	test_error_answer_keeps_object();
	test_send_failed_then_reconnect();
	test_answer_lost_then_new_context();
	test_free_with_object_then_new_context();
	test_unknown_type_id();
	test_unknown_type_id_retry_refused();

	rmdir(dir);

	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);

	return failures ? 1 : 0;
}
