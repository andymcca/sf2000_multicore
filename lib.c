#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "debug.h"
#include "stockfw.h"
#include "dirent.h"

// NOTE: cache flushing for a specific memory range is currently not stable!
/*
void cache_flush(void *addr, size_t size)
{
	uintptr_t idx;
	uintptr_t begin = (uintptr_t)addr;
	uintptr_t end = begin + size - 1;

    // Hit_Writeback_D
    for (idx = begin; idx <= end; idx += 16)
        asm volatile("cache 0x19, 0(%0)" : : "r"(idx));
    asm volatile("sync 0; nop; nop");
    // Hit_Invalidate_I
    for (idx = begin; idx <= end; idx += 16)
        asm volatile("cache 0x10, 0(%0)" : : "r"(idx));
    asm volatile("nop; nop; nop; nop; nop; nop; nop; nop"); // ehb may be nop on this core
}*/

void full_cache_flush()
{
	unsigned idx;

	// Index_Writeback_Inv_D
	for (idx = 0x80000000; idx <= 0x80004000; idx += 16) // all of D-cache
		asm volatile("cache 1, 0(%0); cache 1, 0(%0)" : : "r"(idx));

	asm volatile("sync 0; nop; nop");

	// Index_Invalidate_I
	for (idx = 0x80000000; idx <= 0x80004000; idx += 16) // all of I-cache
		asm volatile("cache 0, 0(%0); cache 0, 0(%0)" : : "r"(idx));

	asm volatile("nop; nop; nop; nop; nop"); // ehb may be nop on this core
}

// a call to this function is generated by gcc when __builtin__clear_cache is used
void _flush_cache(void *buf, size_t sz, int flags)
{
	// note: params are ignored and *all* the cache is cleared instead.
	// this seems to produce the most stable behavior for running dynarec code.

	full_cache_flush();
}

/* ALi's libc lacks newer POSIX stuff, */
char *stpcpy(char *dst, const char *src)
{
	size_t sz = strlen(src) + 1;

	memcpy(dst, src, sz);
	return &dst[sz - 1];
}

// TODO: this __locale_ctype_ptr collide with the one from libc
// maybe defined it with attribute((weak)) or delete it all together
//
///* localization, */
// extern char _ctype_[257];

// const char *__locale_ctype_ptr (void)
// {
	// return _ctype_;
// }

/* reentrant functions (newer builds fixed that), */
extern int g_errno;

int *__errno(void)
{
	return &g_errno;
}

/* but also some ages-old ISO/ANSI stuff, too! */
int puts(const char *s)
{
	return (printf("%s\n", s) < 0 ? EOF : '\n');
}

/* ALi violated ISO/ANSI with fseek taking off_t */
int fseek(FILE *stream, long offset, int whence)
{
	return fseeko(stream, offset, whence);
}

void rewind(FILE *stream)
{
	fseeko(stream, 0, SEEK_SET);
}

void setbuf(FILE *stream, char *buffer)
{
	// ignore calls setbuf
}

// stock fw_fread returns the wrong value on success
// it should return the count of elements, but it returns the number of bytes instead
size_t fread(void *ptr, size_t size, size_t count, FILE *stream)
{
	size_t ret = fw_fread(ptr, size, count, stream);
	// TODO: check if this is correct for all cases
	return ret / size;
}

int fgetc(FILE *stream)
{
	unsigned char c;
	size_t n = fread(&c, 1, 1, stream);
	if (n == 1)
		return c;
	else
		return EOF;
}

int vfprintf(FILE *stream, const char *format, va_list _args)
{
	va_list args;
	va_copy(args, _args);

	// determine the required size for the formatted string
	int str_size = vsnprintf(NULL, 0, format, args);
	va_end(args);

	if (str_size < 0)
		return -1;

	size_t buf_size = str_size + 1;		// +1 for null terminator

	char* buffer = (char*)malloc(buf_size);
	if (buffer == NULL)
		return -1;

	va_copy(args, _args);
	vsnprintf(buffer, buf_size, format, args);
	va_end(args);

	size_t written;

	if (stream == stdout || stream == stderr)
	{
		xlog(buffer);
		written = 1;
	}
	else
		written = fwrite(buffer, str_size, 1, stream);

	free(buffer);

	if (written != 1)
		return -1;

	return str_size;
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int ret = vfprintf(stream, format, args);
	va_end(args);

	return ret;
}

int fputc(int character, FILE *stream)
{
    return fprintf(stream, "%c", character);
}

int fputs(const char *str, FILE *stream)
{
    return fprintf(stream, "%s", str);
}

typedef struct {
	union {
		struct {
			uint8_t	_1[0x18];	// type is at offset 0x18
			uint32_t type;		// 0x81b6 - file,	0x41ff - dir
		};
		struct {
			uint8_t	_2[0x38];	// size is at offset 0x38
			uint32_t size;		// filesize if type is a file
		};
		uint8_t	__[160];		// total struct size is 160
	};
} fs_stat_t;

static int stat_common(int ret, fs_stat_t *buffer, struct stat *sbuf)
{
	if (ret == 0)
	{
		memset(sbuf, 0, sizeof(*sbuf));
		sbuf->st_mode = S_ISREG(buffer->type)*S_IFREG | S_ISDIR(buffer->type)*S_IFDIR | (S_IRUSR|S_IWUSR);
		sbuf->st_size = buffer->size;
		return 0;
	}
	else
		return -1;
}

// wrap fs_stat to supply a more standard stat implementation
// for now only `type` (for dir or file) and `size` fields of `struct stat` are filled
int	stat(const char *path, struct stat *sbuf)
{
	fs_stat_t buffer = {0};
	int ret = fs_stat(path, &buffer);
	return stat_common(ret, &buffer, sbuf);
}

int	fstat(int fd, struct stat *sbuf)
{
	fs_stat_t buffer = {0};
	int ret = fs_fstat(fd, &buffer);
	return stat_common(ret, &buffer, sbuf);
}

int access(const char *path, int mode)
{
    struct stat buffer;
    return stat(path, &buffer);
}

int mkdir(const char *path, mode_t mode)
{
	return fs_mkdir(path, mode);
}

char *getcwd(char *buf, size_t size)
{
	return NULL;
}
int chdir(const char *path)
{
	return -1;
}
int rmdir(const char *path)
{
	return -1;
}
int unlink(const char *path)
{
	return -1;
}

int chmod(const char *path, mode_t mode)
{
	return -1;
}

int kill(pid_t pid, int sig)
{
	return -1;
}

pid_t getpid(void)
{
	return 1;
}

void abort(void)
{
	unsigned ra;
	asm volatile ("move %0, $ra" : "=r" (ra));
	lcd_bsod("abort() called from 0x%08x", ra);
}

void exit(int status)
{
	unsigned ra;
	asm volatile ("move %0, $ra" : "=r" (ra));
	lcd_bsod("exit(%d) called from 0x%08x", status, ra);
}

/* wrappers were not compiled in, but vfs drivers likely support these ops */
int remove(const char *path)
{
	return -1;
}

int rename(const char *old, const char* new)
{
	return -1;
}

int isatty(int fd)
{
    return 0;
}

clock_t clock(void)
{
	// clock function should return cpu clock ticks, so since os_get_tick_count() returns milliseconds,
	// we devide by 1000 to get the seconds and multiply by CLOCKS_PER_SEC to get the clock ticks.
    return (clock_t)(os_get_tick_count() * CLOCKS_PER_SEC / 1000);
}

DIR *opendir(const char *path)
{
	int fd = fs_opendir(path);
	if (fd < 0)
		return NULL;

	return (DIR*)(fd + 1);
}

int closedir(DIR *dir)
{
	int fd = (int)dir - 1;
	if (fd < 0)
		return -1;

	return fs_closedir(fd);
}

struct dirent *readdir(DIR *dir)
{
	int fd = (int)dir - 1;
	if (fd < 0)
		return NULL;

	struct {
		union {
			struct {
				uint8_t _1[0x10];
				uint32_t type;
			};
			struct {
				uint8_t _2[0x22];
				char    d_name[0x225];
			};
			uint8_t __[0x428];
		};
	} buffer = {0};

	if (fs_readdir(fd, &buffer) < 0)
		return NULL;

	// TODO: not thread safe
	static struct dirent d;

	d.d_type = S_ISREG(buffer.type)*DTYPE_FILE | S_ISDIR(buffer.type)*DTYPE_DIRECTORY;
	strncpy(d.d_name, buffer.d_name, sizeof(d.d_name));
	return &d;
}
