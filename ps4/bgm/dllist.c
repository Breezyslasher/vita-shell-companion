/**
 * A download list's files in Notifications > Downloads (0.96, an
 * experiment), inside the player module (bgm.c includes this after pkg.c).
 *
 * The PS4's download service takes packages only: a media file registered
 * with it is listed, then fails as soon as it is started (0.92-0.94:
 * 0x80990033, CE-32942-0, without a single HTTP request). So each url or
 * pages item of a list is registered as a placeholder and never started:
 * the list shows it under its title, and the service leaves it alone. Our
 * own downloader fetches it (downloads.c), and the service's progress call,
 * hooked inside the shell, answers for a placeholder with that item's
 * bytes. The placeholders are removed when the list ends (10 s after, the
 * last ones shown whole), when the app takes the list back, and at the next
 * list for any left from a crash or an earlier boot (dl-list-tasks.txt).
 *
 * The hooks are on the native functions in libSceBgft, as the music
 * service's are (mcw.c), not the shell's managed wrappers (0.85 hooked
 * those, and the shell went down with a download going). They log the
 * first answers for every task, the PS4's own downloads too, to show what
 * a real download's progress looks like.
 *
 * Switches, in the player's folder: dl-nolist (no placeholders),
 * dl-list-bits.txt (going=0x.., waiting=0x.., done=0x..: the progress's
 * state bits answered for each, while they are worked out).
 */

#include "../mcw/hook.h"

int mcwHookNative(Hook* h, void* fn, const char** why);  // mcw.c, under its install lock
int scePthreadGetthreadid(void);

#define DLL_MAX 64
#define DLL_TASKS SC_BGM_DIR "/dl-list-tasks.txt"
#define DLL_LOGGED 48  // other tasks whose answers are logged

typedef int (*FnBgftUnregister)(int32_t);

// Trying "finished" answers side by side (1.07): with dl-list-try in the
// player's folder, item i is answered, once done, with candidate i % 6, its
// letter added to its title, so one look at the list shows what each reads
// as. 1.05's Complete/Complete read "waiting"; 1.06's Stop/Download with
// every byte, as a finished game answers, read nothing (a game's is
// "Installed", from the game being there).
static const struct {
    char letter;
    uint32_t bits, param;  // progress bits (low 10), query param
} g_dllTry[6] = {
    {'A', 0x310, 0x100c1},   // Stop, Complete
    {'B', 0x313, 0x1103c1},  // Complete, Complete, and bit 20, as installed patches (0x110381)
    {'C', 0x313, 0x103c3},   // Complete, Complete, type 3 as finished games' (0x...63)
    {'D', 0x308, 0x80c1},    // Stop, Download, type 1
    {'E', 0x30c, 0xc2c1},    // Run, Install
    {'F', 0x311, 0x101c1},   // Ready, Complete
};

// And "waiting" answers (1.10): 1.08's photo had a waiting song (Ready/
// Standby, 0x301/0x1c1) reading "Cannot download", and B's done answer
// reading "Queued for downloading". Item i, while it waits, gets i % 6.
static const struct {
    uint32_t bits, param;
} g_dllTryWait[6] = {
    {0x301, 0x1c1},     // A: Ready, Standby (1.03-1.09's)
    {0x313, 0x1103c1},  // B: as 1.08's B, which read "Queued for downloading"
    {0x300, 0x0c1},     // C: Stop, Standby
    {0x305, 0x41c1},    // D: Ready, Prepare
    {0x321, 0x201c1},   // E: Ready, Standby, paused
    {0x309, 0x81c1},    // F: Ready, Download
};

static struct {
    int tried, hooked;
    Hook hooks[2];
    Hook query[4];  // the list's task queries (0.98)
    Hook info[8];   // its task info calls, logged only (1.11)
    uint32_t infoLogged;
    uint32_t queryCalls[4];
    FnBgftUnregister unregister, stop, pause;
    uint32_t bitsGoing, bitsWaiting, bitsDone;
    uint32_t paramGoing, paramWaiting, paramDone;  // the task query's param answered for ours (1.01)
    int haveGoing, haveWaiting, haveDone;
    int trying;  // dl-list-try (1.07)
    volatile int count;
    volatile int pctOk;  // a float percent at 0x40 seen in real tasks' answers (1.04)
    int32_t pctSeen;     // the first real task it was seen in
    struct {
        volatile int32_t task;
        int item;
        uint64_t t0, b0;  // when it began to come, and its bytes then
        uint32_t logged;
        uint32_t lastWas, lastSaid;  // the service's bits and ours, last logged (0.99)
        uint64_t lastParam;          // its param in the last query logged (0.99)
        volatile int failed;         // the service failed it: to be replaced (1.02)
        uint32_t tries;              // replaced so far
    } e[DLL_MAX];
    struct {
        int32_t task;
        uint32_t logged, bits;
        uint64_t transferred, lastUs;
    } other[DLL_LOGGED];
    volatile int others;
    uint32_t calls;
} g_dll;

// The answer for one of ours, filled into what the service gave (its own
// state for a task never started).
static void dllFill(int k, BgftProgress* pr) {
    const int i = g_dll.e[k].item;
    if (i < 0 || i >= g_dls.count) return;
    __typeof__(g_dls.item[0])* it = &g_dls.item[i];
    const int st = it->state;
    const uint64_t bytes = it->bytes;
    uint64_t size = it->size;
    if (st == DLS_DONE) size = bytes ? bytes : size;
    if (!size) size = bytes + 1;  // not known yet: some way to go
    if (st == DLS_GOING && !g_dll.e[k].t0) {
        g_dll.e[k].t0 = sysNowUs();
        g_dll.e[k].b0 = bytes;
    }
    uint32_t rest = 0xFFFFFFFFu;
    if (st == DLS_GOING && g_dll.e[k].t0 && it->size) {
        const uint64_t us = sysNowUs() - g_dll.e[k].t0;
        const uint64_t came = bytes > g_dll.e[k].b0 ? bytes - g_dll.e[k].b0 : 0;
        if (us > 2000000u && came) {
            const uint64_t left = it->size > bytes ? it->size - bytes : 0;
            rest = (uint32_t)(left * (us / 1000u) / came / 1000u);
        }
    }
    pr->errorResult = 0;
    // The low 10 bits only (1.03): the rest is not the state, and the
    // service's own (varying) bits stay.
    const uint32_t keep = pr->bits & ~0x3FFu;
    if (st == DLS_GOING && g_dll.haveGoing) pr->bits = keep | (g_dll.bitsGoing & 0x3FFu);
    else if (st == DLS_DONE && g_dll.trying) pr->bits = keep | g_dllTry[i % 6].bits;
    else if (st == DLS_DONE && g_dll.haveDone) pr->bits = keep | (g_dll.bitsDone & 0x3FFu);
    else if (st != DLS_GOING && st != DLS_DONE && g_dll.trying) pr->bits = keep | g_dllTryWait[i % 6].bits;
    else if (st != DLS_GOING && st != DLS_DONE && g_dll.haveWaiting) pr->bits = keep | (g_dll.bitsWaiting & 0x3FFu);
    pr->length = pr->lengthTotal = size;
    pr->transferred = pr->transferredTotal = st == DLS_DONE ? size : bytes;
    pr->numIndex = 1;
    pr->numTotal = 1;
    // "Preparing to download... (n%)" reads preparingPercent (1.05: 1.04's
    // answers were right and the list still read 0%; and the answer ends at
    // 0x40, with no percentTotal after it). Ours: how much has come.
    // 1.10: a real download going answers 100 here (1.05's log, package
    // 1098: 0x38 "64 64"), and the list then shows it downloading, its bar
    // from the bytes; 0 reads "Preparing to download... (0%)", which 1.05
    // filled with the bytes' percent. 100 going and done, 0 waiting.
    pr->preparingPercent = st == DLS_GOING ? 100 : 0;  // finished: 0, as package 1099
    pr->restSec = pr->restSecTotal = st == DLS_DONE ? 0 : rest;
}

// After the service's progress call (taskId, progress), on the caller's
// thread, the shell's list among them (mcw.c's thunk, kind 18).
void dlListHook(Frame* f) {
    const int32_t task = (int32_t)f->gpr[0];
    BgftProgress* pr = (BgftProgress*)(uintptr_t)f->gpr[1];
    const uint32_t n = __atomic_add_fetch(&g_dll.calls, 1, __ATOMIC_RELAXED);
    if (!pr || hookReadable(pr, 0x38) != 0x38 || !(hookProt(pr) & 2)) return;
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        if (g_dll.e[k].task != task || task <= 0) continue;
        const uint32_t was = pr->bits;
        const int32_t err = pr->errorResult;
        if (err && !g_dll.e[k].failed) g_dll.e[k].failed = 1;  // replaced by dllRepair (1.02)
        const int rc = (int)(int32_t)f->retRax;
        dllFill(k, pr);
        // TaskProgress's last field, percentTotal (a float after
        // localCopyPercent, 0x40), once real tasks have shown it is there.
        if (g_dll.pctOk && hookReadable(pr, 0x44) == 0x44) {
            float pct = pr->lengthTotal ? (float)((double)pr->transferredTotal * 100.0 / (double)pr->lengthTotal) : 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            memcpy((uint8_t*)pr + 0x40, &pct, 4);
        }
        f->retRax = 0;
        // Whenever the service's bits or ours change (0.99), and every 200th.
        const uint32_t seen = g_dll.e[k].logged++;
        const int changed = !seen || was != g_dll.e[k].lastWas || pr->bits != g_dll.e[k].lastSaid;
        g_dll.e[k].lastWas = was;
        g_dll.e[k].lastSaid = pr->bits;
        if (changed || seen % 200 == 0)
            logLine("dllist: %s(%d) [ours, item %d] was 0x%08X bits 0x%x error 0x%08X; answered bits 0x%x, %lu of %lu, "
                    "%u s left  [call %u, thread %d]",
                    f->hook->name, task, g_dll.e[k].item, (unsigned)rc, was, (unsigned)err, pr->bits,
                    (unsigned long)pr->transferred, (unsigned long)pr->length, pr->restSec, n,
                    scePthreadGetthreadid());
        return;
    }
    // Another task. A finished one whose answer has 100.0 (a float) at
    // 0x40 says TaskProgress's percentTotal is there (1.04: 1.03's bar
    // stayed at 0%, everything before it answered): seen in two tasks,
    // ours get it too. Nothing written past 0x40 before that.
    if (!g_dll.pctOk && f->retRax == 0 && pr->lengthTotal && pr->transferredTotal == pr->lengthTotal &&
        hookReadable(pr, 0x44) == 0x44) {
        uint32_t v;
        memcpy(&v, (const uint8_t*)pr + 0x40, 4);
        if (v == 0x42C80000u) {
            if (!g_dll.pctSeen) {
                g_dll.pctSeen = task;
            } else if (g_dll.pctSeen != task) {
                g_dll.pctOk = 1;
                logLine("dllist: percentTotal at 0x40 (100.0 in tasks %d and %d): answered for ours", g_dll.pctSeen, task);
            }
        }
    }
    int o = 0;
    while (o < g_dll.others && g_dll.other[o].task != task) o++;
    if (o == g_dll.others) {
        if (o >= DLL_LOGGED) return;
        g_dll.other[o].task = task;
        g_dll.other[o].logged = 0;
        g_dll.others = o + 1;
    }
    // When its state or bytes change (0.97: a real download's answers as it
    // goes), at most one every 3 s, 30 in all.
    const uint64_t now = sysNowUs();
    __typeof__(g_dll.other[0])* ot = &g_dll.other[o];
    if (ot->logged && ((pr->bits == ot->bits && pr->transferred == ot->transferred) || now - ot->lastUs < 3000000u))
        return;
    if (ot->logged >= 30) return;
    ot->logged++;
    ot->bits = pr->bits;
    ot->transferred = pr->transferred;
    ot->lastUs = now;
    uint32_t tail[4] = {0, 0, 0, 0};  // 0x38-0x47: preparing, local copy, then what follows
    if (hookReadable(pr, 0x48) == 0x48) memcpy(tail, (const uint8_t*)pr + 0x38, 16);
    logLine("dllist: %s(%d) = 0x%08X: bits 0x%x, error 0x%08X, %lu of %lu (total %lu of %lu), %u of %u, %u s left "
            "(total %u), 0x38: %x %x %x %x  [call %u, thread %d]",
            f->hook->name, task, (unsigned)f->retRax, pr->bits, (unsigned)pr->errorResult,
            (unsigned long)pr->transferred, (unsigned long)pr->length, (unsigned long)pr->transferredTotal,
            (unsigned long)pr->lengthTotal, pr->numIndex, pr->numTotal, pr->restSec, pr->restSecTotal, tail[0], tail[1],
            tail[2], tail[3], n, scePthreadGetthreadid());
}

// After one of the service's task queries (0.98), logged only: its results
// are {int taskId; uint param} pairs, each section's logged when they
// change. (0.98-0.99 cleared a "failed" flag, 0x200, on ours wherever a
// pair was found in memory an argument pointed to; 0.99's log showed the
// flag isn't set in these results, and the match it found was not them.)
void dlListQueryHook(Frame* f) {
    const Hook* h = f->hook;
    int q = 0;
    while (q < 4 && &g_dll.query[q] != h) q++;
    const uint32_t n = q < 4 ? __atomic_add_fetch(&g_dll.queryCalls[q], 1, __ATOMIC_RELAXED) : 0;
    if (n <= 4)
        logLine("dllist: %s(%lx, %lx, %lx, %lx, %lx) = 0x%08X  [call %u, thread %d]", h->name,
                (unsigned long)f->gpr[0], (unsigned long)f->gpr[1], (unsigned long)f->gpr[2],
                (unsigned long)f->gpr[3], (unsigned long)f->gpr[4], (unsigned)f->retRax, n,
                scePthreadGetthreadid());
    const int count = g_dll.count;
    // Its results (0.99): (?, max, section, &count, results) as 0.98's log
    // reads, {int task; uint param} pairs. Logged when they hold one of ours
    // whose param changed, and the first 3 that hold any.
    {
        static uint32_t shownAny;
        static uint64_t lastHash[16];
        const uint32_t* num = (const uint32_t*)(uintptr_t)f->gpr[3];
        const uint8_t* res = (const uint8_t*)(uintptr_t)f->gpr[4];
        uint32_t cnt = 0;
        if (hookReadable(num, 4) == 4) cnt = *num;
        // Ours given a real package task's param (1.01: 1.00's log, the
        // PS4's own installs 0x10008261 while going, 0x10008063 done; ours
        // 0x200c1, 0x280c1, 0x40280c1 failed), in the results themselves:
        // count pairs at the fifth argument, as every section logged shows.
        // Section 5 holds the failed tasks (every failed placeholder was
        // there, and only those): ours are taken out of it (1.08), so the
        // Notifications summary never counts one the service failed as an
        // error ("Cannot download" until it was opened again); dllRepair
        // replaces it within seconds. Only in the section query, whose
        // count is the fourth argument.
        if (h == &g_dll.query[1] && f->gpr[2] == 5 && cnt && cnt <= 960 && count &&
            hookReadable(res, (size_t)cnt * 8) == (size_t)cnt * 8 && (hookProt(res) & 2) && (hookProt(num) & 2)) {
            uint32_t w = 0;
            for (uint32_t j = 0; j < cnt; j++) {
                int32_t id;
                memcpy(&id, res + j * 8, 4);
                int mine = 0;
                for (int k = 0; k < count && k < DLL_MAX; k++)
                    if (g_dll.e[k].task == id && id > 0) {
                        mine = 1;
                        g_dll.e[k].failed = 1;
                    }
                if (!mine) {
                    if (w != j) memmove((uint8_t*)res + w * 8, res + j * 8, 8);
                    w++;
                }
            }
            if (w != cnt) {
                logLine("dllist: section 5: %u of ours taken out of the failed ones", cnt - w);
                *(uint32_t*)(uintptr_t)f->gpr[3] = w;
                cnt = w;
            }
        }
        // A finished one moves from section 3 (going) to the top of section
        // 9 (finished), as the PS4's own do (1.13: packages 1078 and 1099
        // left section 3 and came first in section 9 as 0x10008063, Stop/
        // Download with every byte). The list builds its rows per section:
        // ours, finished but still in section 3, read oddly whatever they
        // answered.
        if (h == &g_dll.query[1] && (f->gpr[2] == 3 || f->gpr[2] == 9) && count && cnt <= 960 &&
            (hookProt(num) & 2)) {
            const uint32_t max = (uint32_t)f->gpr[1] < 960 ? (uint32_t)f->gpr[1] : 960;
            if (f->gpr[2] == 3 && cnt && hookReadable(res, (size_t)cnt * 8) == (size_t)cnt * 8 && (hookProt(res) & 2)) {
                uint32_t w = 0;
                for (uint32_t j = 0; j < cnt; j++) {
                    int32_t id;
                    memcpy(&id, res + j * 8, 4);
                    int out = 0;
                    for (int k = 0; k < count && k < DLL_MAX; k++) {
                        const int i = g_dll.e[k].item;
                        if (g_dll.e[k].task == id && id > 0 && i >= 0 && i < g_dls.count && g_dls.item[i].state == DLS_DONE)
                            out = 1;
                    }
                    if (!out) {
                        if (w != j) memmove((uint8_t*)res + w * 8, res + j * 8, 8);
                        w++;
                    }
                }
                if (w != cnt) {
                    *(uint32_t*)(uintptr_t)f->gpr[3] = w;
                    cnt = w;
                }
            } else if (f->gpr[2] == 9) {
                // Ours finished, newest first, before the PS4's own.
                int32_t add[DLL_MAX];
                uint32_t n = 0;
                for (int k = count - 1; k >= 0; k--) {
                    const int i = g_dll.e[k].item;
                    if (k >= DLL_MAX || g_dll.e[k].task <= 0 || i < 0 || i >= g_dls.count || g_dls.item[i].state != DLS_DONE)
                        continue;
                    int there = 0;
                    for (uint32_t j = 0; j < cnt && hookReadable(res + j * 8, 8) == 8; j++) {
                        int32_t id;
                        memcpy(&id, res + j * 8, 4);
                        if (id == g_dll.e[k].task) there = 1;
                    }
                    if (!there) add[n++] = g_dll.e[k].task;
                }
                if (n && cnt + n <= max && hookReadable(res, (size_t)(cnt + n) * 8) == (size_t)(cnt + n) * 8 &&
                    (hookProt(res) & 2) && (hookProt(res + (cnt + n) * 8 - 1) & 2)) {
                    if (cnt) memmove((uint8_t*)res + n * 8, res, (size_t)cnt * 8);
                    for (uint32_t j = 0; j < n; j++) {
                        memcpy((uint8_t*)res + j * 8, &add[j], 4);
                        memset((uint8_t*)res + j * 8 + 4, 0, 4);  // its param, written below
                    }
                    cnt += n;
                    *(uint32_t*)(uintptr_t)f->gpr[3] = cnt;
                }
            }
        }
        if (cnt && cnt <= 960 && count && hookReadable(res, (size_t)cnt * 8) == (size_t)cnt * 8 &&
            (hookProt(res) & 2)) {
            for (uint32_t j = 0; j < cnt; j++) {
                int32_t id;
                memcpy(&id, res + j * 8, 4);
                for (int k = 0; k < count && k < DLL_MAX; k++) {
                    if (g_dll.e[k].task != id || id <= 0) continue;
                    const int i = g_dll.e[k].item;
                    const int st = i >= 0 && i < g_dls.count ? g_dls.item[i].state : DLS_WAITING;
                    const uint32_t prm = st == DLS_DONE && g_dll.trying ? g_dllTry[i % 6].param
                                       : st != DLS_DONE && st != DLS_GOING && g_dll.trying ? g_dllTryWait[i % 6].param
                                       : st == DLS_DONE    ? g_dll.paramDone
                                       : st == DLS_GOING ? g_dll.paramGoing
                                                         : g_dll.paramWaiting;
                    memcpy((uint8_t*)res + j * 8 + 4, &prm, 4);
                }
            }
        }
        if (cnt && cnt <= 960 && hookReadable(res, 8) == 8) {
            int ours = 0;
            for (uint32_t j = 0; j < cnt && j < 960 && hookReadable(res + j * 8, 8) == 8; j++) {
                int32_t id;
                uint32_t prm;
                memcpy(&id, res + j * 8, 4);
                memcpy(&prm, res + j * 8 + 4, 4);
                for (int k = 0; k < count && k < DLL_MAX; k++)
                    if (g_dll.e[k].task == id && g_dll.e[k].lastParam != (uint64_t)prm + 1) {
                        g_dll.e[k].lastParam = (uint64_t)prm + 1;
                        ours = 1;
                    }
            }
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t j = 0; j < cnt && j < 960 && hookReadable(res + j * 8, 8) == 8; j++)
                for (int b = 0; b < 8; b++) hash = (hash ^ res[j * 8 + b]) * 1099511628211ull;
            const uint32_t sec = (uint32_t)f->gpr[2] & 15;
            const int differs = hash != lastHash[sec];
            lastHash[sec] = hash;
            if ((ours || differs) && shownAny < 200) {
                shownAny++;
                char line[400];
                int at = fmt(line, sizeof(line), "dllist: %s section %lu: %u task(s):", h->name,
                             (unsigned long)f->gpr[2], cnt);
                for (uint32_t j = 0; j < cnt && j < 14 && hookReadable(res + j * 8, 8) == 8; j++) {
                    int32_t id;
                    uint32_t prm;
                    memcpy(&id, res + j * 8, 4);
                    memcpy(&prm, res + j * 8 + 4, 4);
                    at += fmt(line + at, sizeof(line) - at, " %d:%x", id, prm);
                }
                logLine("%s", line);
            }
        }
    }
    (void)count;  // read only (1.00): 0.98 cleared a flag in memory an argument pointed to, which was not the results
}

// After one of the service's task info calls (1.11), logged only: what
// else the list asks of a task (its info, a value by id: TaskInfoValueId
// 18 Paused, 20 Title, ...), and what it was told, for ours above all.
void dlListInfoHook(Frame* f) {
    const int32_t task = (int32_t)f->gpr[0];
    int ours = 0;
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++)
        if (g_dll.e[k].task == task && task > 0) ours = 1;
    const uint32_t n = __atomic_add_fetch(&g_dll.infoLogged, 1, __ATOMIC_RELAXED);
    if (!(ours ? n <= 400 : n <= 30)) return;
    uint64_t out = 0;
    const uint64_t* p = (const uint64_t*)(uintptr_t)f->gpr[2];
    int have = (uintptr_t)p > 0x10000 && hookReadable(p, 8) == 8;
    if (have) out = *p;
    logLine("dllist: %s(%d, %lx, %lx)%s = 0x%08X, a2 -> %s%lx  [thread %d]", f->hook->name, task,
            (unsigned long)f->gpr[1], (unsigned long)f->gpr[2], ours ? " [ours]" : "", (unsigned)f->retRax,
            have ? "" : "(not read) ", (unsigned long)out, scePthreadGetthreadid());
}

static int dllHex(const char* text, const char* key, uint32_t* out) {
    char v[24];
    statusFieldBgm(text, key, v, sizeof(v));
    if (!v[0]) return 0;
    const char* p = v;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint32_t x = 0;
    for (; *p; p++) {
        const char c = *p;
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        x = x << 4 | (uint32_t)d;
    }
    *out = x;
    return 1;
}

static void dllUnregister(int32_t task) {
    if (task <= 0) return;
    const int rc = g_dll.unregister ? g_dll.unregister(task) : g_dll.stop ? g_dll.stop(task) : -1;
    logLine("dllist: task %d removed (%s): 0x%08X", task, g_dll.unregister ? "unregister" : g_dll.stop ? "stop" : "nothing to",
            (unsigned)rc);
}

// Once a boot: the hooks, and what is left from before. 1 if it can go on.
static int dllSetup(void) {
    if (g_dll.tried) return g_dll.hooked;
    g_dll.tried = 1;
    if (!bgftSetup()) return 0;
    const int lib = libLoad("libSceBgft.sprx");
    g_dll.unregister = (FnBgftUnregister)anySym(lib, "sceBgftServiceIntDownloadUnregisterTask");
    g_dll.stop = (FnBgftUnregister)anySym(lib, "sceBgftServiceDownloadStopTask");
    g_dll.pause = (FnBgftUnregister)anySym(lib, "sceBgftServiceDownloadPauseTask");
    // The list's task queries (0.98): the names the shell's wrappers
    // (BgftWrapper.DownloadQueryTask, ...Section) would call, as
    // DownloadGetProgress calls sceBgftServiceIntDownloadGetProgress.
    static const char* const qnames[4] = {"sceBgftServiceIntDownloadQueryTask", "sceBgftServiceIntDownloadQueryTaskSection",
                                          "sceBgftServiceDownloadQueryTask", "sceBgftServiceDownloadQueryTaskSection"};
    for (int i = 0; i < 4; i++) {
        void* fn = anySym(lib, qnames[i]);
        Hook* h = &g_dll.query[i];
        h->name = qnames[i] + 10;
        h->json = 19;
        const char* why = "not found";
        const int rc = fn ? mcwHookNative(h, fn, &why) : -1;
        logLine("dllist: %s %p: %s%s", qnames[i], fn, rc == 0 ? "hooked" : "not hooked: ", rc == 0 ? "" : why);
    }
    static const char* const names[2] = {"sceBgftServiceDownloadGetProgress", "sceBgftServiceIntDownloadGetProgress"};
    for (int i = 0; i < 2; i++) {
        void* fn = anySym(lib, names[i]);
        Hook* h = &g_dll.hooks[i];
        h->name = names[i] + 10;  // ServiceDownloadGetProgress, ServiceIntDownloadGetProgress
        h->json = 18;
        const char* why = "not found";
        const int rc = fn ? mcwHookNative(h, fn, &why) : -1;
        if (rc == 0) g_dll.hooked = 1;
        logLine("dllist: %s %p: %s%s", names[i], fn, rc == 0 ? "hooked" : "not hooked: ", rc == 0 ? "" : why);
    }
    logLine("dllist: unregister %p, stop %p, pause %p", (void*)g_dll.unregister, (void*)g_dll.stop, (void*)g_dll.pause);
    // The list's other calls for a task (1.11, logged only): whichever of
    // these names libSceBgft has.
    static const char* const inames[8] = {
        "sceBgftServiceIntDownloadGetTaskInfo",     "sceBgftServiceDownloadGetTaskInfo",
        "sceBgftServiceIntGetTaskInfoValueInt",     "sceBgftServiceIntGetTaskInfoValueUInt",
        "sceBgftServiceIntGetTaskInfoValueString",  "sceBgftServiceGetTaskInfoValueInt",
        "sceBgftServiceIntDownloadGetTaskInfoValueInt", "sceBgftServiceIntDownloadGetPlayGoProgress"};
    for (int i = 0; i < 8; i++) {
        void* fn = anySym(lib, inames[i]);
        if (!fn) {
            logLine("dllist: %s: not found", inames[i]);
            continue;
        }
        Hook* h = &g_dll.info[i];
        h->name = inames[i] + 10;
        h->json = 20;
        const char* why = "";
        const int rc = mcwHookNative(h, fn, &why);
        logLine("dllist: %s %p: %s%s", inames[i], fn, rc == 0 ? "hooked (logged only)" : "not hooked: ", rc == 0 ? "" : why);
    }
    // Left from a crash or an earlier boot.
    char text[1024];
    const int n = readText(DLL_TASKS, text, sizeof(text));
    for (int at = 0; at < n;) {
        int32_t t = 0;
        while (at < n && text[at] >= '0' && text[at] <= '9') t = t * 10 + (text[at++] - '0');
        if (t > 0) dllUnregister(t);
        while (at < n && (text[at] < '0' || text[at] > '9')) at++;
    }
    sysUnlink(DLL_TASKS);
    return g_dll.hooked;
}

static void dllSave(void) {
    char text[DLL_MAX * 12 + 4];
    size_t at = 0;
    for (int k = 0; k < g_dll.count; k++) at += (size_t)fmt(text + at, sizeof(text) - at, "%d\n", g_dll.e[k].task);
    text[at] = 0;
    writeText(DLL_TASKS, text);
}

// One item's placeholder: registered, paused, its task (or <= 0).
static int32_t dllRegister(int i) {
    static uint32_t serial;
    __typeof__(g_dls.item[0])* it = &g_dls.item[i];
    // A content id of its own each time: XXYYYY-TITLEID_00-<16 letters and digits>.
    char id[40];
    fmt(id, sizeof(id), "IV0000-SCMP00001_00-SCDL%03u%09lu", serial++ % 1000,
        (unsigned long)(sysNowUs() / 1000u % 1000000000u));
    BgftParam p;
    memset(&p, 0, sizeof(p));
    p.userId = g_bgft.user;
    p.entitlementType = 5;
    p.id = id;
    p.contentUrl = it->arg[0];
    char name[160];
    if (g_dll.trying) fmt(name, sizeof(name), "[%c] %s", g_dllTry[i % 6].letter, it->title[0] ? it->title : it->id);
    else fmt(name, sizeof(name), "%s", it->title[0] ? it->title : it->id);
    p.contentName = name;
    p.iconPath = "";
    p.playgoScenarioId = "0";
    p.option = 0x10000;
    p.packageType = "";
    p.packageSubType = "";
    p.packageSize = it->size;
    int32_t task = -1;
    const int rc = g_bgft.reg(&p, &task);
    logLine("dllist: item %d (%s) registered: 0x%08X, task %d", i, p.contentName, (unsigned)rc, task);
    if (rc < 0 || task <= 0) return -1;
    // Paused at once (0.98): the service marked 0.97's failed within a
    // second, never started. (Not always enough: 1.01's log, one failed
    // 9 s later; dllRepair replaces it.)
    if (g_dll.pause && !sysExists(SC_BGM_DIR "/dl-list-nopause")) {
        BgftProgress pr;
        memset(&pr, 0, sizeof(pr));
        g_bgft.progress(task, &pr);
        const uint32_t b0 = pr.bits;
        const int32_t e0 = pr.errorResult;
        const int prc = g_dll.pause(task);
        memset(&pr, 0, sizeof(pr));
        g_bgft.progress(task, &pr);
        logLine("dllist: task %d: bits 0x%x error 0x%08X; paused 0x%08X; bits 0x%x error 0x%08X", task, b0,
                (unsigned)e0, (unsigned)prc, pr.bits, (unsigned)pr.errorResult);
    }
    return task;
}

// Placeholders the service failed, replaced by new ones (1.02), on the
// downloads' thread (dlStatus): at most 3 times each, so one the service
// fails at once doesn't come back for ever.
static void dllRepair(void) {
    int changed = 0;
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        if (!g_dll.e[k].failed) continue;
        if (g_dll.e[k].tries >= 3) continue;
        g_dll.e[k].tries++;
        const int32_t old = g_dll.e[k].task;
        const int32_t task = dllRegister(g_dll.e[k].item);
        if (task <= 0) continue;
        g_dll.e[k].lastParam = 0;
        g_dll.e[k].logged = 0;
        __atomic_store_n(&g_dll.e[k].task, task, __ATOMIC_RELEASE);
        g_dll.e[k].failed = 0;
        logLine("dllist: task %d failed by the service: replaced by task %d (try %u)", old, task, g_dll.e[k].tries);
        dllUnregister(old);
        changed = 1;
    }
    if (changed) dllSave();
}

static void dllRemoveAll(void);

// A list beginning: a placeholder for each of its url and pages items.
static void dlListBegin(void) {
    dllRemoveAll();  // the last list's, shown finished till now
    if (sysExists(SC_BGM_DIR "/dl-nolist") || !dllSetup()) return;
    char text[256];
    // What ours answer (1.03), from the shell's own declarations
    // (Sce.Vsh.Orbis.Bgft: TaskProgress, TaskParam and their enums) and
    // every value logged so far:
    //   progress bits: state (Stop, Ready, Run, Complete) in 0-1, runstate
    //     (Standby, Prepare, Download, Install, Complete, ...) in 2-4,
    //     paused 5, subtype (6 Game, 8 GamePatch, 12 Package) in 6-9;
    //   query param: type (1 Download) in 0-3, subtype 4-7, state 8-9,
    //     runstate 14-16, paused 17, an error 26.
    // The list's look is keyed by (type, state, runstate) for the subtype:
    // 1.01 answered a game's (0x10008261, subtype 6), and the list, finding
    // no game for it, said "Cannot download". Ours stay Package (12).
    g_dll.haveGoing = g_dll.haveWaiting = g_dll.haveDone = 1;
    g_dll.trying = sysExists(SC_BGM_DIR "/dl-list-try");
    if (g_dll.trying) logLine("dllist: trying finished answers (dl-list-try): item i gets [A-F][i %% 6]");
    g_dll.bitsGoing = 0x30a;    // Package, Run, Download
    g_dll.bitsWaiting = 0x301;  // Package, Ready, Standby
    // Done (1.09, from 1.08's side-by-side try, dl-list-try): Complete/
    // Complete with the param's type 3, as the PS4's own finished downloads
    // have (0x...63), shows the music-note icon and no warning. (Complete/
    // Complete with type 1 read "waiting" (1.05) or, with bit 20, "Queued
    // for downloading"; Stop/Download with type 1 "Cannot download"; Stop/
    // Complete and Run/Install nothing, a "?" icon.)
    // 1.13: finished ones go to section 9 (see dlListQueryHook) and answer
    // as the PS4's own finished downloads there: Stop/Download with every
    // byte, the param's type 3 (0x...63). (1.07 answered these in section 3:
    // the music-note icon, no warning.)
    g_dll.bitsDone = 0x308;     // Package, Stop, Download
    g_dll.paramGoing = 0x82c1;   // Download, Package, Run, Download
    g_dll.paramWaiting = 0x1c1;  // Download, Package, Ready, Standby (as registered)
    g_dll.paramDone = 0x80c3;    // type 3, Package, Stop, Download (as 0x10008063)
    if (readText(SC_BGM_DIR "/dl-list-bits.txt", text, sizeof(text)) > 0) {
        if (dllHex(text, "going", &g_dll.bitsGoing)) g_dll.haveGoing = 1;
        dllHex(text, "waiting", &g_dll.bitsWaiting);
        dllHex(text, "done", &g_dll.bitsDone);
        dllHex(text, "param-waiting", &g_dll.paramWaiting);
        dllHex(text, "param-going", &g_dll.paramGoing);
        dllHex(text, "param-done", &g_dll.paramDone);
    }
    if (g_bgft.user == -1) bgftUser();
    for (int i = 0; i < g_dls.count && g_dll.count < DLL_MAX; i++) {
        __typeof__(g_dls.item[0])* it = &g_dls.item[i];
        if (memcmp(it->kind, "url", 4) && memcmp(it->kind, "pages", 6)) continue;
        const int32_t task = dllRegister(i);
        if (task <= 0) continue;
        const int k = g_dll.count;
        g_dll.e[k].item = i;
        g_dll.e[k].t0 = g_dll.e[k].b0 = 0;
        g_dll.e[k].logged = 0;
        g_dll.e[k].failed = 0;
        g_dll.e[k].tries = 0;
        g_dll.e[k].lastParam = 0;
        g_dll.e[k].task = task;
        __atomic_store_n(&g_dll.count, k + 1, __ATOMIC_RELEASE);
    }
    if (g_dll.count) dllSave();
}

// A list ended: its placeholders shown whole for 10 s (unless another list
// or the app comes first), then removed.
static void dllRemoveAll(void) {
    const int n = g_dll.count;
    if (!n) return;
    __atomic_store_n(&g_dll.count, 0, __ATOMIC_RELEASE);
    sysSleepMs(50);  // a call answering one of them, done
    for (int k = 0; k < n; k++) dllUnregister(g_dll.e[k].task);
    sysUnlink(DLL_TASKS);
}

// A list ended. Its placeholders stay, shown finished, as the PS4's own
// downloads do (1.04: 1.03 removed them 10 s after, and the entries went):
// removed when the app takes the list back, when the next list begins, and
// at the next boot's first start of the module (dlListLeftovers).
static void dlListEnd(void) {
    if (g_dls.takeBack) dllRemoveAll();
}

// At the module's start: any placeholders left from an earlier boot (or a
// crash), removed. Only when there are some: nothing is hooked otherwise.
static void dlListLeftovers(void) {
    if (sysExists(DLL_TASKS)) dllSetup();
}
