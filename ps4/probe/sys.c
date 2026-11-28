/**
 * The PS4 probes' runtime (sys.h). System call numbers are FreeBSD's, and
 * Orbis's own for the dynamic linker: 591 sys_dynlib_dlsym, 592
 * sys_dynlib_get_list, 594 sys_dynlib_load_prx.
 */

#include "sys.h"

int probeMain(void);

#ifndef SC_PRX
// The entry point: the payload loader jumps here, with no C runtime. A
// module (SC_PRX) has the system's loader and crtlib's _init instead.
__asm__(
    ".section .text.start, \"ax\"\n"
    ".global _start\n"
    "_start:\n"
    "  pushq %rbp\n"
    "  movq %rsp, %rbp\n"
    "  andq $-16, %rsp\n"
    "  callq probeEntry\n"
    "  movq %rbp, %rsp\n"
    "  popq %rbp\n"
    "  retq\n"
    ".text\n");
#endif

__attribute__((noinline)) long sysCall(long num, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long rax;
    // An error comes back with the carry set and errno in rax: as -errno.
    __asm__ volatile("syscall; jnc 1f; negq %%rax; 1:"
                     : "=a"(rax), "+D"(a), "+S"(b), "+d"(c), "+r"(r10), "+r"(r8), "+r"(r9)
                     : "a"(num)
                     : "rcx", "r11", "cc", "memory");
    return rax;
}

#ifdef SC_PRX
// In a module, which may run inside SceShellUI, through libkernel, as the
// projects that run modules there do (ps4-hen-rest: sceKernelOpen,
// sceKernelMkdir; OpenOrbis's libc "uses libkernel instead of syscalls for
// compatibility"). With system calls of its own the player never got as far
// as its log inside the shell.
int sceKernelOpen(const char* path, int flags, int mode);
long sceKernelWrite(int fd, const void* buf, size_t n);
long sceKernelRead(int fd, void* buf, size_t n);
int sceKernelClose(int fd);
int sceKernelMkdir(const char* path, int mode);
int sceKernelFsync(int fd);
int sceKernelUsleep(unsigned int us);
int sceKernelClockGettime(int clock, void* ts);
int sceKernelMmap(void* addr, size_t len, int prot, int flags, int fd, int64_t off, void** out);
int sceKernelMunmap(void* addr, size_t len);
int sceKernelUnlink(const char* path);
int sceKernelRename(const char* from, const char* to);
int64_t sceKernelLseek(int fd, int64_t off, int whence);
int sceKernelGetdents(int fd, char* buf, int n);

int sysOpen(const char* path, int flags, int mode) {
    return sceKernelOpen(path, flags, mode);
}
int sysWrite(int fd, const void* buf, size_t n) {
    return (int)sceKernelWrite(fd, buf, n);
}
int sysRead(int fd, void* buf, size_t n) {
    return (int)sceKernelRead(fd, buf, n);
}
int sysClose(int fd) {
    return sceKernelClose(fd);
}
int sysMkdir(const char* path, int mode) {
    return sceKernelMkdir(path, mode);
}
int sysFsync(int fd) {
    return sceKernelFsync(fd);
}
int sysExists(const char* path) {
    const int fd = sysOpen(path, SC_O_RDONLY, 0);
    if (fd < 0) return 0;
    sysClose(fd);
    return 1;
}
void sysSleepMs(uint32_t ms) {
    sceKernelUsleep(ms * 1000u);
}
uint64_t sysNowUs(void) {
    struct {
        int64_t sec, nsec;
    } t = {0, 0};
    sceKernelClockGettime(4 /* CLOCK_MONOTONIC */, &t);
    return (uint64_t)t.sec * 1000000u + (uint64_t)t.nsec / 1000u;
}
void* sysMap(size_t size) {
    void* p = NULL;
    const int rc = sceKernelMmap(NULL, size, 3 /* read, write */, 0x1000 | 0x2 /* anon, private */,
                                 -1, 0, &p);
    return rc < 0 ? NULL : p;
}
void sysUnmap(void* p, size_t size) {
    sceKernelMunmap(p, size);
}
int sysUnlink(const char* path) {
    return sceKernelUnlink(path);
}
int sysRename(const char* from, const char* to) {
    return sceKernelRename(from, to);
}
int64_t sysSeek(int fd, int64_t off, int whence) {
    return sceKernelLseek(fd, off, whence);
}
int sysGetdents(int fd, void* buf, size_t n) {
    return sceKernelGetdents(fd, (char*)buf, (int)n);
}
#else
int sysOpen(const char* path, int flags, int mode) {
    return (int)sysCall(5, (long)path, flags, mode, 0, 0, 0);
}
int sysWrite(int fd, const void* buf, size_t n) {
    return (int)sysCall(4, fd, (long)buf, (long)n, 0, 0, 0);
}
int sysRead(int fd, void* buf, size_t n) {
    return (int)sysCall(3, fd, (long)buf, (long)n, 0, 0, 0);
}
int sysClose(int fd) {
    return (int)sysCall(6, fd, 0, 0, 0, 0, 0);
}
int sysMkdir(const char* path, int mode) {
    return (int)sysCall(136, (long)path, mode, 0, 0, 0, 0);
}
int sysFsync(int fd) {
    return (int)sysCall(95, fd, 0, 0, 0, 0, 0);
}
int sysExists(const char* path) {
    const int fd = sysOpen(path, SC_O_RDONLY, 0);
    if (fd < 0) return 0;
    sysClose(fd);
    return 1;
}
void sysSleepMs(uint32_t ms) {
    struct {
        int64_t sec, nsec;
    } t = {ms / 1000, (int64_t)(ms % 1000) * 1000000};
    sysCall(240, (long)&t, 0, 0, 0, 0, 0);
}
uint64_t sysNowUs(void) {
    struct {
        int64_t sec, nsec;
    } t = {0, 0};
    sysCall(232, 4 /* CLOCK_MONOTONIC */, (long)&t, 0, 0, 0, 0);
    return (uint64_t)t.sec * 1000000u + (uint64_t)t.nsec / 1000u;
}
void* sysMap(size_t size) {
    const long p = sysCall(477, 0, (long)size, 3 /* read, write */, 0x1000 | 0x2 /* anon, private */,
                           -1, 0);
    return p < 0 && p > -4096 ? NULL : (void*)p;
}
void sysUnmap(void* p, size_t size) {
    sysCall(73, (long)p, (long)size, 0, 0, 0, 0);
}
int sysUnlink(const char* path) {
    return (int)sysCall(10, (long)path, 0, 0, 0, 0, 0);
}
int sysRename(const char* from, const char* to) {
    return (int)sysCall(128, (long)from, (long)to, 0, 0, 0, 0);
}
int64_t sysSeek(int fd, int64_t off, int whence) {
    return sysCall(478, fd, off, whence, 0, 0, 0);
}
int sysGetdents(int fd, void* buf, size_t n) {
    return (int)sysCall(272, fd, (long)buf, (long)n, 0, 0, 0);
}
#endif
void sysExit(int code) {
    sysCall(1, code, 0, 0, 0, 0, 0);
    for (;;) {
    }
}

void* memset(void* d, int c, size_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void* memcpy(void* d, const void* s, size_t n) {
    uint8_t* p = (uint8_t*)d;
    const uint8_t* q = (const uint8_t*)s;
    while (n--) *p++ = *q++;
    return d;
}
void* memmove(void* d, const void* s, size_t n) {
    uint8_t* p = (uint8_t*)d;
    const uint8_t* q = (const uint8_t*)s;
    if (p < q) {
        while (n--) *p++ = *q++;
    } else {
        while (n--) p[n] = q[n];
    }
    return d;
}
int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}
size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

// ── Libraries ────────────────────────────────────────────────────────────────
static int loadOne(const char* path) {
    int h = 0;
    const long rc = sysCall(594, (long)path, 0, (long)&h, 0, 0, 0);
    return rc >= 0 ? h : 0;
}

int libLoad(const char* name) {
    char path[128];
    fmt(path, sizeof(path), "/system/common/lib/%s", name);
    int h = loadOne(path);
    if (!h) h = loadOne(name);
    if (!h) {
        fmt(path, sizeof(path), "/system/priv/lib/%s", name);
        h = loadOne(path);
    }
    return h;
}

void* libSym(int handle, const char* symbol) {
    void* addr = 0;
    const long rc = sysCall(591, handle, (long)symbol, (long)&addr, 0, 0, 0);
    return rc >= 0 ? addr : NULL;
}

// A function of libkernel, from the module loaded under either name, else
// from any loaded module that has it.
__attribute__((unused)) static void* kernelSym(const char* symbol) {
    static int kernel;
    if (!kernel) kernel = libLoad("libkernel.sprx");
    if (!kernel) kernel = libLoad("libkernel_sys.sprx");
    void* p = kernel ? libSym(kernel, symbol) : NULL;
    if (p) return p;
    int handles[128], count = 0;
    if (sysCall(592, (long)handles, 128, (long)&count, 0, 0, 0) < 0) return NULL;
    for (int i = 0; i < count && i < 128; i++)
        if ((p = libSym(handles[i], symbol))) return p;
    return NULL;
}

// ── Formatting ───────────────────────────────────────────────────────────────
int fmtv(char* out, size_t cap, const char* f, va_list ap) {
    size_t n = 0;
#define PUT(c)                         \
    do {                               \
        if (n + 1 < cap) out[n] = (c); \
        n++;                           \
    } while (0)
    for (; *f; f++) {
        if (*f != '%') {
            PUT(*f);
            continue;
        }
        f++;
        int zero = 0, width = 0, lng = 0;
        if (*f == '0') {
            zero = 1;
            f++;
        }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l') {
            lng++;
            f++;
        }
        char buf[24];
        int len = 0;
        const char* s = buf;
        if (*f == 's') {
            s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            len = (int)strlen(s);
        } else if (*f == 'c') {
            buf[0] = (char)va_arg(ap, int);
            len = 1;
        } else if (*f == '%') {
            buf[0] = '%';
            len = 1;
        } else if (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x' || *f == 'X' || *f == 'p') {
            uint64_t v;
            int neg = 0;
            if (*f == 'p') {
                v = (uint64_t)va_arg(ap, void*);
            } else if (*f == 'd' || *f == 'i') {
                const int64_t sv = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
                neg = sv < 0;
                v = neg ? (uint64_t)(-sv) : (uint64_t)sv;
            } else {
                v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            }
            const int base = (*f == 'x' || *f == 'X' || *f == 'p') ? 16 : 10;
            const char* digits = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char rev[24];
            int k = 0;
            do {
                rev[k++] = digits[v % base];
                v /= base;
            } while (v);
            if (neg) buf[len++] = '-';
            if (*f == 'p') {
                buf[len++] = '0';
                buf[len++] = 'x';
            }
            while (k) buf[len++] = rev[--k];
        } else {
            PUT('%');
            PUT(*f);
            continue;
        }
        for (int pad = width - len; pad > 0; pad--) PUT(zero ? '0' : ' ');
        for (int i = 0; i < len; i++) PUT(s[i]);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
#undef PUT
    return (int)n;
}

int fmt(char* out, size_t cap, const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    const int n = fmtv(out, cap, f, ap);
    va_end(ap);
    return n;
}

// ── The log ──────────────────────────────────────────────────────────────────
static int g_log = -1;
static uint64_t g_began;

void logOpen(const char* name) {
    char path[128];
    g_began = sysNowUs();
#ifdef SC_PRX
    // A module may run inside the shell, which has /user/data, not /data.
    sysMkdir("/user/data/ShellCompanion", 0777);
    fmt(path, sizeof(path), "/user/data/ShellCompanion/%s", name);
#else
    sysMkdir("/data/ShellCompanion", 0777);
    fmt(path, sizeof(path), "/data/ShellCompanion/%s", name);
#endif
    g_log = sysOpen(path, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
}

void logLine(const char* f, ...) {
    char line[512];
    const uint64_t ms = (sysNowUs() - g_began) / 1000u;
    int n = fmt(line, sizeof(line), "%6u.%03u ", (unsigned)(ms / 1000u), (unsigned)(ms % 1000u));
    va_list ap;
    va_start(ap, f);
    n += fmtv(line + n, sizeof(line) - n, f, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    if (g_log >= 0) {
        sysWrite(g_log, line, n);
        sysFsync(g_log);  // a crash keeps what came before it
    }
}

// ── Notifications ────────────────────────────────────────────────────────────
// sceKernelSendNotificationRequest's request, as OneFamily sends it.
typedef struct {
    int32_t type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    uint8_t useIconImageUri;
    char message[1024];
    char iconUri[1024];
    char unk[1024];
} NotifyRequest;

#ifdef SC_PRX
// In a module the system's loader links these, from libkernel, as GoldHEN's
// plugins have them. Found by name at run time instead (kernelSym), the
// player's thread was never made: scePthreadCreate did not come back.
int sceKernelSendNotificationRequest(int device, void* req, size_t size, int blocking);
int scePthreadCreate(void* thread, const void* attr, void* (*fn)(void*), void* arg,
                     const char* name);
int sceKernelGetSystemSwVersion(void* out);
#endif

void notify(const char* f, ...) {
    typedef int (*Send)(int, NotifyRequest*, size_t, int);
    static Send send;
#ifdef SC_PRX
    if (!send) send = (Send)sceKernelSendNotificationRequest;
#else
    if (!send) send = (Send)kernelSym("sceKernelSendNotificationRequest");
#endif
    static NotifyRequest req;
    memset(&req, 0, sizeof(req));
    req.targetId = -1;
    va_list ap;
    va_start(ap, f);
    fmtv(req.message, sizeof(req.message), f, ap);
    va_end(ap);
    const int rc = send ? send(0, &req, sizeof(req), 0) : -1;
    logLine("notification \"%s\": 0x%08X", req.message, (unsigned)rc);
}

int threadStart(void* (*fn)(void*), void* arg, const char* name) {
    typedef int (*Create)(void* thread, const void* attr, void* (*)(void*), void*, const char*);
#ifdef SC_PRX
    Create create = (Create)scePthreadCreate;
#else
    Create create = (Create)kernelSym("scePthreadCreate");
#endif
    static uint64_t thread;
    return create ? create(&thread, NULL, fn, arg, name) : -1;
}

void firmware(char* out, size_t cap) {
    struct {
        uint64_t size;
        char text[0x1C];
        uint32_t version;
    } v;
    typedef int (*Get)(void*);
    memset(&v, 0, sizeof(v));
    v.size = sizeof(v);
#ifdef SC_PRX
    Get get = (Get)sceKernelGetSystemSwVersion;
#else
    Get get = (Get)kernelSym("sceKernelGetSystemSwVersion");
#endif
    const int rc = get ? get(&v) : -1;
    if (rc < 0)
        fmt(out, cap, "unknown (0x%08X)", (unsigned)rc);
    else
        fmt(out, cap, "%s (0x%08X)", v.text, v.version);
}

// What the entry calls: the probe, then back to the loader, as OneFamily's
// daemon returns. Not exit(): whether GoldHEN runs a payload in a process
// of its own or in one of the system's is not known here, and exit() in the
// latter would end it.
#ifndef SC_PRX
void probeEntry(void) {
    const int rc = probeMain();
    logLine("probe: returning %d to the loader", rc);
}
#endif
