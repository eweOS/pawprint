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
	unsigned int noDefault : 1;
	char **excludedList;
	int excludedCount;
} gArg;

/*
 * NOTE:
 * Remember to check parse_conf if these macros are changed
 * Handlers are executed from the lowest bit to the highest
 */
#define s(k) (1 << (k))
#define ATTR_FILE s(0)	    // File or directory
#define ATTR_CREATE s(1)    // Create or fail
#define ATTR_APPEND s(2)    // Append or not

#define ATTR_PERM s(4)	    // Adjust permission
#define ATTR_CREATEDIR s(5) // Create directory
#define ATTR_NOSYM s(6)	    // Do not follow symlink
#define ATTR_RECUR s(7)	    // Recusively
#define ATTR_WRITE s(8)	    // Write message
#define ATTR_OWNERSHIP s(9) // Adjust ownership, both group and user
#define ATTR_CLEAN s(10)    // Need cleaning
#define ATTR_REMOVE s(11)   // Need removing
#define ATTR_ATTR s(12)	    // Need setting attribute
#define ATTR_EXCLUDE s(13)  // Do not remove

#define ATTR_ONBOOT s(30)   // On --boot only
#define ATTR_GLOB s(31)	    // Need expanding

typedef uint32_t entry_attr_t;

struct file_entry {
	const char *path;
	entry_attr_t attr;
};

// Log Macros
// For default,print log to stderr
FILE *gLogStream;
#define log_error(...) (fprintf(gLogStream, "error: " __VA_ARGS__))
#define log_warn(...) (fprintf(gLogStream, "warning: " __VA_ARGS__))
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

	time_t lastChange = t.st_atim.tv_sec > t.st_mtim.tv_sec
				? t.st_atim.tv_sec
				: t.st_mtim.tv_sec;
	return lastChange > t.st_ctim.tv_sec ? lastChange : t.st_ctim.tv_sec;
}

static int is_directory(const char *path)
{
	struct stat t;

	if (stat(path, &t)) {
		log_warn("Cannot get the status of file %s", path);
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
	static void name(const char *path, const char *mode,                   \
			 const char *userName, const char *grpName,            \
			 const char *age, const char *arg)
#define handler_ignore                                                         \
	(void)path;                                                            \
	(void)mode;                                                            \
	(void)userName;                                                        \
	(void)grpName;                                                         \
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
	for (int i = 0; i < gArg.excludedCount - 1; i++) {
		if (!fnmatch(gArg.excludedList[i], path, 0))
			return 1;
	}
	return 0;
}

static void clean_file(const char *path, void *ctx)
{
	if (is_excluded(path))
		return;

	time_t ddl = *(time_t *)ctx;
	if (get_last_time(path) < ddl || gArg.clean) {
		if (remove(path))
			log_warn("Cannot remove file %s\n", path);
	}
	return;
}

def_handler(attr_clean)
{
	handler_ignore;

	if (!gArg.clean)
		return;

	time_t ddl = time(NULL) - convert_age(age);
	iterate_directory(path, clean_file, &ddl, true);

	return;
}

def_handler(attr_createdir)
{
	handler_ignore;

	if (!gArg.create)
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
	if (!gArg.create)
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

	struct group *grpInfo = getgrnam(group);
	if (!grpInfo) {
		log_warn("Invalid group %s\n", group);
		return;
	}

	if (chown(path, -1, grpInfo->gr_gid))
		log_warn("Cannot transfer file %s to group %s\n", path, group);
	return;
}

def_handler(attr_ownership)
{
	handler_ignore;
	adjust_user(path, userName);
	adjust_group(path, grpName);
	return;
}

def_handler(attr_write)
{
	handler_ignore;

	if (!gArg.create)
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

	if (!gArg.remove)
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

	gArg.excludedCount++;
	gArg.excludedList = (char **)realloc(
	    gArg.excludedList, sizeof(char *) * gArg.excludedCount);
	check(gArg.excludedList,
	      "Cannot allocate memory for excluded path %s\n", path);
	gArg.excludedList[gArg.excludedCount - 2] = t;

	return;
}

typedef struct {
	entry_attr_t attr;
	const char *modeStr, *userName, *grpName, *ageStr, *arg;
} Process_File_In;

/*
 * The order in array ctx is important:
 *    attr,modeStr,userName,grpName,age,arg
 * All are in the same size as a normal pointer
 */
static void process_file(const char *path, void *ctx)
{
	typedef void (*Attr_Handler)(const char *path, const char *mode,
				     const char *userName, const char *grpName,
				     const char *age, const char *arg);
	static Attr_Handler attrHandler[] = {
	    [1] = attr_create,	[4] = attr_perm,      [5] = attr_createdir,
	    [8] = attr_write,	[9] = attr_ownership, [10] = attr_clean,
	    [11] = attr_remove, [12] = attr_attr,     [13] = attr_exclude,
	};

	Process_File_In *in = ctx;

	for (int i = 0, mask = 1; (size_t)i < (sizeof(in->attr) << 3) - 1;
	     i++) {
		if (in->attr & mask) {
			attrHandler[i](path, in->modeStr, in->userName,
				       in->grpName, in->ageStr, in->arg);
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
	static entry_attr_t attrTableSet[256] = {
	    ['w'] = ATTR_WRITE,
	    ['f'] = ATTR_CREATE | ATTR_WRITE | ATTR_OWNERSHIP | ATTR_PERM,
	    ['d'] = ATTR_CREATEDIR | ATTR_OWNERSHIP | ATTR_PERM | ATTR_CLEAN,
	    ['!'] = ATTR_ONBOOT,
	    ['r'] = ATTR_REMOVE | ATTR_GLOB,
	    ['D'] = ATTR_CREATEDIR | ATTR_OWNERSHIP | ATTR_PERM | ATTR_CLEAN |
		    ATTR_REMOVE,
	    ['q'] = ATTR_CREATEDIR | ATTR_OWNERSHIP | ATTR_PERM | ATTR_CLEAN,
	    ['Q'] = ATTR_CREATEDIR | ATTR_OWNERSHIP | ATTR_PERM | ATTR_CLEAN |
		    ATTR_REMOVE,
	    ['h'] = ATTR_ATTR | ATTR_GLOB,
	    ['x'] = ATTR_EXCLUDE,
	};
	// static entry_attr_t attrTableClear[256] = {
	// 	['+'] = ATTR_WRITE,  // FIXME: '+' is not like this
	// };

	while (!feof(conf)) {
		char *line = NULL;
		char *type = NULL, *path = NULL, *mode = NULL, *user = NULL,
		     *group = NULL, *age = NULL;

		size_t buf_len = 0;
		ssize_t line_len = getline(&line, &buf_len, conf);
		if (line_len < 0 || !line)
			break;

		int term_len = 0;
		int matched = sscanf(line, "%ms%ms%ms%ms%ms%ms%n", &type, &path,
				     &mode, &user, &group, &age, &term_len);
		if (matched < 0)
			break;

		const char *p = skip_space(type);
		if (p[0] == '#')
			continue;

		entry_attr_t attr = 0x00;
		for (; p[0]; p++) {
			uint8_t idx = p[0];
			if (!attrTableSet[idx])
				log_warn("Invalid type %c\n", idx);
			attr |= attrTableSet[idx];
			// attr &= ~attrTableClear[idx];
		}

		if ((attr & ATTR_ONBOOT) && !gArg.boot) // Handler '!'
			continue;

		char dummy[1] = "";
		Process_File_In in = {
		    .attr = attr & ~ATTR_ONBOOT & ~ATTR_GLOB,
		    .modeStr = mode ? mode : dummy,
		    .userName = user ? user : dummy,
		    .grpName = group ? group : dummy,
		    .ageStr = age ? age : dummy,
		    .arg = line + term_len,
		};
		if (attr & ATTR_GLOB) {
			glob_match(path, process_file, (void *)&in);
		} else {
			process_file(path, (void *)&in);
		}

		free_if(7, line, type, path, mode, user, group, age);
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
	gLogStream = stderr;
	int confIdx = 1;

	for (; confIdx < argc; confIdx++) {
		if (!strcmp(argv[confIdx], "--clean")) {
			gArg.clean = 1;
		} else if (!strcmp(argv[confIdx], "--create")) {
			gArg.create = 1;
		} else if (!strcmp(argv[confIdx], "--remove")) {
			gArg.remove = 1;
		} else if (!strcmp(argv[confIdx], "--boot")) {
			gArg.boot = 1;
		} else if (!strcmp(argv[confIdx], "--no-default")) {
			gArg.noDefault = 1;
		} else if (!strcmp(argv[confIdx], "--log")) {
			check(confIdx + 1 < argc,
			      "missing filename for option -l");

			FILE *t = fopen(argv[confIdx + 1], "a");
			if (!t)
				log_warn("Cannot open log file %s\n",
					 argv[confIdx + 1]);
			gLogStream = t;
			confIdx++;
		} else if (!strcmp(argv[confIdx], "--help") ||
			   !strcmp(argv[confIdx], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			break;
		}
	}

	gArg.excludedList = malloc(sizeof(char *));
	gArg.excludedCount = 1;
	gArg.excludedList[0] = NULL;
	check(gArg.excludedList, "Cannot allocate memory for excluded path");

	if (!gArg.noDefault) {
		iterate_directory("/etc/tmpfiles.d", read_conf, NULL, true);
		iterate_directory("/lib/tmpfiles.d", read_conf, NULL, true);
	}

	/* Now no options are recognised */
	for (; confIdx < argc; confIdx++)
		read_conf(argv[confIdx], NULL);

	fclose(gLogStream);
	free(gArg.excludedList);

	return 0;
}
