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
int mcwDbNotifyPost(void);  // mcw.c (1.19): the list told its table changed
int mcwDbNotifyPostAttached(void);  // mcw.c (1.42): the same from the downloads' thread
int mcwDbNotifyKnown(void);         // mcw.c (1.42): the subscriptions' strings seen
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
    Hook ctl[15];   // start, resume, pause, stop, unregister and the Int ones: the list's buttons (1.15, 1.29)
    volatile int selfCall;  // our own pause of a task, not the list's (1.29)
    Hook appSize[3];  // the service's app data size call (1.25)
    uint32_t appSizeLogged;
    int dlThread;   // the downloads' thread, whose calls are ours
    uint64_t endedUs;  // when the last list ended, 0 if one is going
    int restored;      // its entries are from before a reboot (1.29)
    uint32_t titleLogged;
    volatile int posting;  // the list told of changes (DbNotify, 1.42): finished ones leave section 3 at once
    int moveNow;       // dl-list-move-now: finished ones out of section 3 at once (1.39, a test)
    int noPausedParam; // dl-list-param-nopause: paused only in the progress answers (1.35, a test)
    volatile int wantPost;  // an item changed state: tell the list (1.19)
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
        volatile int swapped;      // its finished placeholder (the app's title id) in (1.37)
        volatile int32_t oldTask;  // the one it replaced (dllRepair), still the list's row's for a while (1.33)
        int item;
        uint64_t t0, b0;  // when it began to come, and its bytes then
        uint32_t logged;
        uint32_t lastWas, lastSaid;  // the service's bits and ours, last logged (0.99)
        uint64_t lastParam;          // its param in the last query logged (0.99)
        volatile int failed;         // the service failed it: to be replaced (1.02)
        uint32_t tries;              // replaced so far
        volatile int movedOut;       // finished, and left out of a section 3 answer since (1.14)
        volatile int shownIn;        // the section (5, 9) it has been put in an answer of (1.16)
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
    // Finished, but the list has not asked for section 3 since (1.14): its
    // row there still has the going state it read, and finished answers
    // under it read "Cannot download" (1.13) until the list was opened
    // again. So it answers going, every byte there, until it has been left
    // out of a section 3 answer (and put in section 9's).
    int st = it->state;
    // Finished, not moved to section 9 yet (1.26): as the PS4's own game
    // does when finished (package 1099: Stop/Download with every byte,
    // ...88, preparing 0). The list takes a row's state and runstate from
    // each progress answer (UpdateProgress's b__1, the debug dump), so it
    // shows finished at once; 1.25's "going, every byte, 0 s" read
    // "(1 Second Left)" till the row moved.
    // 1.27: not Stop/Download. The list keeps a row's type from when it
    // was made (going: type 1), and its map (DownloadItemBehavior's .cctor,
    // the debug dump) sends type 1, Stop, Download to a lambda that always
    // returns DownloadError: "Cannot download". Type 1, Complete, Complete
    // goes to the one that returns Installed for a game.
    if (st == DLS_DONE && !g_dll.e[k].movedOut) {
        const uint64_t all = it->bytes ? it->bytes : it->size;
        pr->errorResult = 0;
        pr->bits = (pr->bits & ~0x3FFu) | 0x193u;  // Game, Complete, Complete
        pr->length = pr->lengthTotal = all;
        pr->transferred = pr->transferredTotal = all;
        pr->numIndex = pr->numTotal = 1;
        pr->preparingPercent = 0;
        pr->restSec = pr->restSecTotal = 0;
        return;
    }
    if (st == DLS_FAILED || st == DLS_GONE) {
        // Failed for good (1.15): as package 1097, which the service failed
        // (1.05's log): Stop/Download, its error, in section 5. The error
        // the service gives a placeholder it fails, 0x80990033.
        pr->errorResult = (int32_t)0x80990033;
        pr->bits = (pr->bits & ~0x3FFu) | 0x308u;
        pr->length = pr->lengthTotal = it->size ? it->size : it->bytes + 1;
        pr->transferred = pr->transferredTotal = it->bytes;
        pr->numIndex = pr->numTotal = 1;
        pr->preparingPercent = 0;
        pr->restSec = pr->restSecTotal = 0xFFFFFFFFu;
        return;
    }
    const int moved = g_dll.e[k].movedOut;
    if (st == DLS_DONE && !moved) st = DLS_GOING;
    const uint64_t bytes = it->state == DLS_DONE && !moved ? (it->bytes ? it->bytes : it->size) : it->bytes;
    uint64_t size = it->size;
    if (it->state == DLS_DONE) size = bytes ? bytes : size;
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
            rest = (uint32_t)((left * (us / 1000u) / came + 999u) / 1000u);  // rounded up (1.22)
            if (!rest && left) rest = 1;
        }
    }
    if (it->state == DLS_DONE) rest = 0;
    pr->errorResult = 0;
    // The low 10 bits only (1.03): the rest is not the state, and the
    // service's own (varying) bits stay.
    const uint32_t keep = pr->bits & ~0x3FFu;
    if (st == DLS_GOING && g_dll.haveGoing) pr->bits = keep | (g_dll.bitsGoing & 0x3FFu);
    else if (st == DLS_DONE && g_dll.trying) pr->bits = keep | g_dllTry[i % 6].bits;
    else if (st == DLS_DONE && g_dll.haveDone) pr->bits = keep | (g_dll.bitsDone & 0x3FFu);
    else if (st != DLS_GOING && st != DLS_DONE && g_dll.trying) pr->bits = keep | g_dllTryWait[i % 6].bits;
    else if (st != DLS_GOING && st != DLS_DONE && g_dll.haveWaiting) pr->bits = keep | (g_dll.bitsWaiting & 0x3FFu);
    // Paused from the list (1.29): its paused bit (5), so it reads
    // "Paused" with Resume (GetVisualState: type 1 and paused).
    if (it->paused && st != DLS_DONE) pr->bits |= 0x20u;
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

static void dllSave(void);

// One of ours gone from the service (0x80990019): deleted from the list
// (triangle). Not fetched any more if not done (1.29: 1.28 fetched a
// deleted song), and no longer ours to answer for.
static void dllGone(int k, const char* seenBy) {
    const int32_t task = g_dll.e[k].task;
    if (task <= 0) return;
    g_dll.e[k].task = 0;
    const int di = g_dll.e[k].item;
    logLine("dllist: task %d (item %d) is gone (0x80990019, %s): deleted from the list", task, di, seenBy);
    if (di >= 0 && di < g_dls.count && g_dls.item[di].state != DLS_DONE) g_dls.item[di].cancel = 1;
    dllSave();
}

// After the service's progress call (taskId, progress), on the caller's
// thread, the shell's list among them (mcw.c's thunk, kind 18).
void dlListHook(Frame* f) {
    const int32_t task = (int32_t)f->gpr[0];
    BgftProgress* pr = (BgftProgress*)(uintptr_t)f->gpr[1];
    const uint32_t n = __atomic_add_fetch(&g_dll.calls, 1, __ATOMIC_RELAXED);
    // An item of ours changed state: the list told, as the service's own
    // writes tell it, so it asks for its sections again now, not when it
    // next happens to (1.19: 1.15's re-read 48 s after). Here, on the list's
    // own thread, a thread Mono knows.
    // (1.23: none of DbNotify's four ways made the list ask again, 1.21's
    // log; not called any more.)
    __atomic_store_n(&g_dll.wantPost, 0, __ATOMIC_RELEASE);
    if (!pr || hookReadable(pr, 0x38) != 0x38 || !(hookProt(pr) & 2)) return;
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        if (g_dll.e[k].task != task || task <= 0) continue;
        const uint32_t was = pr->bits;
        const int32_t err = pr->errorResult;
        const int rc = (int)(int32_t)f->retRax;
        if (rc == (int)0x80990019) {
            // Not there (1.15): deleted from the list (triangle), as our own
            // removals answer (0x80990019). No longer ours to answer for.
            dllGone(k, "progress");
            return;
        }
        if (err && !g_dll.e[k].failed) g_dll.e[k].failed = 1;  // replaced by dllRepair (1.02)
        dllFill(k, pr);
        // TaskProgress's last field, percentTotal (a float after
        // localCopyPercent, 0x40), once real tasks have shown it is there.
        if (g_dll.pctOk && hookReadable(pr, 0x44) == 0x44) {
            float pct = pr->lengthTotal ? (float)((double)pr->transferredTotal * 100.0 / (double)pr->lengthTotal) : 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            memcpy((uint8_t*)pr + 0x40, &pct, 4);
        }
        f->retRax = 0;
        // Whenever the service's bits or ours change (0.99), and every 200th;
        // and while going, every 5 s (1.20: bytes and time left as they go).
        const uint32_t seen = g_dll.e[k].logged++;
        static uint64_t lastGoingLog;
        const int goingTick = (pr->bits & 0x1F) == 0x0a && sysNowUs() - lastGoingLog > 5000000u;
        if (goingTick) lastGoingLog = sysNowUs();
        const int changed = !seen || goingTick || was != g_dll.e[k].lastWas || pr->bits != g_dll.e[k].lastSaid;
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

// The section an item of ours belongs in (1.15), as the PS4's own move
// (1.00, 1.05: going in 3, finished first in 9, failed in 5).
static int dllSection(int k) {
    const int i = g_dll.e[k].item;
    if (g_dll.e[k].task <= 0 || i < 0 || i >= g_dls.count) return 0;
    const int st = g_dls.item[i].state;
    return st == DLS_DONE ? 9 : st == DLS_FAILED || st == DLS_GONE ? 5 : 3;
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
                    if (g_dll.e[k].task == id && id > 0 && dllSection(k) != 5) {
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
        if (h == &g_dll.query[1] && (f->gpr[2] == 3 || f->gpr[2] == 5 || f->gpr[2] == 9) && count && cnt <= 960 &&
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
                        (void)i;
                        // Only once the list has had it in its new section's
                        // answer (1.16): 1.15's left section 3 at 55.4 s, the
                        // list asked for section 9 at 79.9 s, and between
                        // them they were in neither, gone from the screen.
                        // (1.41: at once too once made again (dlListDone):
                        // its registration has the list read section 3 and
                        // section 9 within 0.1 s (1.39's log, both ways),
                        // so it isn't out of both for long.)
                        // (1.39: with dl-list-move-now, at once: the
                        // Notifications tile counts every section 3 task
                        // not Run as waiting, finished ones too, and read
                        // "Downloading: 1 Waiting: 5" till all were done.)
                        // The task it replaced (1.41): its removal not gone
                        // through yet, it came in one read with the service's
                        // own param (1.39's log: 1169:200c1 at 138.778).
                        if (g_dll.e[k].swapped && g_dll.e[k].oldTask == id && id > 0) out = 1;
                        if (g_dll.e[k].task == id && id > 0 && dllSection(k) != 3 &&
                            (g_dll.e[k].shownIn == dllSection(k) || g_dll.moveNow || g_dll.e[k].swapped ||
                             g_dll.posting)) {
                            out = 1;
                            if (!g_dll.e[k].movedOut) {
                                g_dll.e[k].movedOut = 1;
                                logLine("dllist: task %d out of section 3, into section %d", id, dllSection(k));
                            }
                        }
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
            } else if (f->gpr[2] == 9 || f->gpr[2] == 5) {
                // Ours finished (9) or failed (5), newest first, before the
                // PS4's own.
                const int want = (int)f->gpr[2];
                int32_t add[DLL_MAX];
                uint32_t n = 0;
                for (int k = count - 1; k >= 0; k--) {
                    if (k >= DLL_MAX || dllSection(k) != want) continue;
                    int there = 0;
                    for (uint32_t j = 0; j < cnt && hookReadable(res + j * 8, 8) == 8; j++) {
                        int32_t id;
                        memcpy(&id, res + j * 8, 4);
                        if (id == g_dll.e[k].task) there = 1;
                    }
                    if (there) g_dll.e[k].shownIn = want;
                    else add[n++] = g_dll.e[k].task;
                }
                if (n && cnt + n <= max && hookReadable(res, (size_t)(cnt + n) * 8) == (size_t)(cnt + n) * 8 &&
                    (hookProt(res) & 2) && (hookProt(res + (cnt + n) * 8 - 1) & 2)) {
                    if (cnt) memmove((uint8_t*)res + n * 8, res, (size_t)cnt * 8);
                    for (uint32_t j = 0; j < n; j++) {
                        memcpy((uint8_t*)res + j * 8, &add[j], 4);
                        memset((uint8_t*)res + j * 8 + 4, 0, 4);  // its param, written below
                        for (int k = 0; k < count && k < DLL_MAX; k++)
                            if (g_dll.e[k].task == add[j] && !g_dll.e[k].shownIn) {
                                g_dll.e[k].shownIn = want;
                                g_dll.wantPost = 1;  // and asked again, to leave section 3 (1.19)
                                logLine("dllist: task %d put in section %d", add[j], want);
                            }
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
                    int st = i >= 0 && i < g_dls.count ? g_dls.item[i].state : DLS_WAITING;
                    // Finished but still in section 3 (1.20): its going
                    // param, as its progress answers going (1.19's log: 80c3
                    // in section 3 beside going progress).
                    // (Not in section 9's answer, where it is finished: 1.20's
                    // log had them there with the going param.)
                    // (1.26: finished, still in section 3, as a finished game's
                    // param, 0x10008063, as its progress answers.)
                    const int doneHere = st == DLS_DONE && !g_dll.e[k].movedOut && f->gpr[2] != 9;
                    // (1.27: type 1, Game, Complete/Complete, 0x10010361,
                    // so a row made again here is type 1 too and reads
                    // Installed; type 3 has no Complete/Complete entry.)
                    const uint32_t paused = !g_dll.noPausedParam && i >= 0 && i < g_dls.count && g_dls.item[i].paused &&
                                                    st != DLS_DONE ? 0x20000u : 0;
                    const uint32_t prm = paused | (doneHere ? 0x10010361u
                                       : st == DLS_FAILED || st == DLS_GONE ? 0x40080c1u  // 1097's 0x14008061: error, Stop/Download; Package
                                       : st == DLS_DONE && g_dll.trying ? g_dllTry[i % 6].param
                                       : st != DLS_DONE && st != DLS_GOING && g_dll.trying ? g_dllTryWait[i % 6].param
                                       : st == DLS_DONE    ? g_dll.paramDone
                                       : st == DLS_GOING ? g_dll.paramGoing
                                                         : g_dll.paramWaiting);
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
        if (g_dll.e[k].task == task && task > 0) {
            ours = 1;
            // Gone (1.30): 1.29's song 6 was deleted from the list, its task
            // info said 0x80990019 at 27.2 s, and no progress call came
            // before it was done and put back in section 9.
            if ((int)(int32_t)f->retRax == (int)0x80990019) dllGone(k, "task info");
            // Finished: the app's own title id in its content id (1.36).
            // 1.32 gave each its own (SCDLnnnnn, no app's) so Cancel and
            // Delete takes only that one, and since then a finished one's
            // row reads nothing at all (1.34): a finished game's row has
            // Start (the app, by title id) and Clear from list, and there is
            // no app. The list asks again for a finished one's (1.34's log:
            // song 3 at 50.3 s, done at 39.5 s). Not with dl-list-own-title.
            char* cid = (char*)(uintptr_t)f->gpr[2];
            const int i = g_dll.e[k].item;
            if (f->gpr[1] == 0xf && (int)(int32_t)f->retRax >= 0 && i >= 0 && i < g_dls.count &&
                g_dls.item[i].state == DLS_DONE && strlen(g_dls.app) == 9 && (uintptr_t)cid > 0x10000 &&
                hookReadable(cid, 20) == 20 && (hookProt(cid) & 2) && !memcmp(cid, "IV0000-SCDL", 11) &&
                cid[16] == '_' && !sysExists(SC_BGM_DIR "/dl-list-own-title")) {
                memcpy(cid + 7, g_dls.app, 9);
                if (g_dll.titleLogged++ < 12) logLine("dllist: task %d finished: title %s given", task, g_dls.app);
            }
        }
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

// The list's buttons (1.15): the service's start, resume, pause, stop and
// unregister, after a call for one of ours not made by the downloads'
// thread. Start or resume would have the service fetch it itself, which it
// can't (0.92-0.94): paused again at once. Unregister (triangle, delete):
// no longer ours. Pause and stop: logged; the downloads go on.
void dlListCtlHook(Frame* f) {
    if (scePthreadGetthreadid() == g_dll.dlThread || g_dll.selfCall) return;
    const char* name = f->hook->name;
    // Every call from the list logged, its first 60 (1.33), with its
    // arguments: the task may not be the first (the state calls).
    static uint32_t seenCalls;
    if (seenCalls++ < 60)
        logLine("dllist: %s(%lx, %lx, %lx) = 0x%08X  [thread %d]", name, (unsigned long)f->gpr[0],
                (unsigned long)f->gpr[1], (unsigned long)f->gpr[2], (unsigned)f->retRax, scePthreadGetthreadid());
    // The "All" ones: every one of ours.
    const int all = hasText(name, "All");
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        const int32_t task = g_dll.e[k].task;
        if (task <= 0) continue;
        const int32_t a0 = (int32_t)f->gpr[0], a1 = (int32_t)f->gpr[1], old = g_dll.e[k].oldTask;
        if (!all && a0 != task && a1 != task && (old <= 0 || (a0 != old && a1 != old))) continue;
        logLine("dllist: %s(%d) [ours, item %d] from the list = 0x%08X  [thread %d]", name, task, g_dll.e[k].item,
                (unsigned)f->retRax, scePthreadGetthreadid());
        const int i = g_dll.e[k].item;
        __typeof__(g_dls.item[0])* it = i >= 0 && i < g_dls.count ? &g_dls.item[i] : NULL;
        const int open = it && it->state != DLS_DONE && it->state != DLS_GONE && it->state != DLS_FAILED &&
                         it->state != DLS_CANCELLED;
        if (hasText(name, "Unregister")) {
            g_dll.e[k].task = 0;
            if (open) it->cancel = 1;  // deleted: not fetched (1.29)
        } else if (hasText(name, "Pause") || hasText(name, "Stop")) {
            // Pause (1.29): the module's fetch of it stops after the piece
            // it is writing, and the next one goes; Resume brings it back.
            if (open) it->paused = 1;
        } else if (hasText(name, "Start") || hasText(name, "Resume") || hasText(name, "HighPriority")) {
            // (1.34: Resume is SetResumeState(task, 1), 1.33's log; Download
            // Now StartTask(task). Both re-paused at once below: 1.33 left
            // SetResumeState alone, and the service, trying to fetch the
            // placeholder itself, failed it ("Cannot download").)
            // Resume, or Download Now: next, after the one going (1.29).
            if (open) {
                it->paused = 0;
                if (!all) it->now = 1;
            }
            if (!hasText(name, "HighPriority") && g_dll.pause) {
                g_dll.selfCall = 1;  // the service still fetches nothing itself
                logLine("dllist: task %d paused again: 0x%08X", task, (unsigned)g_dll.pause(task));
                g_dll.selfCall = 0;
            }
        }
        g_dll.wantPost = 1;
        if (!all) return;
    }
}

// After the service's app data size call (taskId, out size), kind 23
// (1.25): for ours, the song's size, so the list's row (Game) has an
// AppDataSize and isn't "Calculating...". Logged: its first calls.
void dlListAppSizeHook(Frame* f) {
    const int32_t task = (int32_t)f->gpr[0];
    uint64_t* out = (uint64_t*)(uintptr_t)f->gpr[1];
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        if (g_dll.e[k].task != task || task <= 0) continue;
        const int i = g_dll.e[k].item;
        if (i < 0 || i >= g_dls.count) return;
        const uint64_t size = g_dls.item[i].size ? g_dls.item[i].size : g_dls.item[i].bytes + 1;
        const int rc = (int)(int32_t)f->retRax;
        if (out && hookReadable(out, 8) == 8 && (hookProt(out) & 2)) {
            *out = size;
            f->retRax = 0;
        }
        if (g_dll.appSizeLogged++ < 12)
            logLine("dllist: %s(%d) [ours, item %d] was 0x%08X: answered %lu  [thread %d]", f->hook->name, task, i,
                    (unsigned)rc, (unsigned long)size, scePthreadGetthreadid());
        return;
    }
    if (g_dll.appSizeLogged++ < 12)
        logLine("dllist: %s(%d) = 0x%08X, %lu  [thread %d]", f->hook->name, task, (unsigned)f->retRax,
                (unsigned long)(out && hookReadable(out, 8) == 8 ? *out : 0), scePthreadGetthreadid());
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


// Removals the service refused (0x80990019 early in a boot, 1.28's log),
// tried again every 10 s from dlListIdle, up to 30 times (1.29).
static struct {
    int32_t task[DLL_MAX];
    int n, tries;
    uint64_t lastUs;
} g_dllLater;

static void dllUnregisterLater(int32_t task) {
    const int rc = g_dll.unregister ? g_dll.unregister(task) : -1;
    // 0x80990019: not there (1.29's boot: every old task, the new ones
    // numbered from 1102), nothing to try again.
    const int again = rc < 0 && rc != (int)0x80990019;
    logLine("dllist: task %d from before removed: 0x%08X%s", task, (unsigned)rc,
            again ? ", tried again later" : rc < 0 ? " (not there)" : "");
    if (again && g_dllLater.n < DLL_MAX) g_dllLater.task[g_dllLater.n++] = task;
}

static void dllLaterIdle(void) {
    if (!g_dllLater.n || sysNowUs() - g_dllLater.lastUs < 10000000u) return;
    g_dllLater.lastUs = sysNowUs();
    const int last = ++g_dllLater.tries >= 30;
    int w = 0;
    for (int j = 0; j < g_dllLater.n; j++) {
        const int rc = g_dll.unregister ? g_dll.unregister(g_dllLater.task[j]) : -1;
        if (rc < 0 && rc != (int)0x80990019 && !last) g_dllLater.task[w++] = g_dllLater.task[j];
        if (rc >= 0 || last)
            logLine("dllist: task %d from before removed: 0x%08X (try %d)", g_dllLater.task[j], (unsigned)rc,
                    g_dllLater.tries);
    }
    g_dllLater.n = w;
}

static void dllAnswers(void);

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
    // The service's app data size call (1.25): whichever of these names
    // libSceBgft has, as BgftWrapper's other calls are its Int functions.
    static const char* const anames[3] = {"sceBgftServiceIntDownloadGetApplicationDataSize",
                                          "sceBgftServiceDownloadGetApplicationDataSize",
                                          "sceBgftServiceIntDownloadGetAppDataSize"};
    for (int i = 0; i < 3; i++) {
        void* fn = anySym(lib, anames[i]);
        if (!fn) {
            logLine("dllist: %s: not found", anames[i]);
            continue;
        }
        Hook* h = &g_dll.appSize[i];
        h->name = anames[i] + 10;
        h->json = 23;
        const char* why = "";
        const int rc = mcwHookNative(h, fn, &why);
        logLine("dllist: %s %p: %s%s", anames[i], fn, rc == 0 ? "hooked" : "not hooked: ", rc == 0 ? "" : why);
    }
    // The list's buttons (1.15).
    // 1.29: and the Int ones. 1.28's Download Now, Pause and delete called
    // none of the first five (nothing logged); the list's other calls
    // (progress, queries, task info) are all the Int ones.
    static const char* const cnames[15] = {"sceBgftServiceDownloadStartTask", "sceBgftServiceDownloadResumeTask",
                                           "sceBgftServiceDownloadPauseTask", "sceBgftServiceDownloadStopTask",
                                           "sceBgftServiceIntDownloadUnregisterTask",
                                           "sceBgftServiceIntDownloadStartTask", "sceBgftServiceIntDownloadResumeTask",
                                           "sceBgftServiceIntDownloadPauseTask", "sceBgftServiceIntDownloadStopTask",
                                           "sceBgftServiceIntDownloadSetHighPriority",
                                           "sceBgftServiceIntDownloadSetStartState",
                                           // 1.33: Resume left the row paused, no call of ours
                                           // logged; these may be what it calls.
                                           "sceBgftServiceIntDownloadSetResumeState",
                                           "sceBgftServiceIntDownloadResumeTaskAll",
                                           "sceBgftServiceIntDownloadStartTaskAll",
                                           "sceBgftServiceDownloadResumeTaskAll"};
    void* seen[15] = {0};
    for (int i = 0; i < 15; i++) {
        void* fn = anySym(lib, cnames[i]);
        if (!fn) {
            logLine("dllist: %s: not found", cnames[i]);
            continue;
        }
        int twice = 0;  // one function under two names: hooked once
        for (int j = 0; j < i; j++) twice |= seen[j] == fn;
        seen[i] = fn;
        if (twice) {
            logLine("dllist: %s %p: hooked already under another name", cnames[i], fn);
            continue;
        }
        Hook* h = &g_dll.ctl[i];
        h->name = cnames[i] + 10;
        h->json = 21;
        const char* why = "";
        const int rc = mcwHookNative(h, fn, &why);
        logLine("dllist: %s %p: %s%s", cnames[i], fn, rc == 0 ? "hooked" : "not hooked: ", rc == 0 ? "" : why);
    }
    // Left from a crash or an earlier boot (1.29): finished and failed ones
    // taken back, answered as before and shown as they were, removed as a
    // list's are (the next list, the app, or 30 minutes); the rest (cut off
    // by the reboot) removed, tried again while the service says
    // 0x80990019 (1.28's boot: every removal at the module's start).
    static char text[DLL_MAX * 64 + 4];
    const int n = readText(DLL_TASKS, text, sizeof(text) - 1);
    int adopted = 0;
    for (int at = 0; at > -1 && at < n;) {
        int end = at;
        while (end < n && text[end] != '\n') end++;
        text[end] = 0;
        uint64_t v[4] = {0, 0, 0, 0};
        int got = 0;
        for (const char* p = text + at; *p && got < 4;) {
            while (*p == ' ') p++;
            if (*p < '0' || *p > '9') break;
            while (*p >= '0' && *p <= '9') v[got] = v[got] * 10 + (uint64_t)(*p++ - '0');
            got++;
        }
        const int32_t t = (int32_t)v[0];
        const int st = got >= 4 ? (int)v[1] : -1;
        if (t > 0 && (st == DLS_DONE || st == DLS_FAILED || st == DLS_GONE) && g_dls.count < DL_MAX &&
            g_dll.count < DLL_MAX && !g_dls.running) {
            const int i = g_dls.count++;
            memset(&g_dls.item[i], 0, sizeof(g_dls.item[i]));
            fmt(g_dls.item[i].id, sizeof(g_dls.item[i].id), "restored-%d", t);
            g_dls.item[i].state = st;
            g_dls.item[i].bytes = v[2];
            g_dls.item[i].size = v[3];
            g_dls.item[i].task = t;
            const int k = g_dll.count;
            memset(&g_dll.e[k], 0, sizeof(g_dll.e[k]));
            g_dll.e[k].item = i;
            g_dll.e[k].task = t;
            __atomic_store_n(&g_dll.count, k + 1, __ATOMIC_RELEASE);
            adopted++;
        } else if (t > 0) {
            dllUnregisterLater(t);
        }
        at = end + 1;
    }
    if (adopted) {
        g_dll.restored = 1;
        g_dll.endedUs = sysNowUs();
        dllAnswers();
        logLine("dllist: %d finished or failed from before the reboot, shown as they were", adopted);
        dllSave();
    } else {
        sysUnlink(DLL_TASKS);
    }
    return g_dll.hooked;
}

// The tasks, each with its item's state, bytes and size (1.29: "<task>
// <state> <bytes> <size>"), so a finished one still reads finished after a
// reboot (1.28 removed them at the next start, which failed then:
// 0x80990019, and they read "Paused").
static void dllSave(void) {
    char text[DLL_MAX * 64 + 4];
    size_t at = 0;
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        const int i = g_dll.e[k].item;
        const int ok = i >= 0 && i < g_dls.count;
        at += (size_t)fmt(text + at, sizeof(text) - at, "%d %d %lu %lu\n", g_dll.e[k].task,
                          ok ? g_dls.item[i].state : DLS_WAITING, ok ? (unsigned long)g_dls.item[i].bytes : 0ul,
                          ok ? (unsigned long)g_dls.item[i].size : 0ul);
    }
    text[at] = 0;
    writeText(DLL_TASKS, text);
}

// One item's placeholder: registered, paused, its task (or <= 0).
// A URL's size by HEAD (Content-Length), with the list's headers, 0 if not
// known (1.23).
static uint64_t dllHeadSize(const char* url, const char* extra) {
    uint64_t size = 0;
    if (!httpSetup()) return 0;
    const int tmpl = g_http.tmpl(g_http.ctx, "ShellCompanion/" SC_BGM_VERSION, 2, 1);
    const int conn = tmpl >= 0 ? g_http.conn(tmpl, url, 1) : -1;
    const int req = conn >= 0 ? g_http.req(conn, 2 /* HEAD */, url, 0) : -1;
    int code = 0, known = -1;
    if (req >= 0) {
        if (g_http.connectTimeout) g_http.connectTimeout(req, 10u * 1000000u);
        if (g_http.recvTimeout) g_http.recvTimeout(req, 10u * 1000000u);
        dlHeaders(req, g_dls.headers);
        dlHeaders(req, extra);
        if (g_http.send(req, NULL, 0) >= 0) {
            g_http.status(req, &code);
            g_http.length(req, &known, &size);
        }
        g_http.delReq(req);
    }
    if (conn >= 0) g_http.delConn(conn);
    if (tmpl >= 0) g_http.delTmpl(tmpl);
    return code >= 200 && code < 300 && known == 0 ? size : 0;
}

static int32_t dllRegisterAs(int i, const char* title);
static int32_t dllRegister(int i) { return dllRegisterAs(i, NULL); }

// title: the title id in its content id, NULL for one of its own.
static int32_t dllRegisterAs(int i, const char* title) {
    static uint32_t serial;
    __typeof__(g_dls.item[0])* it = &g_dls.item[i];
    // A content id of its own each time: XXYYYY-TITLEID_00-<16 letters and digits>.
    char id[40];
    // A title id of its own too (1.32): 1.31's all had SCMP00001, and
    // "Cancel and Delete" on one (a game's, by title id: CancelAndDelete
    // looks the app up with GetItemByTitleId) took every one of them off
    // the list. SCDLnnnnn, no app's: the serial from the clock so a reboot
    // doesn't start it again.
    if (!serial) serial = (uint32_t)(sysNowUs() / 1000u);
    const uint32_t n = serial++;
    if (title)
        fmt(id, sizeof(id), "IV0000-%.9s_00-SCDL%03u%09lu", title, n % 1000u,
            (unsigned long)(sysNowUs() / 1000u % 1000000000u));
    else
        fmt(id, sizeof(id), "IV0000-SCDL%05u_00-SCDL%03u%09lu", n % 100000u, n % 1000u,
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
    // The app's own icon (1.15; the list's app= line, from the client
    // library), so its rows aren't a "?".
    // The app's icon for this one first (icon=, 1.29: album art, say).
    char icon[MAX_PATH] = "";
    const char* given = dlItemIcon(i, icon, sizeof(icon));
    if (!given && g_dls.app[0]) fmt(icon, sizeof(icon), "/user/appmeta/%s/icon0.png", g_dls.app);
    else if (!given) icon[0] = 0;
    p.iconPath = given ? given : icon[0] && sysExists(icon) ? icon : "";
    if (i == 0 || !p.iconPath[0]) logLine("dllist: icon %s", p.iconPath[0] ? p.iconPath : "none");
    p.playgoScenarioId = "0";
    p.option = 0x10000;
    p.packageType = "";
    p.packageSubType = "";
    // Its size, as a package's is registered (1.23): the list showed ours as
    // "Calculating..." throughout, though answered bytes and time left, and
    // its states include Downloading_DataSizeUnknown. A url item's size by
    // HEAD when the list didn't give it.
    if (!it->size && !memcmp(it->kind, "url", 4)) {
        it->size = dllHeadSize(it->arg[0], it->arg[1]);
        logLine("dllist: item %d: size by HEAD: %lu", i, (unsigned long)it->size);
    }
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
    // A state changed since last seen (1.19): the list to be told.
    static int last[DLL_MAX];
    static int lastCount = -1;
    if (lastCount != g_dll.count) {
        for (int k = 0; k < DLL_MAX; k++) last[k] = -1;
        lastCount = g_dll.count;
    }
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        const int i = g_dll.e[k].item;
        const int st = i >= 0 && i < g_dls.count ? g_dls.item[i].state : -1;
        if (st != last[k]) {
            if (last[k] >= 0) {
                g_dll.wantPost = 1;
                changed = 1;  // its state kept (1.29)
            }
            last[k] = st;
        }
    }
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        if (!g_dll.e[k].failed || g_dll.e[k].task <= 0 || dllSection(k) == 5) continue;
        if (g_dll.e[k].tries >= 3) continue;
        g_dll.e[k].tries++;
        const int32_t old = g_dll.e[k].task;
        const int32_t task = dllRegister(g_dll.e[k].item);
        if (task <= 0) continue;
        g_dll.e[k].lastParam = 0;
        g_dll.e[k].logged = 0;
        g_dll.e[k].oldTask = old;
        __atomic_store_n(&g_dll.e[k].task, task, __ATOMIC_RELEASE);
        g_dll.e[k].failed = 0;
        logLine("dllist: task %d failed by the service: replaced by task %d (try %u)", old, task, g_dll.e[k].tries);
        dllUnregister(old);
        changed = 1;
    }
    if (changed) {
        dllSave();
        // The list and the Notifications tile told, as the service tells
        // them of its own (1.42): they read their sections again.
        if (!sysExists(SC_BGM_DIR "/dl-list-nopost") && mcwDbNotifyKnown()) {
            g_dll.posting = 1;  // before the post: the reads it brings come at once
            mcwDbNotifyPostAttached();
        }
    }
}

// What ours answer, set (1.29: also for those restored after a reboot).
static void dllAnswers(void) {
    char text[256];
    g_dll.haveGoing = g_dll.haveWaiting = g_dll.haveDone = 1;
    g_dll.trying = sysExists(SC_BGM_DIR "/dl-list-try");
    // A test (1.35): whether the param's paused bit changing is what has
    // the list make a resumed song's row again (1.34's log: rebuilt at the
    // section read after Resume, as the song above it left the section).
    g_dll.noPausedParam = sysExists(SC_BGM_DIR "/dl-list-param-nopause");
    g_dll.moveNow = sysExists(SC_BGM_DIR "/dl-list-move-now");
    if (g_dll.moveNow) logLine("dllist: dl-list-move-now: finished ones out of section 3 at once");
    if (g_dll.noPausedParam) logLine("dllist: dl-list-param-nopause: paused only in the progress answers");
    if (g_dll.trying) logLine("dllist: trying finished answers (dl-list-try): item i gets [A-F][i %% 6]");
    // 1.25, from the list's own code (the debug dump, code4.bin): a row
    // reads "Calculating..." while MapValueArgs.IsCalculating, (SubType 12
    // Package or 6 Game) and AppDataSize 0; and AppDataSize is set only for
    // Game downloads (type 1, subtype 6), by DownloadGetApplicationDataSize
    // (task info, and each progress poll, its subtype from our progress
    // bits). So going and waiting ones are Game, with the values the PS4's
    // own game tasks had (1.04/1.05: 1093 registered 0x10000161, 1098 going
    // 0x10008261 / ...8a), and the app data size call answered with the
    // song's size (dllAppSizeHook). Finished ones stay Package in section 9
    // (1.08's: the music-note icon, no warning).
    g_dll.bitsGoing = 0x18a;    // Game, Run, Download (package 1098)
    g_dll.bitsWaiting = 0x181;  // Game, Ready, Standby
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
    g_dll.paramGoing = 0x10008261;    // as package 1098 going
    g_dll.paramWaiting = 0x10000161;  // as package 1093 just registered
    g_dll.paramDone = 0x80c3;    // type 3, Package, Stop, Download (as 0x10008063)
    if (readText(SC_BGM_DIR "/dl-list-bits.txt", text, sizeof(text)) > 0) {
        if (dllHex(text, "going", &g_dll.bitsGoing)) g_dll.haveGoing = 1;
        dllHex(text, "waiting", &g_dll.bitsWaiting);
        dllHex(text, "done", &g_dll.bitsDone);
        dllHex(text, "param-waiting", &g_dll.paramWaiting);
        dllHex(text, "param-going", &g_dll.paramGoing);
        dllHex(text, "param-done", &g_dll.paramDone);
    }
}

static void dllRemoveAll(void);

// A list beginning: a placeholder for each of its url and pages items.
static void dlListBegin(void) {
    g_dll.dlThread = scePthreadGetthreadid();
    g_dll.endedUs = 0;
    dllRemoveAll();  // the last list's, shown finished till now
    if (sysExists(SC_BGM_DIR "/dl-nolist") || !dllSetup()) return;
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
    dllAnswers();
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
        g_dll.e[k].movedOut = 0;
        g_dll.e[k].shownIn = 0;
        g_dll.e[k].lastParam = 0;
        g_dll.e[k].oldTask = 0;
        g_dll.e[k].swapped = 0;
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
    else g_dll.endedUs = sysNowUs();
}

// While no list is going (dlMain's wait): the last list's placeholders go
// 30 minutes after it ended (1.15), unless dl-list-keep is there. Without
// the module (after a reboot) they would show the service's own state, a
// paused task, until the app's next start removed them.
static void ndbIdle(void);  // ndbprobe.c (1.17)

static void dlListIdle(void) {
    ndbIdle();
    dllLaterIdle();
    if (!g_dll.endedUs || !g_dll.count || sysNowUs() - g_dll.endedUs < 30ull * 60 * 1000000) return;
    g_dll.endedUs = 0;
    if (sysExists(SC_BGM_DIR "/dl-list-keep")) return;
    logLine("dllist: the last list ended 30 min ago: its entries removed");
    dllRemoveAll();
}

// Before each item (1.32): ours asked for, so one deleted from the list is
// seen (the progress hook's 0x80990019) even if the list hasn't asked for
// it since: 1.31's songs 4 and 5 went with song 3's delete and were fetched.
static void dlListCheck(void) {
    if (!g_bgft.progress) return;
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        const int32_t t = g_dll.e[k].task;
        if (t <= 0) continue;
        BgftProgress pr;
        memset(&pr, 0, sizeof(pr));
        g_bgft.progress(t, &pr);
    }
}

// An item finished (1.37): its row made again, with the app's title id.
// The list builds a row's look once, with the title id the task had then:
// 1.36's songs 4-6, finished in section 3 with rows made while they were
// coming (their own title id, no app), read nothing, though 4 and 5 were
// asked for again with the app's; songs 1-3, made again in section 9 with
// it, read "Installed.". A new task is in the list's next section read at
// once (every registration so far); the old one goes. Songs still coming
// keep their own (Cancel and Delete takes one only). Not with
// dl-list-own-title.
static void dlListDone(int i) {
    if (strlen(g_dls.app) != 9 || sysExists(SC_BGM_DIR "/dl-list-own-title")) return;
    // (1.42: not when the list can be told directly, DbNotify with the
    // subscriptions' own strings, unless dl-list-swap: its new row is made
    // in section 9 then, after it finished.)
    if (mcwDbNotifyKnown() && !sysExists(SC_BGM_DIR "/dl-list-swap")) return;
    for (int k = 0; k < g_dll.count && k < DLL_MAX; k++) {
        if (g_dll.e[k].item != i || g_dll.e[k].swapped || g_dll.e[k].task <= 0 || g_dll.e[k].movedOut) continue;
        g_dll.e[k].swapped = 1;
        const int32_t old = g_dll.e[k].task;
        // Its own title id still (1.38): with the app's the service said
        // 0x80990055 to every one (1.37's log). The list asks for a new
        // row's content id as it makes it, finished by then, and is
        // answered with the app's (dlListInfoHook, 1.36).
        const int32_t task = dllRegisterAs(i, NULL);
        if (task <= 0) return;
        g_dll.e[k].lastParam = 0;
        g_dll.e[k].logged = 0;
        g_dll.e[k].shownIn = 0;
        g_dll.e[k].oldTask = old;
        __atomic_store_n(&g_dll.e[k].task, task, __ATOMIC_RELEASE);
        logLine("dllist: task %d finished: made again as task %d", old, task);
        dllUnregister(old);
        dllSave();
        return;
    }
}

// The list's items have rows in Notifications > Downloads (1.31): song
// placeholders, or packages (the service's own rows).
static int dlListShown(void) {
    if (g_dll.count > 0) return 1;
    for (int i = 0; i < g_dls.count; i++)
        if (!memcmp(g_dls.item[i].kind, "pkg", 4)) return 1;
    return 0;
}

// A list starting: entries restored after a reboot go before its items
// are read in their place (1.29).
static void dlListForget(void) {
    if (!g_dll.restored) return;
    g_dll.restored = 0;
    dllRemoveAll();
    g_dls.count = 0;
}

// At the module's start: any placeholders left from an earlier boot (or a
// crash), removed. Only when there are some: nothing is hooked otherwise.
static void dlListLeftovers(void) {
    if (sysExists(DLL_TASKS)) dllSetup();
}
