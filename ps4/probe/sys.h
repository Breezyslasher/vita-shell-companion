/**
 * The PS4 probes' own runtime: what an ELF payload sent to GoldHEN needs
 * with nothing resolved for it at load time. System calls by number, system
 * libraries loaded and their functions found by name (sys_dynlib_load_prx,
 * sys_dynlib_dlsym), a log written a line at a time and synced, so that a
 * probe that crashes still leaves what came before, and the system's own
 * notifications. No libc: the probes are freestanding. The approach follows
 * OneFamily's daemon (ChiricoG/OneFamily, MIT), which runs this way on
 * GoldHEN.
 */

#ifndef SC_PS4_SYS_H
#define SC_PS4_SYS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

long sysCall(long num, long a, long b, long c, long d, long e, long f);

int sysOpen(const char* path, int flags, int mode);
int sysWrite(int fd, const void* buf, size_t n);
int sysRead(int fd, void* buf, size_t n);
int sysClose(int fd);
int sysMkdir(const char* path, int mode);
int sysFsync(int fd);
int sysExists(const char* path);
void sysSleepMs(uint32_t ms);
uint64_t sysNowUs(void);
void* sysMap(size_t size);  // anonymous read/write memory, or NULL
void sysExit(int code);

#define SC_O_RDONLY 0x0000
#define SC_O_WRONLY 0x0001
#define SC_O_CREAT 0x0200
#define SC_O_TRUNC 0x0400

// A system library loaded (by path, then by name): its handle, or 0.
int libLoad(const char* name);
// A function of it by name, or NULL.
void* libSym(int handle, const char* symbol);

void* memset(void* d, int c, size_t n);
void* memcpy(void* d, const void* s, size_t n);
size_t strlen(const char* s);
int fmtv(char* out, size_t cap, const char* f, va_list ap);
int fmt(char* out, size_t cap, const char* f, ...) __attribute__((format(printf, 3, 4)));

// The log: /data/ShellCompanion/<name>, a new one each run; every line
// carries the ms since the probe began.
void logOpen(const char* name);
void logLine(const char* f, ...) __attribute__((format(printf, 1, 2)));

// A system notification, at the top left of the screen.
void notify(const char* f, ...) __attribute__((format(printf, 1, 2)));

// The firmware, as libkernel gives it ("11.00"), into out.
void firmware(char* out, size_t cap);

#endif
