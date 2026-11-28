/**
 * Shell Companion's PS4 client (shell_companion.h). Apache-2.0.
 *
 * The files it shares with the modules (ps4/bgm/bgm.h): the app, jailbroken,
 * sees /data/ShellCompanion/ps4 as /user/data/ShellCompanion/ps4, the path
 * the shell has, which is used here throughout.
 */

#include "shell_companion.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../bgm/bgm.h"

#define PLUGINS "/user/data/GoldHEN/plugins"
#define WATCH_BEAT SC_BGM_DIR "/mcw-beat.txt"
#define EBGM SC_BGM_DIR "/ebgm"

static char g_titleId[16];
static char g_name[64];

// One caller at a time in the calls that use static buffers (two threads of
// the app would garble each other's results): a spin lock, libc only.
static int g_lock;
static void lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) usleep(1000);
}
static void unlock(void) {
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

// ── System calls: GoldHEN's (500) and the clock ──────────────────────────────

__attribute__((noinline)) static long scSys(long num, long a, long b, long c) {
    long rax;
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile("syscall; jnc 1f; negq %%rax; 1:"
                     : "=a"(rax), "+D"(a), "+S"(b), "+d"(c), "+r"(r10), "+r"(r8), "+r"(r9)
                     : "a"(num)
                     : "rcx", "r11", "cc", "memory");
    return rax;
}

// The monotonic clock in ms: the modules' heartbeats are in it.
static uint64_t nowMs(void) {
    struct {
        int64_t sec, nsec;
    } t = {0, 0};
    scSys(232, 4 /* CLOCK_MONOTONIC */, (long)&t, 0);
    return (uint64_t)t.sec * 1000u + (uint64_t)t.nsec / 1000000u;
}

static struct {
    uint32_t uid, ruid, rgid, groups;
    uint64_t paid;
    uint64_t caps[2];
    void* prison;
    void *cdir, *jdir, *rdir;
} g_jb;  // GoldHEN SDK's jailbreak_backup

struct ScPrxLoad {
    char processName[32];
    char prxPath[100];
    uint64_t res;
} __attribute__((packed));  // GoldHEN SDK's proc_prx_load

// ── Files ────────────────────────────────────────────────────────────────────

static void note(char* what, size_t size, const char* f, ...) __attribute__((format(printf, 3, 4)));
static void note(char* what, size_t size, const char* f, ...) {
    if (!what || !size) return;
    const size_t at = strlen(what);
    if (at + 2 >= size) return;
    if (at) what[at] = ' ';
    va_list ap;
    va_start(ap, f);
    vsnprintf(what + at + (at ? 1 : 0), size - at - (at ? 1 : 0), f, ap);
    va_end(ap);
}

static int readAll(const char* path, char* out, size_t cap) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t n = 0;
    ssize_t k;
    while (n + 1 < cap && (k = read(fd, out + n, cap - 1 - n)) > 0) n += (size_t)k;
    close(fd);
    out[n] = 0;
    return (int)n;
}

// Written whole beside path, then moved in.
static int writeAll(const char* path, const char* data, size_t size) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n < size) {
        const ssize_t k = write(fd, data + n, size - n);
        if (k <= 0) break;
        n += (size_t)k;
    }
    close(fd);
    chmod(tmp, 0666);
    if (n != size || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int copyFile(const char* from, const char* to) {
    const int in = open(from, O_RDONLY);
    if (in < 0) return -1;
    static char buf[256 * 1024];
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.new", to);
    const int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out < 0) {
        close(in);
        return -1;
    }
    ssize_t k;
    int ok = 1;
    while ((k = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)k) != k) ok = 0;
    close(in);
    close(out);
    chmod(tmp, 0777);  // the shell loads a module only if it may run it
    if (!ok || rename(tmp, to) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

// key=value's value in a file's text, into out: 1 if there.
static int field(const char* text, const char* key, char* out, size_t cap) {
    const size_t n = strlen(key);
    for (const char* p = text; *p;) {
        if (!strncmp(p, key, n) && p[n] == '=') {
            p += n + 1;
            size_t k = 0;
            while (*p && *p != '\n' && k + 1 < cap) out[k++] = *p++;
            out[k] = 0;
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (cap) out[0] = 0;
    return 0;
}

static int beatFresh(const char* path) {
    char text[4096], v[32];
    if (readAll(path, text, sizeof(text)) <= 0 || !field(text, "beat_ms", v, sizeof(v))) return 0;
    // Signed: a beat a little ahead of now (written between the two
    // reads) is fresh; one from before this boot reads far ahead, or long ago.
    const int64_t age = (int64_t)(nowMs() - strtoull(v, NULL, 10));
    return age > -1000 && age < 3000;
}

// ── Setting up ───────────────────────────────────────────────────────────────

int scInit(const char* titleId, const char* name) {
    if (!titleId || !titleId[0] || !name) return -1;
    snprintf(g_titleId, sizeof(g_titleId), "%s", titleId);
    snprintf(g_name, sizeof(g_name), "%s", name);
    return 0;
}

int scPlayerRunning(void) {
    return beatFresh(SC_BGM_STATUS);
}

int scWatchRunning(void) {
    return beatFresh(WATCH_BEAT);
}

// The Music section's first answers that are not the player's (its account
// and session link: ps4/README.md), unless ebgm/keep is there.
static void writeEbgm(void) {
    mkdir(EBGM, 0777);
    if (access(EBGM "/keep", F_OK) == 0) return;
    static const char profile[] =
        "{\"spotify\":{\"subscriptionType\":\"PREMIUM\",\"legalCountry\":\"US\"}}\n";
    static const char session[] =
        "{\"id\":\"shellcompanion\",\"token\":\"shellcompanion\","
        "\"access_token\":\"shellcompanion\",\"expires_in\":3600}\n";
    writeAll(EBGM "/me_partner_profile.json", profile, sizeof(profile) - 1);
    writeAll(EBGM "/sp_sessionlink.json", session, sizeof(session) - 1);
}

int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, const void* opt,
                             int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addr);

// The app's user (its initial user), for the shell's calls that want one
// (the PS4's download service: inside the shell no user could be had):
// user.txt.
static void writeUser(void) {
    const int h = sceKernelLoadStartModule("libSceUserService.sprx", 0, NULL, 0, NULL, NULL);
    if (h < 0) return;
    int (*init)(const void*) = NULL;
    int (*initial)(int*) = NULL;
    sceKernelDlsym(h, "sceUserServiceInitialize", (void**)&init);
    sceKernelDlsym(h, "sceUserServiceGetInitialUser", (void**)&initial);
    if (!initial) return;
    if (init) {
        const int params[1] = {700};
        init(params);  // an error if the app has: fine
    }
    int user = -1;
    if (initial(&user) != 0 || user == -1) return;
    char text[32];
    const int n = snprintf(text, sizeof(text), "user=%d\n", user);
    writeAll(SC_BGM_DIR "/user.txt", text, (size_t)n);
}

// A module into SceShellUI: from the app's package, else the copy in
// GoldHEN's plugins folder, which is where the shell will take it from.
static int loadModule(const char* name, char* what, size_t size) {
    char pkg[160], dest[160];
    snprintf(pkg, sizeof(pkg), "/mnt/sandbox/pfsmnt/%s-app0/%s", g_titleId, name);
    snprintf(dest, sizeof(dest), PLUGINS "/%s", name);
    mkdir("/user/data/GoldHEN", 0777);
    mkdir(PLUGINS, 0777);
    if (copyFile(pkg, dest) != 0 && access(dest, R_OK) != 0) {
        note(what, size, "%s: not in the package (%s) nor in " PLUGINS ".", name, pkg);
        return -1;
    }
    static struct ScPrxLoad args;
    memset(&args, 0, sizeof(args));
    snprintf(args.processName, sizeof(args.processName), "SceShellUI");
    snprintf(args.prxPath, sizeof(args.prxPath), "%s", dest);
    args.res = ~0ULL;
    const long rc = scSys(500, 6 /* PROCESS_PRX_LOAD */, (long)&args, 0);
    if (rc != 0 || (int32_t)args.res < 0) {
        note(what, size, "%s: the shell would not load it (0x%lX, 0x%lX).", name, (unsigned long)rc,
             (unsigned long)args.res);
        return -1;
    }
    note(what, size, "%s loaded.", name);
    return 0;
}

static int waitFor(int (*running)(void), unsigned ms) {
    for (unsigned w = 0; w < ms; w += 100) {
        if (running()) return 1;
        usleep(100000);
    }
    return running();
}

static int openLocked(char* what, size_t size);

int scOpen(char* what, size_t size) {
    lock();
    const int rc = openLocked(what, size);
    unlock();
    return rc;
}

static int openLocked(char* what, size_t size) {
    if (what && size) what[0] = 0;
    if (!g_titleId[0]) {
        note(what, size, "scInit first.");
        return -1;
    }
    // Out of the sandbox, to reach /data and GoldHEN's calls. Not undone:
    // the app keeps reaching /data (status, "+" events) until it exits
    // (shell_companion.h, scOpen).
    if (access("/user/data", W_OK) != 0) scSys(500, 2 /* JAILBREAK */, (long)&g_jb, 0);
    mkdir("/user/data/ShellCompanion", 0777);
    mkdir(SC_BGM_DIR, 0777);
    if (access(SC_BGM_DIR, W_OK) != 0) {
        note(what, size, "Cannot write " SC_BGM_DIR " (GoldHEN's jailbreak?).");
        return -1;
    }
    writeEbgm();
    writeUser();
    // One module, the player and the watch both (sc_companion.prx, 0.95):
    // loaded when neither answers.
    const int player = scPlayerRunning(), watch = scWatchRunning();
    if (player && watch) {
        note(what, size, "Shell Companion was there.");
        return 0;
    }
    if (player || watch) {
        // One of them only: an older Shell Companion (two modules) loaded
        // earlier this boot; a new one now would play or hook twice.
        note(what, size, "An older Shell Companion is running (%s only): restart the PS4.",
             player ? "the player" : "the music watch");
        return -1;
    }
    int rc = 0;
    if (loadModule("sc_companion.prx", what, size) != 0) return -1;
    if (!waitFor(scPlayerRunning, 3000)) {
        note(what, size, "The player does not answer.");
        rc = -1;
    }
    if (!waitFor(scWatchRunning, 5000)) {
        note(what, size, "The music watch does not answer (the Music section will not show the queue).");
        rc = -1;
    }
    return rc;
}

// ── The queue ────────────────────────────────────────────────────────────────

static size_t putEncoded(char* out, size_t at, size_t cap, const char* s);

// A field, its tabs, line breaks and | made spaces (on a URL's line the
// player takes | as a separator too).
static size_t putField(char* out, size_t at, size_t cap, const char* s) {
    for (; s && *s && at + 1 < cap; s++)
        out[at++] = (*s == '\t' || *s == '\n' || *s == '\r' || *s == '|') ? ' ' : *s;
    return at;
}

int scWriteQueue(const ScQueue* queue, const ScTrack* tracks, uint32_t count) {
    if (!tracks && count) return -1;
    if (count > 10000) count = 10000;
    size_t cap = 64 + 16384;  // the head, "+"'s templates included
    const ScPlus* plus = queue ? queue->plus : NULL;
    if (plus) {
        const char* f[8] = {plus->saveUrl, plus->deleteUrl, plus->method, plus->body,
                            plus->type, plus->headers, plus->containsUrl, plus->containsMatch};
        for (int k = 0; k < 8; k++) cap += f[k] ? 3 * strlen(f[k]) : 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        const ScTrack* t = &tracks[i];
        // A track with nowhere to play it from: refused, as leaving it out
        // would move #start and every place after it onto other tracks.
        if (!(t->local && t->local[0]) && !(t->url && t->url[0])) return -1;
        const char* f[7] = {t->local, t->url, t->title, t->artist, t->album, t->coverUrl, t->id};
        cap += 16;
        for (int k = 0; k < 7; k++) cap += f[k] ? strlen(f[k]) : 0;
    }
    char* out = (char*)malloc(cap);
    if (!out) return -1;
    size_t at = (size_t)snprintf(out, cap, "#start %u\n#position %u\n", queue ? queue->index : 0,
                                 queue ? queue->positionMs : 0);
    if (plus && plus->mode == SC_PLUS_HTTP) {
        // "+"'s settings, each %-encoded (the player's plus.c).
        const char* key[8] = {"save", "delete", "method", "body", "type", "headers", "contains", "match"};
        const char* val[8] = {plus->saveUrl, plus->deleteUrl, plus->method, plus->body,
                              plus->type, plus->headers, plus->containsUrl, plus->containsMatch};
        at += (size_t)snprintf(out + at, cap - at, "#plus.mode http\n");
        for (int k = 0; k < 8; k++) {
            if (!val[k] || !val[k][0]) continue;
            at += (size_t)snprintf(out + at, cap - at, "#plus.%s ", key[k]);
            at = putEncoded(out, at, cap, val[k]);
            out[at++] = '\n';
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        const ScTrack* t = &tracks[i];
        const char* where = t->local && t->local[0] ? t->local : t->url;
        // The shell's path: /data is /user/data inside it.
        if (!strncmp(where, "/data/", 6)) at = putField(out, at, cap, "/user");
        at = putField(out, at, cap, where);
        const char* rest[5] = {t->title, t->artist, t->album, t->coverUrl, t->id};
        int last = 4;
        while (last >= 0 && !(rest[last] && rest[last][0])) last--;
        for (int k = 0; k <= last; k++) {
            if (at + 1 < cap) out[at++] = '\t';
            at = putField(out, at, cap, rest[k]);
        }
        if (at + 1 < cap) out[at++] = '\n';
    }
    const int rc = writeAll(SC_BGM_QUEUE, out, at);
    free(out);
    return rc;
}

// ── Handing over and taking back ─────────────────────────────────────────────

// 1 if a command file of its own name (below) waits for the player.
static int namedWaiting(void) {
    DIR* d = opendir(SC_BGM_DIR);
    if (!d) return 0;
    int any = 0;
    for (struct dirent* e; !any && (e = readdir(d));) {
        const size_t len = strlen(e->d_name);
        any = !strncmp(e->d_name, "cmd.", 4) && strcmp(e->d_name, "cmd.work") &&
              !(len > 4 && !strcmp(e->d_name + len - 4, ".new"));
    }
    closedir(d);
    return any;
}

// Each command file written whole beside the others, then put in place in
// one step, so the player never reads (and deletes) one half written: cmd
// when there is none and no other waits, else cmd.<ms>-<pid>-<n>, which the
// player reads after cmd, in name order (the ms 13 digits, so that order is
// the order sent).
int scCommand(const char* command) {
    if (!command || !command[0]) return -1;
    static unsigned seq;
    lock();
    const unsigned n0 = seq++;
    unlock();
    const size_t n = strlen(command);
    const int nl = command[n - 1] != '\n';
    char name[160], tmp[176];
    snprintf(name, sizeof(name), "%s.%013llu-%d-%u", SC_BGM_CMD, (unsigned long long)nowMs(), (int)scSys(20 /* getpid */, 0, 0, 0), n0);
    snprintf(tmp, sizeof(tmp), "%s.new", name);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    const int ok = write(fd, command, n) == (ssize_t)n && (!nl || write(fd, "\n", 1) == 1);
    close(fd);
    chmod(tmp, 0666);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    // cmd if free (link does not replace it, unlike rename), else its own name.
    if (!namedWaiting() && scSys(9 /* link */, (long)tmp, (long)SC_BGM_CMD, 0) == 0) {
        unlink(tmp);
        return 0;
    }
    if (rename(tmp, name) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int scMusicPlay(void) {
    return scCommand("reload\nplay\n");
}

int scMusicTakeBack(void) {
    // Sent even when the heartbeat is late: the player may be waiting on a
    // slow or unreachable server, and takes it when it is back. Only a
    // player never there (no status at all) is not asked.
    if (access(SC_BGM_STATUS, F_OK) != 0) return -1;
    const uint64_t sent = nowMs();
    if (scCommand("takeback\n") != 0) return -1;
    // Until the player has said so in a status written since: up to 5 s.
    for (int w = 0; w < 100; w++) {
        ScQueueStatus st;
        if (scReadQueueStatus(&st) == 0 && st.beatMs >= sent && !strcmp(st.state, "stopped")) return 0;
        usleep(50000);
    }
    return -1;
}

int scReadQueueStatus(ScQueueStatus* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    static char text[8192];
    lock();
    if (readAll(SC_BGM_STATUS, text, sizeof(text)) <= 0) {
        unlock();
        return -1;
    }
    char v[64];
    field(text, "state", out->state, sizeof(out->state));
    if (field(text, "index", v, sizeof(v))) out->index = atoi(v);
    if (field(text, "count", v, sizeof(v))) out->count = atoi(v);
    out->track = field(text, "track", v, sizeof(v)) ? atoi(v) : out->index;
    if (field(text, "pos_ms", v, sizeof(v))) out->positionMs = (uint32_t)strtoul(v, NULL, 10);
    if (field(text, "dur_ms", v, sizeof(v))) out->durationMs = (uint32_t)strtoul(v, NULL, 10);
    field(text, "file", out->file, sizeof(out->file));
    field(text, "title", out->title, sizeof(out->title));
    field(text, "artist", out->artist, sizeof(out->artist));
    field(text, "album", out->album, sizeof(out->album));
    if (field(text, "volume", v, sizeof(v))) out->volume = atoi(v);
    if (field(text, "repeat", v, sizeof(v))) out->repeat = atoi(v);
    if (field(text, "shuffle", v, sizeof(v))) out->shuffle = atoi(v);
    if (field(text, "beat_ms", v, sizeof(v))) out->beatMs = strtoull(v, NULL, 10);
    unlock();
    return 0;
}

// A field %-encoded where it holds a tab, a line break or a %.
static size_t putEncoded(char* out, size_t at, size_t cap, const char* s) {
    static const char hex[] = "0123456789ABCDEF";
    for (; s && *s && at + 4 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '%' || c == '\t' || c == '\r' || c == '\n') {
            out[at++] = '%';
            out[at++] = hex[c >> 4];
            out[at++] = hex[c & 15];
        } else {
            out[at++] = (char)c;
        }
    }
    return at;
}

// ── "+" pressed while the app was away ───────────────────────────────────────

#define PLUS_EVENTS SC_BGM_DIR "/plus-events.txt"

static int readPlusEvents(ScPlusEvent* events, uint32_t cap, uint32_t* count);

int scReadPlusEvents(ScPlusEvent* events, uint32_t cap, uint32_t* count) {
    lock();
    const int rc = readPlusEvents(events, cap, count);
    unlock();
    return rc;
}

static int readPlusEvents(ScPlusEvent* events, uint32_t cap, uint32_t* count) {
    if (count) *count = 0;
    // Taken first, so that a press meanwhile goes to a new file. What was not
    // read last time (past cap) stays in the .work file, older than any press
    // since: read before the file is taken again.
    char work[128];
    snprintf(work, sizeof(work), "%s.work", PLUS_EVENTS);
    if (access(work, F_OK) != 0 && rename(PLUS_EVENTS, work) != 0)
        return access(PLUS_EVENTS, F_OK) == 0 ? -1 : 0;
    static char text[256 * 1024];
    const int len = readAll(work, text, sizeof(text));
    if (len < 0) return -1;
    uint32_t n = 0;
    char* p = text;
    while (*p && (!events || n < cap)) {
        char* e = p;
        while (*e && *e != '\n') e++;
        if (!*e && len == (int)sizeof(text) - 1) break;  // cut by the buffer: next time
        if (*e) *e++ = 0;
        char* f[4] = {p, NULL, NULL, NULL};
        int k = 1;
        for (char* q = p; *q && k < 4; q++)
            if (*q == '\t') {
                *q = 0;
                f[k++] = q + 1;
            }
        if (k == 4 && events) {
            ScPlusEvent* ev = &events[n++];
            ev->ms = strtoull(f[0], NULL, 10);
            ev->saved = !strcmp(f[1], "save");
            snprintf(ev->id, sizeof(ev->id), "%s", f[2]);
            snprintf(ev->file, sizeof(ev->file), "%s", f[3]);
        }
        p = e;
    }
    // The rest of the file (past cap, or past the buffer) kept for next time,
    // written whole beside it and moved in.
    const size_t done = (size_t)(p - text);
    int rc = 0;
    const int in = open(work, O_RDONLY);
    const off_t size = in >= 0 ? lseek(in, 0, SEEK_END) : 0;
    if (size <= (off_t)done) {
        if (in >= 0) close(in);
        unlink(work);
    } else {
        const size_t want = (size_t)size - done;
        char* rest = (char*)malloc(want);
        size_t got = 0;
        if (rest && lseek(in, (off_t)done, SEEK_SET) == (off_t)done) {
            ssize_t k;
            while (got < want && (k = read(in, rest + got, want - got)) > 0) got += (size_t)k;
        }
        close(in);
        if (got != want || writeAll(work, rest, got) != 0) rc = -1;
        free(rest);
    }
    if (count) *count = n;
    return n ? 0 : rc;
}

// ── Self-update ──────────────────────────────────────────────────────────────

#define UPDATE_FILE SC_BGM_DIR "/update.txt"
#define UPDATE_RESULT SC_BGM_DIR "/update-result.txt"

int scUpdateInstallPkg(const char* pkg, int relaunch) {
    if (!g_titleId[0] || !pkg || !pkg[0] || !scPlayerRunning()) return -1;
    unlink(UPDATE_RESULT);
    char text[1200];
    const int n = snprintf(text, sizeof(text), "title=%s\nsource=%s\nrelaunch=%d\n", g_titleId, pkg, relaunch ? 1 : 0);
    if (n <= 0 || (size_t)n >= sizeof(text) || writeAll(UPDATE_FILE, text, (size_t)n) != 0) return -1;
    return scCommand("update_start\n");
}

int scReadUpdateResult(int* result, char* why, size_t whySize) {
    char text[1024];
    if (readAll(UPDATE_RESULT, text, sizeof(text)) <= 0) return -1;
    char v[24];
    if (result) *result = field(text, "result", v, sizeof(v)) ? atoi(v) : -1;
    if (why && whySize) field(text, "why", why, whySize);
    return 0;
}
