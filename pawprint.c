// SPDX-License-Identifier: MIT

/*
 * pawprint
 * Copyright (c) 2022-2025 Ziyao.
 * This project is a part of eweOS.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <grp.h>
#include <linux/fs.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static struct {
	unsigned int boot : 1;
	unsigned int clean : 1;
	unsigned int create : 1;
	unsigned int remove : 1;
	unsigned int no_default : 1;
	char **excluded;
	int excluded_count;
} g_arg;

/*
 * Handlers are executed from the lowest number/bit to the highest
 */
enum {
	A_CREATE = 1,	  // Create file
	A_APPEND,	  // Append file

	A_CREATE_DIR = 4, // Create directory
	A_WRITE,	  // Write to file
	A_OWNER,	  // Adjust ownership
	A_PERM,		  // Adjust permission
	A_CLEAN,	  // Clean directory (based on age)
	A_REMOVE,	  // Remove file, or contents of directory
	A_ATTR,		  // Set file attributes
	A_EXCLUDE,	  // Do not remove

	A_EXIST = 28,	  // Check if file or directory exists
	A_NOSYM,	  // Do not follow symlink
	A_RECUR,	  // Recursively apply rules
	A_GLOB,		  // Expand glob
};

#define s(k) (1 << (k))

typedef uint32_t entry_attr_t;

struct file_entry {
	const char *path;
	entry_attr_t attr;
};

// Log Macros
FILE *g_log_file;
#define log_error(...) (fprintf(g_log_file, "error: " __VA_ARGS__))
#define log_warn(...) (fprintf(g_log_file, "warning: " __VA_ARGS__))
#define check(assertion, ...)                                                  \
	do {                                                                   \
		if (!(assertion)) {                                            \
			log_error(__VA_ARGS__);                                \
			exit(-1);                                              \
		}                                                              \
	} while (0)
#define checkl(cond, log, ...) ((void)(cond ? 0 : log_warn(log, __VA_ARGS__)))

static const char *skip_space(const char *p)
{
	while ((*p == ' ' || *p == '\t') && *p)
		p++;
	return p;
}

static void next_line(FILE *fp)
{
	while (fgetc(fp) != '\n')
		;
	return;
}

static void free_if(int num, ...)
{
	va_list arg;
	va_start(arg, num);
	void *p;

	while (num) {
		p = va_arg(arg, void *);
		if (p)
			free(p);
		num--;
	}

	va_end(arg);
	return;
}

static int is_valid_file(const char *path)
{
	struct stat t;
	return !stat(path, &t);
}

/*
 * NOTE: Assuming that the file exists
 */
static time_t get_last_time(const char *path)
{
	struct stat t;

	stat(path, &t);

	time_t last_change = t.st_atim.tv_sec > t.st_mtim.tv_sec
				 ? t.st_atim.tv_sec
				 : t.st_mtim.tv_sec;
	return last_change > t.st_ctim.tv_sec ? last_change : t.st_ctim.tv_sec;
}

static int is_directory(const char *path)
{
	struct stat t;

	if (stat(path, &t)) {
		log_warn("Cannot get the status of file %s\n", path);
		return 0;
	}

	return S_ISDIR(t.st_mode);
}

static void iterate_directory_sub(const char *path,
				  void (*callback)(const char *path, void *ctx),
				  void *ctx, bool r, bool top)
{
	DIR *root = opendir(path);
	if (!root) {
		log_warn("Cannot open directory %s\n", path);
		return;
	}
	chdir(path);

	for (struct dirent *dir = readdir(root); dir; dir = readdir(root)) {
		if (dir->d_name[0] == '.')
			continue;

		if (is_directory(dir->d_name) && r) {
			iterate_directory_sub(dir->d_name, callback, ctx, true,
					      false);
		} else {
			callback(dir->d_name, ctx);
		}
	}

	closedir(root);
	chdir("..");

	if (!top)
		callback(path, ctx);

	return;
}

static void iterate_directory(const char *path,
			      void (*callback)(const char *path, void *ctx),
			      void *ctx, bool r)
{
	iterate_directory_sub(path, callback, ctx, r, true);
}

static void glob_match(const char *pattern,
		       void (*callback)(const char *path, void *ctx), void *ctx)
{
	glob_t buf;

	if (glob(pattern, GLOB_NOSORT, NULL, &buf)) {
		return;
	}

	for (size_t i = 0; i < buf.gl_pathc; i++)
		callback(buf.gl_pathv[i], ctx);

	globfree(&buf);

	return;
}

#define def_handler(name)                                                      \
	static void name(const char *path, const char *mode, const char *user, \
			 const char *group, const char *age, const char *arg)
#define handler_ignore                                                         \
	(void)path;                                                            \
	(void)mode;                                                            \
	(void)user;                                                            \
	(void)group;                                                           \
	(void)age;                                                             \
	(void)arg;

static time_t convert_age(const char *s)
{
	if (!s[0] || s[0] == '-') // Skip
		return (time_t)-1;

	time_t t = 0;
	for (time_t scale = strtol(s, (char **)&s, 0); *s;
	     scale = strtol(s, (char **)&s, 0)) {
		time_t unit = *s == 'd'	  ? 86400
			      : *s == 'w' ? 604800
			      : *s == 'h' ? 3600
			      : *s == 'm' ? 60
			      : *s == 's' ? 1
					  : 0;
		if (!unit)
			return (time_t)-1;
		t += unit * scale;
		s++;
	}

	return t;
}

inline static int is_excluded(const char *path)
{
	for (int i = 0; i < g_arg.excluded_count - 1; i++) {
		if (!fnmatch(g_arg.excluded[i], path, 0))
			return 1;
	}
	return 0;
}

static void clean_file(const char *path, void *ctx)
{
	if (is_excluded(path))
		return;

	time_t ddl = *(time_t *)ctx;
	if (get_last_time(path) < ddl || g_arg.clean) {
		if (remove(path))
			log_warn("Cannot remove file %s\n", path);
	}
	return;
}

def_handler(attr_clean)
{
	handler_ignore;

	if (!g_arg.clean)
		return;

	time_t ddl = time(NULL) - convert_age(age);
	iterate_directory(path, clean_file, &ddl, true);

	return;
}

def_handler(attr_create_dir)
{
	handler_ignore;

	if (!g_arg.create)
		return;

	if (!is_valid_file(path)) {
		if (mkdir(path, 0755))
			log_warn("Cannot create directory %s\n", path);
	}
	return;
}

def_handler(attr_create)
{
	handler_ignore;
	if (!g_arg.create)
		return;

	if (!is_valid_file(path)) {
		int fd = open(path, O_CREAT | O_WRONLY);
		if (fd < 0) {
			log_warn("Cannot create file %s\n", path);
			return;
		}
		close(fd);
	}
	return;
}

def_handler(attr_perm)
{
	handler_ignore;

	if (mode[0] == '-' || !mode[0]) // Simply ignore
		return;

	if (chmod(path, (mode_t)strtol(mode, NULL, 8)))
		log_warn("Cannot set file mode as %s for %s\n", mode, path);

	return;
}

static void adjust_user(const char *path, const char *user)
{
	if (user[0] == '-' || !user[0]) // Ignore
		return;

	struct passwd *pswd = getpwnam(user);
	if (!pswd) {
		log_warn("Invalid user %s\n", user);
		return;
	}

	if (chown(path, pswd->pw_uid, -1))
		log_warn("Cannot transfer file %s to user %s\n", path, user);
	return;
}

static void adjust_group(const char *path, const char *group)
{
	if (group[0] == '-' || !group[0])
		return;

	struct group *info = getgrnam(group);
	if (!info) {
		log_warn("Invalid group %s\n", group);
		return;
	}

	if (chown(path, -1, info->gr_gid))
		log_warn("Cannot transfer file %s to group %s\n", path, group);
	return;
}

def_handler(attr_owner)
{
	handler_ignore;
	adjust_user(path, user);
	adjust_group(path, group);
	return;
}

def_handler(attr_write)
{
	handler_ignore;

	if (!g_arg.create)
		return;

	if (is_valid_file(path)) {
		int fd = open(path, O_WRONLY | O_TRUNC);
		if (fd < 0) {
			log_warn("Cannot open file %s\n", path);
			return;
		}
		ssize_t length = strlen(arg);
		for (ssize_t s = write(fd, arg, strlen(arg)); s && length;
		     s = write(fd, arg, length)) {
			if (s <= 0) {
				log_warn("Cannot write to file %s\n", path);
				close(fd);
				return;
			}
			length -= s;
		}
		close(fd);
	}
	return;
}

static void do_remove(const char *path, void *in)
{
	(void)in;
	remove(path);
	return;
}

def_handler(attr_remove)
{
	handler_ignore;

	if (!g_arg.remove)
		return;

	if (is_directory(path)) {
		iterate_directory(path, do_remove, NULL, true);
	} else {
		do_remove(path, NULL);
	}

	return;
}

static void do_set_file_attr(const char *path, void *in)
{
	const char *attr = in;

	int type = 0;
	if (attr[0] == '+') {
		type = 1;
	} else if (attr[0] == '-') {
		type = 0;
	} else {
		log_warn("Invalid file attribute operation %c\n", attr[0]);
		return;
	}

	static unsigned long int flags[256] = {
	    ['a'] = FS_APPEND_FL,      ['D'] = FS_DIRSYNC_FL,
	    ['i'] = FS_IMMUTABLE_FL,   ['j'] = FS_JOURNAL_DATA_FL,
	    ['A'] = FS_NOATIME_FL,     ['C'] = FS_NOCOW_FL,
	    ['d'] = FS_NODUMP_FL,      ['t'] = FS_NOTAIL_FL,
	    ['P'] = FS_PROJINHERIT_FL, ['s'] = FS_SECRM_FL,
	    ['S'] = FS_SYNC_FL,	       ['T'] = FS_TOPDIR_FL,
	    ['u'] = FS_UNRM_FL,
	};

	unsigned long int mask = 0;
	for (int i = 1; attr[i]; i++) {
		if (!flags[(int)attr[i]]) {
			log_warn("Invalid file attribute %c\n", attr[i]);
		}
		mask |= flags[(int)attr[i]];
	}

	mask = type ? mask : ~mask;

	int origin;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		log_warn("Cannot open file %s\n", path);
		return;
	}

	ioctl(fd, FS_IOC_GETFLAGS, &origin);
	origin = type ? origin | mask : origin & ~mask;
	ioctl(fd, FS_IOC_SETFLAGS, &origin);

	close(fd);

	return;
}

def_handler(attr_attr)
{
	handler_ignore;
	do_set_file_attr(path, (void *)arg);
	return;
}

def_handler(attr_exclude)
{
	handler_ignore;

	char *t = strdup(path);
	check(t, "Cannot allocate memory for excluded path %s\n", path);

	g_arg.excluded_count++;
	g_arg.excluded = (char **)realloc(
	    g_arg.excluded, sizeof(char *) * g_arg.excluded_count);
	check(g_arg.excluded, "Cannot allocate memory for excluded path %s\n",
	      path);
	g_arg.excluded[g_arg.excluded_count - 2] = t;

	return;
}

struct process_file_info {
	entry_attr_t attr;
	const char *mode, *user, *group, *age, *arg;
};

/*
 * The order in array ctx is important:
 *    attr, mode, user, group, age, arg
 * All are in the same size as a normal pointer
 */
static void process_file(const char *path, void *ctx)
{
	typedef void (*attr_handler)(const char *path, const char *mode,
				     const char *user, const char *group,
				     const char *age, const char *arg);
	static attr_handler handlers[] = {
	    [A_CREATE] = attr_create,
	    // [A_APPEND] = attr_append,

	    [A_PERM] = attr_perm,
	    [A_CREATE_DIR] = attr_create_dir,
	    [A_WRITE] = attr_write,
	    [A_OWNER] = attr_owner,
	    [A_CLEAN] = attr_clean,
	    [A_REMOVE] = attr_remove,
	    [A_ATTR] = attr_attr,
	    [A_EXCLUDE] = attr_exclude,
	};

	struct process_file_info *info = ctx;

	for (int i = 0, mask = 1; (size_t)i < (sizeof(info->attr) << 3) - 1;
	     i++) {
		if (info->attr & mask) {
			handlers[i](path, info->mode, info->user, info->group,
				    info->age, info->arg);
		}
		mask <<= 1;
	}

	return;
}

/*
 * NOTICE: Glob match will be done in parse_conf()
 */
static void parse_conf(FILE *conf)
{
	static entry_attr_t attr_table_type[256] = {
	    ['f'] = s(A_CREATE) | s(A_WRITE) | s(A_OWNER) | s(A_PERM),
	    ['w'] = s(A_WRITE),
	    ['d'] = s(A_CREATE_DIR) | s(A_OWNER) | s(A_PERM) | s(A_CLEAN),
	    ['D'] = s(A_CREATE_DIR) | s(A_OWNER) | s(A_PERM) | s(A_CLEAN) |
		    s(A_REMOVE),
	    ['r'] = s(A_REMOVE) | s(A_GLOB),
	    ['q'] = s(A_CREATE_DIR) | s(A_OWNER) | s(A_PERM) | s(A_CLEAN),
	    ['Q'] = s(A_CREATE_DIR) | s(A_OWNER) | s(A_PERM) | s(A_CLEAN) |
		    s(A_REMOVE),
	    ['x'] = s(A_EXCLUDE),
	    ['z'] = s(A_OWNER) | s(A_PERM) | s(A_GLOB),
	    ['h'] = s(A_ATTR) | s(A_GLOB),
	};

	while (!feof(conf)) {
		char *line = NULL;
		char *type = NULL, *path = NULL, *mode = NULL, *user = NULL,
		     *group = NULL, *age = NULL;

		size_t buf_len = 0;
		ssize_t line_len = getline(&line, &buf_len, conf);
		if (line_len < 0 || !line)
			goto break_cleanup;

		int term_len = 0;
		int matched = sscanf(line, "%ms%ms%ms%ms%ms%ms%n", &type, &path,
				     &mode, &user, &group, &age, &term_len);
		if (matched < 0)
			goto break_cleanup;

		const char *p = skip_space(type);
		entry_attr_t attr = 0x00;

		if (*p == '#') {
			goto continue_cleanup;
		} else if (attr_table_type[*p]) {
			attr |= attr_table_type[*p];
		} else {
			log_warn("Invalid type %c\n", *p);
			goto continue_cleanup;
		}
		p++;

		for (; *p; p++) {
			if (*p == '!' && !g_arg.boot)
				goto continue_cleanup;
			// TODO: '+' handler
		}

		char *arg = (char *)skip_space(line + term_len);
		arg[strlen(arg) - 1] = '\0';

		char dummy[1] = "";
		struct process_file_info info = {
		    .attr = attr & ~s(A_GLOB),
		    .mode = mode ? mode : dummy,
		    .user = user ? user : dummy,
		    .group = group ? group : dummy,
		    .age = age ? age : dummy,
		    .arg = arg,
		};
		if (attr & s(A_GLOB)) {
			glob_match(path, process_file, (void *)&info);
		} else {
			process_file(path, (void *)&info);
		}

	continue_cleanup:
		free_if(7, line, type, path, mode, user, group, age);
		continue;
	break_cleanup:
		free_if(7, line, type, path, mode, user, group, age);
		break;
	}

	return;
}

static void read_conf(const char *path, void *ctx)
{
	(void)ctx;
	FILE *conf = fopen(path, "r");
	if (!conf) {
		log_warn("Cannot open configuration file %s\n", path);
		return;
	}

	parse_conf(conf);
	fclose(conf);
	return;
}

static void usage(const char *name)
{
	fprintf(stderr, "%s [OPTIONS] [Configuration]\n", name);
	fputs("\n"
	      "Options:\n"
	      "  --clean        Clean files\n"
	      "  --create       Create files\n"
	      "  --remove       Remove files\n"
	      "  --boot         Enable entries marked on-boot-only ('!' "
	      "modifier)\n"
	      "  --no-default   Do not parse the default configuration\n"
	      "  --log          Specify the log file\n"
	      "  --help         Print this help\n"
	      "\n"
	      "Refer to tmpfiles.d(5) for details, though pawprint may not "
	      "implement all functions.\n"
	      "pawprint is a part of eweOS project, distributed under the MIT "
	      "License.\n",
	      stderr);
	return;
}

int main(int argc, const char *argv[])
{
	// By default, print log to stderr
	g_log_file = stderr;

	int conf_idx = 1;
	for (; conf_idx < argc; conf_idx++) {
		if (!strcmp(argv[conf_idx], "--clean")) {
			g_arg.clean = 1;
		} else if (!strcmp(argv[conf_idx], "--create")) {
			g_arg.create = 1;
		} else if (!strcmp(argv[conf_idx], "--remove")) {
			g_arg.remove = 1;
		} else if (!strcmp(argv[conf_idx], "--boot")) {
			g_arg.boot = 1;
		} else if (!strcmp(argv[conf_idx], "--no-default")) {
			g_arg.no_default = 1;
		} else if (!strcmp(argv[conf_idx], "--log")) {
			check(conf_idx + 1 < argc,
			      "missing filename for option -l\n");

			FILE *t = fopen(argv[conf_idx + 1], "a");
			if (!t)
				log_warn("Cannot open log file %s\n",
					 argv[conf_idx + 1]);
			g_log_file = t;
			conf_idx++;
		} else if (!strcmp(argv[conf_idx], "--help") ||
			   !strcmp(argv[conf_idx], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			break;
		}
	}

	g_arg.excluded = malloc(sizeof(char *));
	g_arg.excluded_count = 1;
	g_arg.excluded[0] = NULL;
	check(g_arg.excluded, "Cannot allocate memory for excluded path\n");

	if (!g_arg.no_default) {
		iterate_directory("/etc/tmpfiles.d", read_conf, NULL, true);
		iterate_directory("/lib/tmpfiles.d", read_conf, NULL, true);
	}

	/* Now no options are recognised */
	for (; conf_idx < argc; conf_idx++)
		read_conf(argv[conf_idx], NULL);

	fclose(g_log_file);
	free(g_arg.excluded);

	return 0;
}
