/*
 * pistrap-helper: the root half of PiStrap. Installed setuid root inside
 * PiStrap.app by the image tools (PiStrap itself runs as mobile).
 *
 *   pistrap-helper status
 *   pistrap-helper bootstrap <file.tar[.gz|.xz|.zst]>
 *   pistrap-helper install <file.deb>...
 *   pistrap-helper uicache
 *
 * Only files inside PiStrap.app/Packages or /var/mobile/Library/PiStrap are
 * accepted, and the bootstrap may only create paths under /var/jb. Output is
 * plain lines on stdout (PiStrap shows them); the exit status is 0 on success.
 * The archive is read with the system libarchive (dlopen, no headers needed).
 */
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define JB "/var/jb"
#define USER_DIR "/var/mobile/Library/PiStrap/"

static char app_pkgs[PATH_MAX];         /* <PiStrap.app>/Packages/ */

static int
allowed(const char *path)
{
	char real[PATH_MAX];
	if (realpath(path, real) == NULL) {
		printf("error: %s: %s\n", path, strerror(errno));
		return 0;
	}
	if (strncmp(real, USER_DIR, strlen(USER_DIR)) == 0 ||
	    (app_pkgs[0] && strncmp(real, app_pkgs, strlen(app_pkgs)) == 0)) {
		return 1;
	}
	printf("error: %s is not in PiStrap's package folders\n", real);
	return 0;
}

static int
run(char *const argv[])
{
	char *env[] = {
		"PATH=" JB "/usr/bin:" JB "/bin:" JB "/usr/sbin:" JB "/sbin:/usr/bin:/bin:/usr/sbin:/sbin",
		"HOME=/var/root", "TMPDIR=" JB "/tmp", "DEBIAN_FRONTEND=noninteractive", NULL
	};
	pid_t pid;
	printf("$");
	for (int i = 0; argv[i]; i++) {
		printf(" %s", argv[i]);
	}
	printf("\n");
	fflush(stdout);
	int e = posix_spawn(&pid, argv[0], NULL, NULL, argv, env);
	if (e) {
		printf("error: cannot run %s: %s\n", argv[0], strerror(e));
		return -1;
	}
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
	}
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		printf("error: %s exited with status 0x%x\n", argv[0], st);
		return -1;
	}
	return 0;
}

/* libarchive, resolved at run time. */
struct la {
	void *(*read_new)(void);
	int (*support_filter_all)(void *);
	int (*support_format_all)(void *);
	int (*open_filename)(void *, const char *, size_t);
	int (*next_header)(void *, void **);
	int (*read_close)(void *);
	int (*read_free)(void *);
	const char *(*error_string)(void *);
	const char *(*entry_pathname)(void *);
	void (*entry_set_pathname)(void *, const char *);
	const char *(*entry_hardlink)(void *);
	void (*entry_set_hardlink)(void *, const char *);
	int (*read_extract)(void *, void *, int);
};

static int
la_load(struct la *a)
{
	void *h = dlopen("/usr/lib/libarchive.2.dylib", RTLD_NOW);
	if (h == NULL) {
		printf("error: no libarchive: %s\n", dlerror());
		return -1;
	}
#define SYM(f, n) if ((*(void **)&a->f = dlsym(h, n)) == NULL) { printf("error: libarchive lacks %s\n", n); return -1; }
	SYM(read_new, "archive_read_new");
	SYM(support_filter_all, "archive_read_support_filter_all");
	SYM(support_format_all, "archive_read_support_format_all");
	SYM(open_filename, "archive_read_open_filename");
	SYM(next_header, "archive_read_next_header");
	SYM(read_close, "archive_read_close");
	SYM(read_free, "archive_read_free");
	SYM(error_string, "archive_error_string");
	SYM(entry_pathname, "archive_entry_pathname");
	SYM(entry_set_pathname, "archive_entry_set_pathname");
	SYM(entry_hardlink, "archive_entry_hardlink");
	SYM(entry_set_hardlink, "archive_entry_set_hardlink");
	SYM(read_extract, "archive_read_extract");
#undef SYM
	return 0;
}

/* "./var/jb/x" or "var/jb/x" -> "/var/jb/x"; anything else (or "..") -> NULL. */
static const char *
jb_path(const char *p, char *out, size_t n)
{
	while (p[0] == '.' && p[1] == '/') {
		p += 2;
	}
	while (*p == '/') {
		p++;
	}
	if (strncmp(p, "var/jb", 6) != 0 || (p[6] != '/' && p[6] != '\0') || strstr(p, "..")) {
		return NULL;
	}
	/*
	 * /var is a symlink to /private/var, and ARCHIVE_EXTRACT_SECURE_SYMLINKS
	 * refuses to extract through it: the first run left /var/jb empty.
	 */
	snprintf(out, n, "/private/%s", p);
	return out;
}

static int
bootstrap(const char *file)
{
	struct la a;
	if (!allowed(file) || la_load(&a)) {
		return 1;
	}
	struct stat sb;
	if (stat(JB "/.procursus_strapped", &sb) == 0) {
		printf("already bootstrapped (" JB "/.procursus_strapped exists)\n");
		return 0;
	}
	void *ar = a.read_new();
	a.support_filter_all(ar);
	a.support_format_all(ar);
	if (a.open_filename(ar, file, 65536) != 0) {
		printf("error: %s: %s\n", file, a.error_string(ar));
		return 1;
	}
	mkdir(JB, 0755);
	/* ARCHIVE_EXTRACT_OWNER|PERM|TIME|ACL|FFLAGS|XATTR|SECURE_NODOTDOT|SECURE_SYMLINKS */
	int flags = 0x1 | 0x2 | 0x4 | 0x20 | 0x40 | 0x80 | 0x200 | 0x100;
	void *ent;
	long n = 0;
	int r;
	char path[PATH_MAX], link[PATH_MAX];
	while ((r = a.next_header(ar, &ent)) == 0) {
		const char *name = a.entry_pathname(ent);
		if (name == NULL || jb_path(name, path, sizeof(path)) == NULL) {
			if (name && strcmp(name, ".") && strcmp(name, "./") && strcmp(name, "./var/") &&
			    strcmp(name, "var/") && strcmp(name, "./var") && strcmp(name, "var")) {
				printf("skipped (outside " JB "): %s\n", name);
			}
			continue;
		}
		a.entry_set_pathname(ent, path);
		const char *hl = a.entry_hardlink(ent);
		if (hl) {
			if (jb_path(hl, link, sizeof(link)) == NULL) {
				printf("skipped hard link outside " JB ": %s\n", hl);
				continue;
			}
			a.entry_set_hardlink(ent, link);
		}
		if (a.read_extract(ar, ent, flags) != 0) {
			printf("warning: %s: %s\n", path, a.error_string(ar));
		}
		if ((++n % 500) == 0) {
			printf("extracted %ld files\n", n);
			fflush(stdout);
		}
	}
	if (r != 1) {           /* ARCHIVE_EOF */
		printf("error: reading %s: %s\n", file, a.error_string(ar));
		a.read_free(ar);
		return 1;
	}
	a.read_close(ar);
	a.read_free(ar);
	printf("extracted %ld files into " JB "\n", n);
	if (stat(JB "/prep_bootstrap.sh", &sb) == 0) {
		char *argv[] = { JB "/bin/sh", JB "/prep_bootstrap.sh", NULL };
		if (run(argv)) {
			return 1;
		}
	}
	return 0;
}

static int
install(int argc, char **argv)
{
	char *args[64] = { JB "/usr/bin/dpkg", "-i", "--force-confnew" };
	int n = 3;
	for (int i = 0; i < argc && n < 63; i++) {
		if (!allowed(argv[i])) {
			return 1;
		}
		args[n++] = argv[i];
	}
	args[n] = NULL;
	return run(args) ? 1 : 0;
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (setuid(0) || setgid(0)) {
		printf("error: not running as root (helper must be setuid root)\n");
		return 1;
	}
	/* <app>/pistrap-helper -> <app>/Packages/ */
	char self[PATH_MAX];
	if (realpath(argv[0], self)) {
		char *slash = strrchr(self, '/');
		if (slash) {
			*slash = '\0';
			snprintf(app_pkgs, sizeof(app_pkgs), "%s/Packages/", self);
		}
	}
	if (argc >= 2 && strcmp(argv[1], "status") == 0) {
		struct stat sb;
		printf("root ok; bootstrap %s\n", stat(JB "/.procursus_strapped", &sb) == 0 ? "installed" : "not installed");
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "bootstrap") == 0) {
		return bootstrap(argv[2]);
	}
	if (argc >= 3 && strcmp(argv[1], "install") == 0) {
		return install(argc - 2, argv + 2);
	}
	if (argc == 2 && strcmp(argv[1], "uicache") == 0) {
		char *a[] = { JB "/usr/bin/uicache", "-a", NULL };
		return run(a) ? 1 : 0;
	}
	printf("usage: pistrap-helper status | bootstrap <tar> | install <deb>... | uicache\n");
	return 2;
}
