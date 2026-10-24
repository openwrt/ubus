#ifndef __TEST_UBUSD_H
#define __TEST_UBUSD_H

#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libubus.h"

#define TEST_SKIPPED	77

static const char *ubusd_bin;
static char sock_path[PATH_MAX];
static char sock_dir[] = "/tmp/ubus-test-XXXXXX";
static volatile pid_t ubusd_pid;
static int failures;

#define CHECK(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "%s:%d: check failed: %s\n",		\
			__FILE__, __LINE__, #cond);			\
		failures++;						\
	}								\
} while (0)

static inline void ubusd_start(void)
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

static inline void ubusd_reap(void)
{
	waitpid(ubusd_pid, NULL, 0);
	ubusd_pid = 0;
	unlink(sock_path);
}

static inline void ubusd_kill(void)
{
	kill(ubusd_pid, SIGKILL);
	ubusd_reap();
}

static inline void ubusd_kill_alarm(int sig)
{
	kill(ubusd_pid, SIGKILL);
}

static inline bool object_visible(const char *name)
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

static inline bool proc_file_write(const char *path, const char *data)
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
static inline bool userns_enter(void)
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

static inline int test_ubusd_init(int argc, char **argv)
{
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

	if (!mkdtemp(sock_dir))
		return 2;

	snprintf(sock_path, sizeof(sock_path), "%s/ubus.sock", sock_dir);

	return 0;
}

static inline int test_ubusd_done(void)
{
	rmdir(sock_dir);

	if (failures)
		fprintf(stderr, "%d check(s) failed\n", failures);

	return failures ? 1 : 0;
}

#endif
