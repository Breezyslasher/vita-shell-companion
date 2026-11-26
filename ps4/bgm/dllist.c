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

static struct {
    int tried, hooked;
    Hook hooks[2];
    Hook query[4];  // the list's task queries (0.98)
    uint32_t queryCalls[4];
    FnBgftUnregister unregister, stop, pause;
    uint32_t bitsGoing, bitsWaiting, bitsDone;
    uint32_t paramGoing, paramDone;  // the task query's param answered for ours (1.01)
    int haveGoing, haveWaiting, haveDone;
    volatile int count;
    struct {
        volatile int32_t task;
        int item;
        uint64_t t0, b0;  // when it began to come, and its bytes then
        uint32_t logged;
        uint32_t lastWas, lastSaid;  // the service's bits and ours, last logged (0.99)
        uint64_t lastParam;          // its param in the last query logged (0.99)
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
    if (st == DLS_GOING && g_dll.haveGoing) pr->bits = g_dll.bitsGoing;
    else if (st == DLS_DONE && g_dll.haveDone) pr->bits = g_dll.bitsDone;
    else if (st != DLS_GOING && st != DLS_DONE && g_dll.haveWaiting) pr->bits = g_dll.bitsWaiting;
    pr->length = pr->lengthTotal = size;
    pr->transferred = pr->transferredTotal = st == DLS_DONE ? size : bytes;
    pr->numIndex = 1;
    pr->numTotal = 1;
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
        const int rc = (int)(int32_t)f->retRax;
        dllFill(k, pr);
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
    // Another task: a real download's answers, the first few, to learn from.
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
    logLine("dllist: %s(%d) = 0x%08X: bits 0x%x, error 0x%08X, %lu of %lu (total %lu of %lu), %u of %u, %u s left "
            "(total %u)  [call %u, thread %d]",
            f->hook->name, task, (unsigned)f->retRax, pr->bits, (unsigned)pr->errorResult,
            (unsigned long)pr->transferred, (unsigned long)pr->length, (unsigned long)pr->transferredTotal,
            (unsigned long)pr->lengthTotal, pr->numIndex, pr->numTotal, pr->restSec, pr->restSecTotal, n,
            scePthreadGetthreadid());
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
        if (cnt && cnt <= 960 && count && hookReadable(res, (size_t)cnt * 8) == (size_t)cnt * 8 &&
            (hookProt(res) & 2)) {
            for (uint32_t j = 0; j < cnt; j++) {
                int32_t id;
                memcpy(&id, res + j * 8, 4);
                for (int k = 0; k < count && k < DLL_MAX; k++) {
                    if (g_dll.e[k].task != id || id <= 0) continue;
                    const int i = g_dll.e[k].item;
                    const int done = i >= 0 && i < g_dls.count && g_dls.item[i].state == DLS_DONE;
                    const uint32_t prm = done ? g_dll.paramDone : g_dll.paramGoing;
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

// A list beginning: a placeholder for each of its url and pages items.
static void dlListBegin(void) {
    g_dll.count = 0;
    if (sysExists(SC_BGM_DIR "/dl-nolist") || !dllSetup()) return;
    char text[256];
    g_dll.haveGoing = g_dll.haveWaiting = g_dll.haveDone = 0;
    g_dll.haveGoing = 1;
    g_dll.bitsDone = 0x188;  // the PS4's own finished downloads (0.96's log)
    g_dll.haveDone = 1;
    g_dll.bitsGoing = 0x18a;  // a real install as it began (1.00's log: ...8a)
    g_dll.paramGoing = 0x10008261;
    g_dll.paramDone = 0x10008063;
    if (readText(SC_BGM_DIR "/dl-list-bits.txt", text, sizeof(text)) > 0) {
        if (dllHex(text, "going", &g_dll.bitsGoing)) g_dll.haveGoing = 1;
        g_dll.haveWaiting = dllHex(text, "waiting", &g_dll.bitsWaiting);
        g_dll.haveDone = dllHex(text, "done", &g_dll.bitsDone);
        dllHex(text, "param-going", &g_dll.paramGoing);
        dllHex(text, "param-done", &g_dll.paramDone);
    }
    if (g_bgft.user == -1) bgftUser();
    const uint64_t stamp = sysNowUs() / 1000u % 1000000000u;
    for (int i = 0; i < g_dls.count && g_dll.count < DLL_MAX; i++) {
        __typeof__(g_dls.item[0])* it = &g_dls.item[i];
        if (memcmp(it->kind, "url", 4) && memcmp(it->kind, "pages", 6)) continue;
        // A content id of its own: XXYYYY-TITLEID_00-<16 letters and digits>.
        char id[40];
        fmt(id, sizeof(id), "IV0000-SCMP00001_00-SCDL%03d%09lu", i % 1000, (unsigned long)stamp);
        BgftParam p;
        memset(&p, 0, sizeof(p));
        p.userId = g_bgft.user;
        p.entitlementType = 5;
        p.id = id;
        p.contentUrl = it->arg[0];
        p.contentName = it->title[0] ? it->title : it->id;
        p.iconPath = "";
        p.playgoScenarioId = "0";
        p.option = 0x10000;
        p.packageType = "";
        p.packageSubType = "";
        p.packageSize = it->size;
        int32_t task = -1;
        const int rc = g_bgft.reg(&p, &task);
        logLine("dllist: item %d (%s) registered: 0x%08X, task %d", i, p.contentName, (unsigned)rc, task);
        if (rc < 0 || task <= 0) continue;
        // Paused at once (0.98): the service marked 0.97's failed within a
        // second, never started.
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
        const int k = g_dll.count;
        g_dll.e[k].item = i;
        g_dll.e[k].t0 = g_dll.e[k].b0 = 0;
        g_dll.e[k].logged = 0;
        g_dll.e[k].task = task;
        __atomic_store_n(&g_dll.count, k + 1, __ATOMIC_RELEASE);
    }
    if (g_dll.count) dllSave();
}

// A list ended: its placeholders shown whole for 10 s (unless another list
// or the app comes first), then removed.
static void dlListEnd(void) {
    if (!g_dll.count) return;
    for (int s = 0; s < 100 && !g_dls.start && !g_dls.takeBack; s++) sysSleepMs(100);
    const int n = g_dll.count;
    __atomic_store_n(&g_dll.count, 0, __ATOMIC_RELEASE);
    sysSleepMs(50);  // a call answering one of them, done
    for (int k = 0; k < n; k++) dllUnregister(g_dll.e[k].task);
    sysUnlink(DLL_TASKS);
}
