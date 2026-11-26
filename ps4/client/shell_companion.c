/**
 * Shell Companion's PS4 client (shell_companion.h). Apache-2.0.
 *
 * The files it shares with the modules (ps4/bgm/bgm.h): the app, jailbroken,
 * sees /data/ShellCompanion/ps4 as /user/data/ShellCompanion/ps4, the path
 * the shell has, which is used here throughout.
 */

#include "shell_companion.h"

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
    const uint64_t beat = strtoull(v, NULL, 10), now = nowMs();
    // A beat from before this boot reads later than now, or long ago.
    return beat <= now + 1000 && now - beat < 3000;
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

int scOpen(char* what, size_t size) {
    if (what && size) what[0] = 0;
    if (!g_titleId[0]) {
        note(what, size, "scInit first.");
        return -1;
    }
    // Out of the sandbox, to reach /data and GoldHEN's calls.
    if (access("/user/data", W_OK) != 0) scSys(500, 2 /* JAILBREAK */, (long)&g_jb, 0);
    mkdir("/user/data/ShellCompanion", 0777);
    mkdir(SC_BGM_DIR, 0777);
    if (access(SC_BGM_DIR, W_OK) != 0) {
        note(what, size, "Cannot write " SC_BGM_DIR " (GoldHEN's jailbreak?).");
        return -1;
    }
    writeEbgm();
    int rc = 0;
    if (scPlayerRunning()) {
        note(what, size, "The player was there.");
    } else if (loadModule("sc_bgm.prx", what, size) != 0 || !waitFor(scPlayerRunning, 3000)) {
        note(what, size, "The player does not answer.");
        rc = -1;
    }
    if (scWatchRunning()) {
        note(what, size, "The music watch was there.");
    } else if (loadModule("sc_mcw.prx", what, size) != 0 || !waitFor(scWatchRunning, 5000)) {
        note(what, size, "The music watch does not answer (the Music section will not show the queue).");
        rc = -1;
    }
    return rc;
}

// ── The queue ────────────────────────────────────────────────────────────────

// A field, its tabs and line breaks made spaces.
static size_t putField(char* out, size_t at, size_t cap, const char* s) {
    for (; s && *s && at + 1 < cap; s++) out[at++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
    return at;
}

int scWriteQueue(const ScQueue* queue, const ScTrack* tracks, uint32_t count) {
    if (!tracks && count) return -1;
    if (count > 10000) count = 10000;
    size_t cap = 64;
    for (uint32_t i = 0; i < count; i++) {
        const ScTrack* t = &tracks[i];
        const char* f[6] = {t->local, t->url, t->title, t->artist, t->album, t->coverUrl};
        cap += 16;
        for (int k = 0; k < 6; k++) cap += f[k] ? strlen(f[k]) : 0;
    }
    char* out = (char*)malloc(cap);
    if (!out) return -1;
    size_t at = (size_t)snprintf(out, cap, "#start %u\n#position %u\n", queue ? queue->index : 0,
                                 queue ? queue->positionMs : 0);
    for (uint32_t i = 0; i < count; i++) {
        const ScTrack* t = &tracks[i];
        const char* where = t->local && t->local[0] ? t->local : t->url;
        if (!where || !where[0]) continue;
        // The shell's path: /data is /user/data inside it.
        if (!strncmp(where, "/data/", 6)) at = putField(out, at, cap, "/user");
        at = putField(out, at, cap, where);
        const char* rest[4] = {t->title, t->artist, t->album, t->coverUrl};
        int last = 3;
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

int scCommand(const char* command) {
    if (!command || !command[0]) return -1;
    // Added to what is there; the player renames cmd before it reads it.
    const int fd = open(SC_BGM_CMD, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return -1;
    const size_t n = strlen(command);
    const int ok = write(fd, command, n) == (ssize_t)n && (command[n - 1] == '\n' || write(fd, "\n", 1) == 1);
    close(fd);
    return ok ? 0 : -1;
}

int scMusicPlay(void) {
    return scCommand("reload\nplay\n");
}

int scMusicTakeBack(void) {
    if (!scPlayerRunning()) return -1;
    if (scCommand("takeback\n") != 0) return -1;
    // Until the player has said so, briefly.
    for (int w = 0; w < 20; w++) {
        ScQueueStatus st;
        if (scReadQueueStatus(&st) == 0 && !strcmp(st.state, "stopped")) return 0;
        usleep(50000);
    }
    return 0;
}

int scReadQueueStatus(ScQueueStatus* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    static char text[8192];
    if (readAll(SC_BGM_STATUS, text, sizeof(text)) <= 0) return -1;
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
    return 0;
}

// ── Background downloads ─────────────────────────────────────────────────────

#define DL_LIST SC_BGM_DIR "/downloads.txt"
#define DL_STATUS SC_BGM_DIR "/downloads-status.txt"

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

int scWriteDownloads(int notify, const char* headers, const ScDownload* items, uint32_t count) {
    if (!items && count) return -1;
    if (count > 200) count = 200;
    const size_t cap = 4096 + (size_t)count * 4096;
    char* out = (char*)malloc(cap);
    if (!out) return -1;
    size_t at = (size_t)snprintf(out, cap, "notify=%d\nheaders=", notify ? 1 : 0);
    at = putEncoded(out, at, cap, headers);
    out[at++] = '\n';
    for (uint32_t i = 0; i < count; i++) {
        const ScDownload* d = &items[i];
        if (!d->file || !d->file[0] || !d->kind) continue;
        at += (size_t)snprintf(out + at, cap - at, "item=");
        at = putEncoded(out, at, cap, d->id);
        out[at++] = '\t';
        at = putEncoded(out, at, cap, d->file);
        at += (size_t)snprintf(out + at, cap - at, "\t%llu\t", (unsigned long long)d->size);
        at = putEncoded(out, at, cap, d->title);
        out[at++] = '\t';
        at = putEncoded(out, at, cap, d->kind);
        for (int a = 0; a < 3; a++) {
            out[at++] = '\t';
            at = putEncoded(out, at, cap, d->args[a]);
        }
        out[at++] = '\n';
    }
    at += (size_t)snprintf(out + at, cap - at, "end\n");
    const int rc = writeAll(DL_LIST, out, at);
    free(out);
    return rc;
}

int scDownloadsStart(void) {
    return scCommand("dl_start\n");
}

int scDownloadsTakeBack(void) {
    if (!scPlayerRunning()) return 0;  // nothing in the shell to stop
    if (scCommand("dl_takeback\n") != 0) return -1;
    for (int w = 0; w < 50; w++) {
        int running = 1;
        uint32_t n = 0;
        if (scReadDownloadsStatus(&running, NULL, 0, &n) == 0 && !running) return 0;
        usleep(100000);
    }
    return -1;
}

int scReadDownloadsStatus(int* running, ScDownloadStatus* items, uint32_t cap, uint32_t* count) {
    static char text[200 * 160 + 128];
    if (count) *count = 0;
    if (readAll(DL_STATUS, text, sizeof(text)) <= 0) return -1;
    char v[16];
    if (running) *running = field(text, "running", v, sizeof(v)) ? atoi(v) : 0;
    uint32_t n = 0;
    for (char* p = text; *p;) {
        char* e = p;
        while (*e && *e != '\n') e++;
        const char keep = *e;
        *e = 0;
        if (!strncmp(p, "item=", 5) && items && n < cap) {
            ScDownloadStatus* s = &items[n];
            memset(s, 0, sizeof(*s));
            char* f[7] = {0};
            int k = 0;
            char* q = p + 5;
            f[k++] = q;
            for (; *q && k < 7; q++)
                if (*q == '\t') {
                    *q = 0;
                    f[k++] = q + 1;
                }
            snprintf(s->id, sizeof(s->id), "%s", f[0]);
            if (k > 1) snprintf(s->state, sizeof(s->state), "%s", f[1]);
            if (k > 2) s->bytes = strtoull(f[2], NULL, 10);
            if (k > 3) s->size = strtoull(f[3], NULL, 10);
            if (k > 6) {
                s->pagesDone = (uint32_t)strtoul(f[6], NULL, 10);
                const char* slash = strchr(f[6], '/');
                if (slash) s->pageCount = (uint32_t)strtoul(slash + 1, NULL, 10);
            }
            n++;
        }
        *e = keep;
        p = *e ? e + 1 : e;
    }
    if (count) *count = n;
    return 0;
}
