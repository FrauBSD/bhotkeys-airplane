/*
 * Copyright (c) 2026 Devin Teske <dteske@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * airplane-radiosd: root helper for airplane mode.
 *
 * rc.d starts it as root. It listens on a Unix socket (0660, group
 * airplane_radiosd) so a session can turn radios off or on without becoming
 * root. Membership in wheel is not required.
 *
 * Line protocol (one command per connection, reply one line):
 *   on | off | status | quit
 *
 * Usage:
 *   airplane-radiosd start|stop|status|serve
 *   airplane-radiosd on|off
 *     (direct, when already root; also used internally)
 *
 * WLAN interface: airplane_radiosd_wlan, else AIRPLANE_WLAN, else wlan0.
 *
 * Bluetooth: stop/start /etc/rc.d/bluetooth for ubt0..ubt3, unload/load
 * ng_ubt(4), and best-effort iwmbtfw(8) for Intel USB controllers.
 */
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <net/if.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UBT_MAX		4
#define DEFAULT_SOCK	"/var/run/airplane-radiosd.sock"
#define DEFAULT_PID	"/var/run/airplane-radiosd.pid"
#define DEFAULT_GROUP	"airplane_radiosd"

static const char *sock_path = DEFAULT_SOCK;
static const char *pid_path = DEFAULT_PID;
static const char *sock_group = DEFAULT_GROUP;
static char wlan_if[IFNAMSIZ] = "wlan0";
static volatile sig_atomic_t running = 1;

static void
on_signal(int sig __unused)
{
	running = 0;
}

static int
run_argv(const char *path, char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		int fd;

		fd = open("/dev/null", O_RDONLY);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			if (fd > 2)
				close(fd);
		}
		execv(path, argv);
		_exit(127);
	}
	for (;;) {
		if (waitpid(pid, &status, 0) < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		break;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return (0);
	return (-1);
}

static void
wlan_init(void)
{
	const char *n;

	n = getenv("airplane_radiosd_wlan");
	if (n == NULL || n[0] == '\0')
		n = getenv("AIRPLANE_WLAN");
	if (n == NULL || n[0] == '\0')
		return;
	if (strlcpy(wlan_if, n, sizeof(wlan_if)) >= sizeof(wlan_if))
		errx(1, "wlan name too long");
}

static int
wlan_flags(const char *ifname, int *flags_out)
{
	struct ifreq ifr;
	int s;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return (-1);
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFFLAGS, &ifr) != 0) {
		close(s);
		return (-1);
	}
	*flags_out = ifr.ifr_flags;
	close(s);
	return (0);
}

static int
wlan_set_up(const char *ifname, int up)
{
	struct ifreq ifr;
	int s, flags;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return (-1);
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFFLAGS, &ifr) != 0) {
		close(s);
		return (-1);
	}
	flags = ifr.ifr_flags;
	if (up)
		flags |= IFF_UP;
	else
		flags &= ~IFF_UP;
	ifr.ifr_flags = flags;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) != 0) {
		close(s);
		return (-1);
	}
	close(s);
	return (0);
}

static void
bluetooth_stack_stop(void)
{
	int i;
	char unit[8];
	char *argv[5];

	for (i = 0; i < UBT_MAX; i++) {
		snprintf(unit, sizeof(unit), "ubt%d", i);
		argv[0] = "bluetooth";
		argv[1] = "quietstop";
		argv[2] = unit;
		argv[3] = NULL;
		(void)run_argv("/etc/rc.d/bluetooth", argv);
	}
}

static void
bluetooth_stack_start(void)
{
	int i;
	char unit[8];
	char *argv[5];

	for (i = 0; i < UBT_MAX; i++) {
		snprintf(unit, sizeof(unit), "ubt%d", i);
		argv[0] = "bluetooth";
		argv[1] = "quietstart";
		argv[2] = unit;
		argv[3] = NULL;
		(void)run_argv("/etc/rc.d/bluetooth", argv);
	}
}

/*
 * Best-effort Intel USB BT firmware reload after ng_ubt attach.
 * Scans usbconfig(8) for vendor 0x8087 devices.
 */
static void
iwmbt_firmware_load(void)
{
	FILE *fp;
	char line[512];
	char ugen[32];
	char *argv[6];
	char *p, *q;

	if (access("/usr/sbin/iwmbtfw", X_OK) != 0)
		return;
	fp = popen("/usr/sbin/usbconfig list 2>/dev/null", "r");
	if (fp == NULL)
		return;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strstr(line, "0x8087") == NULL)
			continue;
		p = strstr(line, "ugen");
		if (p == NULL)
			continue;
		q = strpbrk(p, ": \t\r\n");
		if (q == NULL)
			continue;
		if ((size_t)(q - p) >= sizeof(ugen))
			continue;
		memcpy(ugen, p, (size_t)(q - p));
		ugen[q - p] = '\0';
		argv[0] = "iwmbtfw";
		argv[1] = "-d";
		argv[2] = ugen;
		argv[3] = NULL;
		(void)run_argv("/usr/sbin/iwmbtfw", argv);
	}
	pclose(fp);
}

static void
bluetooth_off(void)
{
	char *argv_unload[] = { "kldunload", "-n", "ng_ubt", NULL };

	bluetooth_stack_stop();
	/* Already unloaded is not a failure. */
	(void)run_argv("/sbin/kldunload", argv_unload);
}

static void
bluetooth_on(void)
{
	char *argv_load[] = { "kldload", "-n", "ng_ubt", NULL };

	(void)run_argv("/sbin/kldload", argv_load);
	/* Give USB/devd a moment to attach ubt nodes. */
	usleep(400000);
	iwmbt_firmware_load();
	usleep(200000);
	bluetooth_stack_start();
}

static int
airplane_active(const char *ifname)
{
	int flags;

	/*
	 * Airplane mode is active when WLAN is not UP. A missing
	 * interface counts as down. Toggle pairs Bluetooth quiet or
	 * restore with the WLAN change.
	 */
	if (wlan_flags(ifname, &flags) != 0)
		return (1);
	return ((flags & IFF_UP) == 0);
}

static int
do_status(char *out, size_t outsz)
{
	snprintf(out, outsz, "%s",
	    airplane_active(wlan_if) ? "on" : "off");
	return (0);
}

static int
do_on(char *out, size_t outsz)
{
	bluetooth_off();
	if (wlan_set_up(wlan_if, 0) != 0) {
		snprintf(out, outsz, "error: %s down: %s",
		    wlan_if, strerror(errno));
		return (-1);
	}
	snprintf(out, outsz, "ok");
	return (0);
}

static int
do_off(char *out, size_t outsz)
{
	bluetooth_on();
	if (wlan_set_up(wlan_if, 1) != 0) {
		snprintf(out, outsz, "error: %s up: %s",
		    wlan_if, strerror(errno));
		return (-1);
	}
	snprintf(out, outsz, "ok");
	return (0);
}

/*
 * Handle one command. Writes reply into out. Returns 0 on success,
 * -1 on error (out still may hold a message), 1 if quit requested.
 */
static int
handle_cmd(const char *cmd, char *out, size_t outsz)
{
	if (strcmp(cmd, "on") == 0)
		return (do_on(out, outsz) == 0 ? 0 : -1);
	if (strcmp(cmd, "off") == 0)
		return (do_off(out, outsz) == 0 ? 0 : -1);
	if (strcmp(cmd, "status") == 0)
		return (do_status(out, outsz) == 0 ? 0 : -1);
	if (strcmp(cmd, "quit") == 0) {
		snprintf(out, outsz, "bye");
		running = 0;
		return (1);
	}
	snprintf(out, outsz, "error: unknown command");
	return (-1);
}

static void
trim_line(char *s)
{
	size_t n;

	n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
	    s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
}

static void
fix_sock_owner(void)
{
	struct group *gr;

	gr = getgrnam(sock_group);
	if (gr == NULL)
		errx(1, "group %s: not found", sock_group);
	if (chown(sock_path, 0, gr->gr_gid) != 0)
		err(1, "chown %s", sock_path);
	if (chmod(sock_path, 0660) != 0)
		err(1, "chmod %s", sock_path);
}

static int
serve_loop(void)
{
	struct sockaddr_un sun;
	int lfd, cfd;
	char buf[256], reply[256];
	ssize_t n;
	size_t len;

	(void)unlink(sock_path);
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0)
		err(1, "socket");
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	if (strlcpy(sun.sun_path, sock_path, sizeof(sun.sun_path)) >=
	    sizeof(sun.sun_path))
		errx(1, "socket path too long");
	if (bind(lfd, (struct sockaddr *)&sun, SUN_LEN(&sun)) != 0)
		err(1, "bind %s", sock_path);
	fix_sock_owner();
	if (listen(lfd, 4) != 0)
		err(1, "listen");

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	while (running) {
		int hr = 0;

		cfd = accept(lfd, NULL, NULL);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			if (!running)
				break;
			warn("accept");
			continue;
		}
		len = 0;
		while (len + 1 < sizeof(buf)) {
			n = read(cfd, buf + len, sizeof(buf) - 1 - len);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (n == 0)
				break;
			len += (size_t)n;
			if (memchr(buf, '\n', len) != NULL)
				break;
		}
		buf[len] = '\0';
		trim_line(buf);
		reply[0] = '\0';
		if (buf[0] != '\0') {
			hr = handle_cmd(buf, reply, sizeof(reply));
			if (reply[0] == '\0')
				snprintf(reply, sizeof(reply),
				    hr == 0 ? "ok" : "error");
			dprintf(cfd, "%s\n", reply);
		}
		close(cfd);
		if (hr == 1)
			break;
	}
	close(lfd);
	(void)unlink(sock_path);
	return (0);
}

static int
cmd_start_foreground(void)
{
	if (geteuid() != 0)
		errx(1, "must run as root");
	warnx("serving %s", sock_path);
	return (serve_loop());
}

static int
read_pid(void)
{
	FILE *fp;
	int pid = -1;

	fp = fopen(pid_path, "r");
	if (fp == NULL)
		return (-1);
	if (fscanf(fp, "%d", &pid) != 1)
		pid = -1;
	fclose(fp);
	return (pid);
}

static int
cmd_stop(void)
{
	int pid;

	pid = read_pid();
	if (pid > 1) {
		if (kill(pid, SIGTERM) != 0 && errno != ESRCH)
			warn("kill %d", pid);
	}
	(void)unlink(sock_path);
	(void)unlink(pid_path);
	return (0);
}

static int
cmd_status_svc(void)
{
	int pid;
	char cur[64];

	pid = read_pid();
	/*
	 * kill(pid, 0) returns EPERM when the process exists and the caller
	 * may not signal it. Status only asks whether it exists.
	 */
	if (pid > 1 && (kill(pid, 0) == 0 || errno == EPERM))
		printf("airplane-radiosd is running as pid %d.\n", pid);
	else
		printf("airplane-radiosd is not running.\n");
	if (do_status(cur, sizeof(cur)) == 0)
		printf("airplane=%s\n", cur);
	return (0);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: airplane-radiosd start|stop|status|serve\n"
	    "       airplane-radiosd on|off\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	char reply[256];
	const char *env;
	int hr;

	env = getenv("airplane_radiosd_socket");
	if (env != NULL && env[0] != '\0')
		sock_path = env;
	env = getenv("airplane_radiosd_group");
	if (env != NULL && env[0] != '\0')
		sock_group = env;
	wlan_init();

	if (argc < 2)
		usage();

	if (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "serve") == 0)
		return (cmd_start_foreground());
	if (strcmp(argv[1], "stop") == 0)
		return (cmd_stop());
	if (strcmp(argv[1], "status") == 0 && argc == 2)
		return (cmd_status_svc());

	/* Direct commands when already root. */
	if (geteuid() != 0)
		errx(1, "must run as root for direct commands");
	hr = handle_cmd(argv[1], reply, sizeof(reply));
	if (reply[0] != '\0')
		printf("%s\n", reply);
	return (hr < 0 ? 1 : 0);
}
