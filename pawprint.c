/*
 *	pawprint
 *	File:/pawprint.c
 *	Date:2022.09.30
 *	By MIT License.
 *	Copyright (c) 2022 Ziyao.
 *	This project is a part of eweOS
 */

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<stdint.h>
#include<errno.h>

#include<unistd.h>
#include<sys/types.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<dirent.h>

#if defined(CONF_TARGET_X86_64)
	#define TARGET_PLATFORM "x86-64"
#elif defined(CONF_TARGET_I386)
	#define TARGET_PLATFORM "x86"
#else
	#error "No Target is Specified.Try to define CONF_TARGET_*"
#endif

static struct {
	int boot:1;
	int clean:1;
	int create:1;
	char *prefixList;
	int prefixCount;
	char *noPrefixList;
	int noPrefixCount;
} gArg;

#define s(k) (1 << (k))
#define ATTR_FILE		s(0)		// File or directory
#define ATTR_CREATE		s(1)		// Create or fail
#define ATTR_APPEND		s(2)		// Append or not
#define ATTR_ONBOOT		s(3)		// On --boot only
#define ATTR_PERM		s(4)		// Adjust permission
#define ATTR_GLOB		s(5)		// Need to be expand
#define ATTR_NOSYM		s(6)		// Do not follow symlink
#define ATTR_RECUR		s(7)		// Recusively
#define ATTR_WRITE		s(8)		// Write message

typedef uint32_t Entry_Attribute;

typedef struct {
	const char *path;
	Entry_Attribute attr;
} File_Entry;

/*	Log  Macros	*/
// For default,print log to stderr
FILE *gLogStream;
#define log_error(...) (fprintf(gLog,"[Error]:" __VA_ARGS__))
#define log_warn(...)  (fprintf(gLogStream,"[Warning]:"  __VA_ARGS__))
#define check(assertion,...) do {					\
	if (!(assertion)) {						\
		log_error(__VA_ARGS__);					\
		exit(-1);						\
	}								\
	while(0)
#define checkl(cond,log,...) ((void)(cond ? 0 : log_warn(log,__VA_ARGS__)))

static void iterate_dir(const char *path,void (*callback)(const char *path,
							  void *ctx),
			void *ctx)
{
	DIR *root = opendir(path);
	if (!root) {
		log_warn("Cannot open directory %s\n",path);
		return;
	}

	for (struct dirent *dir = readdir(root);dir;dir = readdir(root))
		callback(dir->d_name,ctx);

	closedir(root);

	return;
}

static const char *skip_space(const char *p)
{
	while ((*p == ' ' || *p == '\t') && *p)
		p++;
	return p;
}

static void next_line(FILE *fp)
{
	while (fgetc(fp) != '\n');
	return;
}

static int is_valid_file(const char *path)
{
	struct stat t;
	return !stat(path,&t);
}

#define def_handler(name) static void name (const char *path,const char *mode,\
					    const char *userName,	      \
					    const char *grpName,	      \
					    const char *age,const char *arg)
#define handler_ignore (void)path;(void)mode;(void)userName;(void)grpName;    \
		       (void)age;(void)arg

def_handler(attr_write)
{
	handler_ignore;

	if (is_valid_file(path)) {
		int fd = open(path,O_WRONLY | O_TRUNC);
		if (fd < 0) {
			log_warn("Cannot open file %s\n",path);
			return;
		}
		ssize_t length = strlen(arg);
		for (ssize_t s = write(fd,arg,strlen(arg));
		     s && length;
		     s = write(fd,arg,length)) {
			if (s <= 0) {
				log_warn("Cannot write to file %s\n",path);
				close(fd);
				return;
			}
			length -= s;
		}
		close(fd);
	}
	return;
}

/*
 *	NOTICE: Glob match will be done in parse_conf()
 */
static void parse_conf(FILE *conf)
{
	static Entry_Attribute attrTableSet[256] = {
			['w']	= ATTR_WRITE,
			['+']	= ATTR_APPEND,
			['!']	= ATTR_ONBOOT,
		};
	static Entry_Attribute attrTableClear[] = {
			['+']	= ATTR_WRITE
		};

	typedef void (*Attr_Handler)(const char *path,const char *mode,
				     const char *userName,const char *grpName,
				     const char *age,const char *arg);
	static Attr_Handler attrHandler[] =
		{
			[8]	= attr_write,
		};

	while (!feof(conf)) {
		char *typeStr,*pathStr,*modeStr,*userName,*grpName;
		char *ageStr;
		int termNum = fscanf(conf,"%ms%ms%ms%ms%ms%ms",
				     &typeStr,&pathStr,&modeStr,
				     &userName,&grpName,&ageStr);

		if (termNum < 0)
			break;

		const char *p = skip_space(typeStr);
		if (p[0] == '#') {		// Comment
			next_line(conf);
			continue;
		}

		char arg[256];	// FIXME: Fixed max length
		const char *ret = fgets(arg,256,conf);
		if (!ret)
			break;
		size_t argLength = strlen(ret);
		if (arg[argLength- 1] == '\n') {
			arg[argLength - 1] = '\0';
			argLength--;
		}

		Entry_Attribute attr = 0x00;
		for (int i = 0;p[i];p++) {
			if (!attrTableSet[(int)p[i]])
				log_warn("Invalid type %c\n",p[i]);
			attr |= attrTableSet[(int)p[i]];
			attr &= ~attrTableClear[(int)p[i]];
		}

		for (int i = 0,mask = 1;
		     (size_t)i < (sizeof(attr) << 3) - 1;
		     i++) {
			if (attr & mask) {
				attrHandler[i](pathStr,userName,grpName,
					       modeStr,ageStr,skip_space(arg));
			}
			mask <<= 1;
		}

		free(typeStr);
		free(pathStr);
		free(modeStr);
		free(userName);
		free(grpName);
		free(ageStr);
	}
	return;
}

static void read_conf(const char *path,void *ctx)
{
	(void)ctx;
	FILE *conf = fopen(path,"r");
	if (!conf) {
		log_warn("Cannot open configuration file %s\n",path);
		return;
	}

	parse_conf(conf);
	fclose(conf);
	return;
}

static void usage(const char *name)
{
	fprintf(stderr,"%s:\n\t%s ",name,name);
	fputs("\n",stderr);
	return;
}

int main(int argc,const char *argv[])
{
	if (argc == 1) {
		usage(argv[0]);
		return -1;
	}
	(void)iterate_dir;
	(void)gArg;
	gLogStream = stderr;
	int confIdx = 1;
	/*	Now no options are recognised	*/
	for (;confIdx < argc;confIdx++)
		read_conf(argv[confIdx],NULL);

	return 0;
}
