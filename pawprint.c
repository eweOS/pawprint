/*
 *	pawprint
 *	File:/pawprint.c
 *	Date:2022.10.07
 *	By MIT License.
 *	Copyright (c) 2022 Ziyao.
 *	This project is a part of eweOS
 */

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<stdint.h>
#include<errno.h>
#include<time.h>
#include<stdbool.h>

#include<unistd.h>
#include<sys/types.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<dirent.h>
#include<pwd.h>
#include<grp.h>
#include<glob.h>

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
	int remove:1;
	char *prefixList;
	int prefixCount;
	char *noPrefixList;
	int noPrefixCount;
} gArg;

/*
 *	NOTE:
 *		Remember to check parse_conf if these macros are changed
 *		Handlers are executed from the lowest bit to the highest
 */
#define s(k) (1 << (k))
#define ATTR_FILE		s(0)		// File or directory
#define ATTR_CREATE		s(1)		// Create or fail
#define ATTR_APPEND		s(2)		// Append or not

#define ATTR_PERM		s(4)		// Adjust permission
#define ATTR_CREATEDIR		s(5)		// Create directory
#define ATTR_NOSYM		s(6)		// Do not follow symlink
#define ATTR_RECUR		s(7)		// Recusively
#define ATTR_WRITE		s(8)		// Write message
#define ATTR_OWNERSHIP		s(9)		// Adjust ownership
						// both group and user
#define ATTR_CLEAN		s(10)		// Need to be cleaned
#define ATTR_REMOVE		s(11)		// Need to be removed
#define ATTR_ONBOOT		s(30)		// On --boot only
#define ATTR_GLOB		s(31)		// Need to be expanded

typedef uint32_t Entry_Attribute;

typedef struct {
	const char *path;
	Entry_Attribute attr;
} File_Entry;

/*	Log  Macros	*/
// For default,print log to stderr
FILE *gLogStream;
#define log_error(...) (fprintf(gLogStream,"[Error]:" __VA_ARGS__))
#define log_warn(...)  (fprintf(gLogStream,"[Warning]:"  __VA_ARGS__))
#define check(assertion,...) do {					\
	if (!(assertion)) {						\
		log_error(__VA_ARGS__);					\
		exit(-1);						\
	}								\
	while(0)
#define checkl(cond,log,...) ((void)(cond ? 0 : log_warn(log,__VA_ARGS__)))

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

static void free_if(int num,...)
{
	va_list arg;
	va_start(arg,num);
	void *p;

	while (num) {
		p = va_arg(arg,void *);
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
	return !stat(path,&t);
}

/*
 *	NOTE: Assuming that the file exists
 */
static time_t get_last_time(const char *path)
{
	struct stat t;

	stat(path,&t);

	time_t lastChange = t.st_atim.tv_sec > t.st_mtim.tv_sec ?
					t.st_atim.tv_sec : t.st_mtim.tv_sec;
	return lastChange > t.st_ctim.tv_sec ? lastChange : t.st_ctim.tv_sec;
}

static int is_directory(const char *path)
{
	struct stat t;

	if (stat(path,&t)) {
		log_warn("Cannot get the status of file %s",path);
		return 0;
	}

	return S_ISDIR(t.st_mode);
}

static void iterate_directory_sub(const char *path,
				  void (*callback)(const char *path,void *ctx),
				  void *ctx,bool r,bool top)
{
	DIR *root = opendir(path);
	if (!root) {
		log_warn("Cannot open directory %s\n",path);
		return;
	}
	chdir(path);

	for (struct dirent *dir = readdir(root);dir;dir = readdir(root)) {
		if (dir->d_name[0] == '.')
			continue;

		if (is_directory(dir->d_name) && r) {
			iterate_directory_sub(dir->d_name,callback,
					      ctx,true,false);
		} else {
			callback(dir->d_name,ctx);
		}
	}

	closedir(root);
	chdir("..");

	if (!top)
		callback(path,ctx);

	return;
}

static void iterate_directory(const char *path,
			      void (*callback)(const char *path,void *ctx),
			      void *ctx,bool r)
{
	iterate_directory_sub(path,callback,ctx,r,true);
}

static void glob_match(const char *pattern,
		       void (*callback)(const char *path,void *ctx),
		       void *ctx)
{
	glob_t buf;

	if (glob(pattern,GLOB_NOSORT,NULL,&buf)) {
		log_warn("Cannot match files with glob %s\n",pattern);
		return;
	}

	for (size_t i = 0;i < buf.gl_pathc;i++)
		callback(buf.gl_pathv[i],ctx);

	globfree(&buf);

	return;
}

#define def_handler(name) static void name (const char *path,const char *mode,\
					    const char *userName,	      \
					    const char *grpName,	      \
					    const char *age,const char *arg)
#define handler_ignore (void)path;(void)mode;(void)userName;(void)grpName;    \
		       (void)age;(void)arg;

static time_t convert_age(const char *s)
{
	if (!s[0] || s[0] == '-') 		// Skip
		return (time_t)-1;

	time_t t = 0;
	for (time_t scale = strtol(s,(char **)&s,0);
	     *s;scale = strtol(s,(char **)&s,0)) {
		time_t unit = *s == 'd' ? 86400		:
			      *s == 'w' ? 604800	:
			      *s == 'h' ? 3600		:
			      *s == 'm' ? 60		:
			      *s == 's' ? 1		:
					  0;
		if (!unit)
			return (time_t)-1;
		t += unit * scale;
		s++;
	}

	return t;
}

static void clean_file(const char *path,void *ctx)
{
	time_t ddl = *(time_t*)ctx;
	if (get_last_time(path) < ddl) {
		if (remove(path))
			log_warn("Cannot remove file %s\n",path);
	}
	return;
}

def_handler(attr_clean)
{
	handler_ignore;

	if (!gArg.clean)
		return;

	time_t ddl = time(NULL) - convert_age(age);
	iterate_directory(path,clean_file,&ddl,true);

	return;
}

def_handler(attr_createdir)
{
	handler_ignore;

	if (!is_valid_file(path)) {
		if (mkdir(path,0777))
			log_warn("Cannot create directory %s\n",path);
	}
	return;
}

def_handler(attr_create)
{
	handler_ignore;

	if (!is_valid_file(path)) {
		int fd = open(path,O_CREAT | O_WRONLY);
		if (fd < 0) {
			log_warn("Cannot create file %s\n",path);
			return;
		}
		close(fd);
	}
	return;
}

def_handler(attr_perm)
{
	handler_ignore;

	if (mode[0] == '-' || !mode[0])		// Simply ignore
		return;

	if (chmod(path,(mode_t)strtol(mode,NULL,8)))
		log_warn("Cannot set file mode as %s for %s\n",mode,path);

	return;
}

static void adjust_user(const char *path,const char *user)
{
	if (user[0] == '-' || !user[0])		// Ignore
		return;

	struct passwd *pswd = getpwnam(user);
	if (!pswd) {
		log_warn("Invalid user %s\n",user);
		return;
	}

	if (chown(path,pswd->pw_uid,-1))
		log_warn("Cannot transfer file %s to user %s",path,user);
	return;
}

static void adjust_group(const char *path,const char *group)
{
	if (group[0] == '-' || !group[0])
		return;

	struct group *grpInfo = getgrnam(group);
	if (!grpInfo) {
		log_warn("Invalid group %s\n",group);
		return;
	}

	if (chown(path,-1,grpInfo->gr_gid))
		log_warn("Cannot transfer file %s to group %s\n",path,group);
	return;
}

def_handler(attr_ownership)
{
	handler_ignore;
	adjust_user(path,userName);
	adjust_group(path,grpName);
	return;
}

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

static void do_remove(const char *path,void *in)
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
		iterate_directory(path,do_remove,NULL,true);
	} else {
		do_remove(path,NULL);
	}

	return;
}

typedef struct {
	Entry_Attribute attr;
	const char *modeStr,*userName,*grpName,*ageStr,*arg;
} Process_File_In;

/*
 *	The order in array ctx is important:
 *		attr,modeStr,userName,grpName,age,arg
 *	All are in the same size as a normal pointer
 */
static void process_file(const char *path,void *ctx)
{
	typedef void (*Attr_Handler)(const char *path,const char *mode,
				     const char *userName,const char *grpName,
				     const char *age,const char *arg);
	static Attr_Handler attrHandler[] =
		{
			[1]	= attr_create,
			[4]	= attr_perm,
			[5]	= attr_createdir,
			[8]	= attr_write,
			[9]	= attr_ownership,
			[10]	= attr_clean,
			[11]	= attr_remove,
		};

	Process_File_In *in = ctx;

	for (int i = 0,mask = 1;
	     (size_t)i < (sizeof(in->attr) << 3) - 1;
	     i++) {
		if (in->attr & mask) {
			attrHandler[i](path,in->modeStr,in->userName,
				       in->grpName,in->ageStr,in->arg);
		}
		mask <<= 1;
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
			['f']	= ATTR_CREATE | ATTR_WRITE | ATTR_OWNERSHIP |
				  ATTR_PERM,
			['d']	= ATTR_CREATEDIR | ATTR_OWNERSHIP | ATTR_PERM |
				  ATTR_CLEAN,
			['!']	= ATTR_ONBOOT,
			['r']	= ATTR_REMOVE | ATTR_GLOB,
			['D']	= ATTR_CREATEDIR | ATTR_OWNERSHIP | ATTR_PERM |
				  ATTR_CLEAN | ATTR_REMOVE,
		};
	static Entry_Attribute attrTableClear[] = {
			['+']	= ATTR_WRITE,
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
		fgets(arg,256,conf);
		size_t argLength = strlen(arg);
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

		if ((attr & ATTR_ONBOOT) && !gArg.boot)	// Handler '!'
			continue;

		char dummy[1] = "";
		Process_File_In in = {
					.attr		= attr & ~ATTR_ONBOOT &
							  ~ATTR_GLOB,
					.modeStr	= modeStr ? modeStr :
								    dummy,
					.userName	= userName ? userName :
								     dummy,
					.grpName	= grpName ? grpName :
								    dummy,
					.ageStr		= ageStr ? ageStr :
								   dummy,
					.arg		= skip_space(arg),
				     };
		if (attr & ATTR_GLOB) {
			glob_match(pathStr,process_file,(void*)&in);
		} else {
			process_file(pathStr,(void*)&in);
		}

		free_if(6,typeStr,pathStr,modeStr,userName,grpName,ageStr);
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
	(void)get_last_time;
	if (argc == 1) {
		usage(argv[0]);
		return -1;
	}
	memset(&gArg,255,sizeof(gArg));
	(void)gArg;
	gLogStream = stderr;
	int confIdx = 1;
	/*	Now no options are recognised	*/
	for (;confIdx < argc;confIdx++)
		read_conf(argv[confIdx],NULL);

	return 0;
}
