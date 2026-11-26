/**
 * Research (1.17): how the Downloads list is told a task changed, inside
 * the player module (bgm.c includes this after dllist.c).
 *
 * The list (Sce.Vsh.Orbis.BgftAccessor's DownloadItemAccessor2) is a
 * NotificationDbAccessor: it asks the download service again when the
 * notification database (notification.db, table "item", item_type
 * DOWNLOAD/UPLOAD) tells it of a change. The shell writes that database
 * through libSceVshNotificationDbWrapper (sendJSON(json, blocking),
 * sendHintRecord). What a DOWNLOAD row holds, and the JSON sendJSON takes,
 * aren't in the managed code that was dumped. With ndb-probe in the
 * player's folder (checked every few seconds while no list is going):
 *
 *   - notification.db (and its -wal, -shm, -journal) copied to code/, from
 *     wherever it opens: the PS4's own DOWNLOAD rows, to read;
 *   - libSceVshNotificationDbWrapper loaded (else found), sendJSON and
 *     sendHintRecord looked up, its memory written to code/ndbw-<address>.bin
 *     (the JSON's keys are text in it);
 *   - sendJSON hooked: every JSON anything sends through it logged
 *     ("ndb: sendJSON").
 *
 * Nothing is written to the database. ndb-probe is deleted once done.
 */

typedef struct {
    void* start;
    void* end;
    int64_t offset;
    int32_t prot;
    int32_t mtype;
    uint8_t flags;
    char name[32];
} NdbVq;  // as mcw.c's VirtualQueryInfo
int sceKernelVirtualQuery(const void* addr, int flags, NdbVq* info, size_t size);

static Hook g_ndbHooks[2];

// After sendJSON(json, blocking) or sendHintRecord(record, blocking, action)
// (kind 22): what was sent.
void ndbSendHook(Frame* f) {
    static volatile uint32_t n;
    if (__atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) > 300) return;
    const char* s = (const char*)(uintptr_t)f->gpr[0];
    char text[400];
    text[0] = 0;
    if (hasText(f->hook->name, "JSON") && s) {
        size_t k = 0;
        while (k + 1 < sizeof(text) && hookReadable(s + k, 1) == 1 && s[k]) {
            text[k] = s[k];
            k++;
        }
        text[k] = 0;
    }
    logLine("ndb: %s(%lx, %lx, %lx) = 0x%08X %s  [thread %d]", f->hook->name, (unsigned long)f->gpr[0],
            (unsigned long)f->gpr[1], (unsigned long)f->gpr[2], (unsigned)f->retRax, text, scePthreadGetthreadid());
}

static void ndbCopy(const char* from, const char* to) {
    const int in = sysOpen(from, SC_O_RDONLY, 0);
    if (in < 0) {
        logLine("ndb: %s: not opened (0x%08X)", from, (unsigned)in);
        return;
    }
    const int out = sysOpen(to, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    static uint8_t buf[64 * 1024];
    long total = 0;
    int n;
    while (out >= 0 && (n = sysRead(in, buf, sizeof(buf))) > 0) {
        sysWrite(out, buf, (size_t)n);
        total += n;
    }
    if (out >= 0) sysClose(out);
    sysClose(in);
    logLine("ndb: %s: %ld bytes copied to %s", from, total, to);
}

static void ndbProbe(void) {
    logLine("ndb: probe (ndb-probe)");
    sysMkdir(SC_BGM_DIR "/code", 0777);
    // The database, from where it may be.
    static const char* const dirs[] = {"/system_data/priv/mms", "/system_data/common/mms", "/user/system_data/priv/mms"};
    static const char* const files[] = {"notification.db", "notification.db-wal", "notification.db-shm",
                                        "notification.db-journal"};
    for (int d = 0; d < 3; d++)
        for (int f = 0; f < 4; f++) {
            char from[128], to[160];
            fmt(from, sizeof(from), "%s/%s", dirs[d], files[f]);
            fmt(to, sizeof(to), SC_BGM_DIR "/code/%d-%s", d, files[f]);
            ndbCopy(from, to);
        }
    // The wrapper: loaded, its two functions, its memory.
    int h = libLoad("libSceVshNotificationDbWrapper.sprx");
    if (!h) h = libLoad("/system/vsh/sce_module/libSceVshNotificationDbWrapper.sprx");  // tried as given, second
    void* send = anySym(h, "sendJSON");
    void* hint = anySym(h, "sendHintRecord");
    logLine("ndb: libSceVshNotificationDbWrapper %d; sendJSON %p, sendHintRecord %p", h, send, hint);
    void* fns[2] = {send, hint};
    static const char* const names[2] = {"sendJSON", "sendHintRecord"};
    for (int i = 0; i < 2; i++) {
        if (!fns[i] || g_ndbHooks[i].installed) continue;
        g_ndbHooks[i].name = names[i];
        g_ndbHooks[i].json = 22;
        const char* why = "";
        const int rc = mcwHookNative(&g_ndbHooks[i], fns[i], &why);
        logLine("ndb: %s %s%s", names[i], rc == 0 ? "hooked (logged only)" : "not hooked: ", rc == 0 ? "" : why);
    }
    const uint8_t* at = (const uint8_t*)0x10000;
    NdbVq vq;
    int written = 0;
    for (int guard = 0; guard < 20000 && written < 8; guard++) {
        memset(&vq, 0, sizeof(vq));
        if (sceKernelVirtualQuery(at, 1, &vq, sizeof(vq)) != 0) break;
        if ((const uint8_t*)vq.end <= at) break;
        at = (const uint8_t*)vq.end;
        vq.name[31] = 0;
        if (!hasText(vq.name, "NotificationDb") || !(vq.prot & 1)) continue;
        const size_t len = (size_t)((const uint8_t*)vq.end - (const uint8_t*)vq.start);
        char to[160];
        fmt(to, sizeof(to), SC_BGM_DIR "/code/ndbw-%lx.bin", (unsigned long)(uintptr_t)vq.start);
        const int out = sysOpen(to, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
        if (out < 0) continue;
        size_t done = 0;
        while (done < len && hookReadable((const uint8_t*)vq.start + done, 0x4000) == 0x4000) {
            sysWrite(out, (const uint8_t*)vq.start + done, 0x4000);
            done += 0x4000;
        }
        sysClose(out);
        written++;
        logLine("ndb: region %p-%p prot 0x%X %s: %lu bytes to %s", vq.start, vq.end, (unsigned)vq.prot, vq.name,
                (unsigned long)done, to);
    }
    logLine("ndb: probe done, %d region(s) written; sendJSON calls are logged as they come", written);
}

// While no list is going (dlMain's wait): ndb-probe, once.
static void ndbIdle(void) {
    static uint64_t last;
    if (sysNowUs() - last < 3000000u) return;
    last = sysNowUs();
    if (!sysExists(SC_BGM_DIR "/ndb-probe")) return;
    sysUnlink(SC_BGM_DIR "/ndb-probe");
    ndbProbe();
}
