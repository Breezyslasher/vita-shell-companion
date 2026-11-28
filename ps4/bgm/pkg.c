/**
 * Self-update through the PS4's own download service, BGFT (0.89), inside
 * the player module (bgm.c includes this after plus.c): update.txt, the
 * "update_start" command, the client library's scUpdateInstallPkg. The
 * app's own package (a URL, or a path on the console, installed from where
 * it is, "by storage") is installed once the app has gone, the result
 * written to update-result.txt, the app started again.
 *
 * As GoldHEN's Package Installer, flatz's remote package installer and
 * cy33hc's ps4-webdav-client register theirs: sceBgftServiceIntInit (a
 * 1 MB heap), sceBgftServiceIntDownloadRegisterTask(ByStorageEx), then
 * sceBgftServiceDownloadStartTask. A package's content id, type and size
 * come from its header (the first 4 KB).
 */

static void statusFieldBgm(const char* text, const char* key, char* out, size_t cap);

typedef struct {
    void* heap;
    size_t heapSize;
} BgftInit;

typedef struct {
    int32_t userId;
    int32_t entitlementType;
    const char* id;          // the content id
    const char* contentUrl;  // a URL, or a path ("by storage")
    const char* contentExUrl;
    const char* contentName;
    const char* iconPath;
    const char* skuId;
    uint32_t option;
    const char* playgoScenarioId;
    const char* releaseDate;
    const char* packageType;     // PS4GD, PS4AC, PS4AL, PS4DP
    const char* packageSubType;
    uint64_t packageSize;        // a ulong in the shell's own wrapper (OpenOrbis has 32 bits)
} BgftParam;

typedef struct {
    BgftParam params;
    uint32_t slot;
} BgftParamEx;

typedef struct {
    uint32_t bits;
    int32_t errorResult;
    uint64_t length, transferred, lengthTotal, transferredTotal;  // ulongs, as the shell's TaskProgress
    uint32_t numIndex, numTotal, restSec, restSecTotal;
    int32_t preparingPercent, localCopyPercent;
    uint8_t spare[0x80];  // room for a later, larger layout
} BgftProgress;

typedef int (*FnBgftInit)(BgftInit*);
typedef int (*FnBgftRegister)(BgftParam*, int32_t*);
typedef int (*FnBgftRegisterEx)(BgftParamEx*, int32_t*);
typedef int (*FnBgftId)(int32_t);
typedef int (*FnBgftProgress)(int32_t, BgftProgress*);
typedef int (*FnBgftFind)(const char*, int32_t, int32_t*);
typedef int (*FnUserInitial)(int32_t*);
typedef int (*FnAppInstInit)(void);

static struct {
    int tried, ok;
    FnBgftRegister reg;
    FnBgftRegisterEx regEx;
    FnBgftId start;
    FnBgftProgress progress;
    FnBgftFind find;
    int32_t user;
} g_bgft;

// The user the PS4's download service is asked for (0.91: the initial
// user's was -1 inside the shell, and every registration 0x80990004): the
// foreground user, the initial one, the first logged in, else the one the
// app wrote (the client library's scOpen, user.txt). Again while unknown.
static void bgftUser(void) {
    typedef int (*FnUserInitial)(int32_t*);
    FnUserInitial user = (FnUserInitial)anySym(0, "sceUserServiceGetInitialUser");
    g_bgft.user = -1;
    FnUserInitial fore = (FnUserInitial)anySym(0, "sceUserServiceGetForegroundUser");
    int rcFore = -1, rcInit = -1, rcList = -1;
    if (fore) rcFore = fore(&g_bgft.user);
    if (g_bgft.user == -1 && user) rcInit = user(&g_bgft.user);
    if (g_bgft.user == -1) {
        typedef int (*FnLoginList)(int32_t*);
        FnLoginList list = (FnLoginList)anySym(0, "sceUserServiceGetLoginUserIdList");
        int32_t ids[4] = {-1, -1, -1, -1};
        if (list) rcList = list(ids);
        for (int k = 0; k < 4 && g_bgft.user == -1; k++)
            if (ids[k] != -1) g_bgft.user = ids[k];
    }
    if (g_bgft.user == -1) {
        char text[64], v[24];
        if (readText(SC_BGM_DIR "/user.txt", text, sizeof(text)) > 0) {
            statusFieldBgm(text, "user", v, sizeof(v));
            int32_t u = 0;
            for (const char* q = v; *q >= '0' && *q <= '9'; q++) u = u * 10 + (*q - '0');
            if (v[0]) g_bgft.user = u;
        }
    }
    logLine("pkg: user 0x%X (foreground 0x%08X, initial 0x%08X, login list 0x%08X)", (unsigned)g_bgft.user,
            (unsigned)rcFore, (unsigned)rcInit, (unsigned)rcList);
}

// The service, once: 1 if it is there to use.
static int bgftSetup(void) {
    if (g_bgft.tried) return g_bgft.ok;
    g_bgft.tried = 1;
    const int lib = libLoad("libSceBgft.sprx"), inst = libLoad("libSceAppInstUtil.sprx");
    FnBgftInit init = (FnBgftInit)anySym(lib, "sceBgftServiceIntInit");
    g_bgft.reg = (FnBgftRegister)anySym(lib, "sceBgftServiceIntDownloadRegisterTask");
    g_bgft.regEx = (FnBgftRegisterEx)anySym(lib, "sceBgftServiceIntDownloadRegisterTaskByStorageEx");
    g_bgft.start = (FnBgftId)anySym(lib, "sceBgftServiceDownloadStartTask");
    g_bgft.progress = (FnBgftProgress)anySym(lib, "sceBgftServiceDownloadGetProgress");
    g_bgft.find = (FnBgftFind)anySym(lib, "sceBgftServiceDownloadFindTaskByContentId");
    FnAppInstInit instInit = (FnAppInstInit)anySym(inst, "sceAppInstUtilInitialize");
    logLine("pkg: libSceBgft %d, libSceAppInstUtil %d; init %p, register %p, by storage %p, start %p, progress %p",
            lib, inst, (void*)init, (void*)g_bgft.reg, (void*)g_bgft.regEx, (void*)g_bgft.start,
            (void*)g_bgft.progress);
    if (!init || !g_bgft.reg || !g_bgft.regEx || !g_bgft.start || !g_bgft.progress) return 0;
    if (instInit) logLine("pkg: sceAppInstUtilInitialize: 0x%08X", (unsigned)instInit());
    static BgftInit params;
    params.heapSize = 1u << 20;
    params.heap = sysMap(params.heapSize);
    const int rc = params.heap ? init(&params) : -1;
    // The shell has the service up already: an error here may say only that.
    logLine("pkg: sceBgftServiceIntInit: 0x%08X", (unsigned)rc);
    bgftUser();
    g_bgft.ok = 1;
    return 1;
}

typedef struct {
    char contentId[40];
    const char* type;  // PS4GD, PS4AC, PS4AL, PS4DP, or NULL if not one we take
    uint64_t size;
} PkgHead;

static uint32_t be32p(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// A package's header: 1 if it is one. Big-endian: magic "\x7FCNT" at 0,
// content id at 0x40 (36 characters), content type at 0x74, its size at
// 0x430.
static int pkgParse(const uint8_t* h, size_t n, PkgHead* out) {
    memset(out, 0, sizeof(*out));
    if (n < 0x438 || h[0] != 0x7F || h[1] != 'C' || h[2] != 'N' || h[3] != 'T') return 0;
    memcpy(out->contentId, h + 0x40, 36);
    out->contentId[36] = 0;
    const uint32_t type = be32p(h + 0x74);
    out->type = type == 0x1A ? "PS4GD" : type == 0x1B ? "PS4AC" : type == 0x1C ? "PS4AL" : type == 0x1E ? "PS4DP" : NULL;
    out->size = (uint64_t)be32p(h + 0x430) << 32 | be32p(h + 0x434);
    return 1;
}

static int pkgHeadLocal(const char* path, PkgHead* out) {
    static uint8_t h[0x1000];
    const int fd = sysOpen(path, SC_O_RDONLY, 0);
    if (fd < 0) return 0;
    const int n = sysRead(fd, h, sizeof(h));
    sysClose(fd);
    return n > 0 && pkgParse(h, (size_t)n, out);
}

// Bytes off..off+len-1 of a URL by HTTP (Range), with the list's headers
// and extra: how many came.
static size_t urlRange(const char* url, const char* extra, uint64_t off, size_t len, uint8_t* buf) {
    if (!httpSetup()) return 0;
    const int tmpl = g_http.tmpl(g_http.ctx, "ShellCompanion/" SC_BGM_VERSION, 2, 1);
    if (tmpl < 0) return 0;
    const int conn = g_http.conn(tmpl, url, 1);
    const int req = conn >= 0 ? g_http.req(conn, 0, url, 0) : -1;
    size_t got = 0;
    if (req >= 0) {
        if (g_http.connectTimeout) g_http.connectTimeout(req, 15u * 1000000u);
        if (g_http.recvTimeout) g_http.recvTimeout(req, 30u * 1000000u);
        (void)extra;
        char range[64];
        fmt(range, sizeof(range), "bytes=%lu-%lu", (unsigned long)off, (unsigned long)(off + len - 1));
        if (g_http.addHeader) g_http.addHeader(req, "Range", range, 0);
        int code = 0;
        // 200: the server ignored the range; only usable from the start.
        if (g_http.send(req, NULL, 0) >= 0 && (g_http.status(req, &code), code == 206 || (code == 200 && !off))) {
            while (got < len) {
                const int n = g_http.read(req, buf + got, len - got);
                if (n <= 0) break;
                got += (size_t)n;
            }
        }
        g_http.delReq(req);
    }
    if (conn >= 0) g_http.delConn(conn);
    g_http.delTmpl(tmpl);
    return got;
}

// Bytes of a package on the console, or by URL.
static size_t pkgRead(const char* where, const char* extra, uint64_t off, size_t len, uint8_t* buf) {
    if (where[0] != '/') return urlRange(where, extra, off, len, buf);
    const int fd = sysOpen(where, SC_O_RDONLY, 0);
    if (fd < 0) return 0;
    size_t got = 0;
    if (sysSeek(fd, (int64_t)off, 0) == (int64_t)off) {
        while (got < len) {
            const int n = sysRead(fd, buf + got, len - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
    }
    sysClose(fd);
    return got;
}

// Its first 4 KB by HTTP (Range), with the list's headers and extra.
static int pkgHeadUrl(const char* url, const char* extra, PkgHead* out) {
    static uint8_t h[0x1000];
    const size_t got = urlRange(url, extra, 0, sizeof(h), h);
    return got && pkgParse(h, got, out);
}

// The package's own icon (1.28): its ICON0_PNG entry (id 0x1200), kept in
// the clear in the entry table (entry count at 0x10, table offset at 0x18,
// 32-byte entries: id, name offset, flags, flags, offset, size), copied to
// SC_BGM_DIR/icons/<content id>.png for the service's iconPath. The
// Downloads list showed a placeholder icon for ours till installed (1.06).
// 1 if the file is there.
static int pkgIcon(const char* where, const char* extra, const char* contentId, char* out, size_t cap) {
    fmt(out, cap, "%s/icons/%s.png", SC_BGM_DIR, contentId);
    if (sysExists(out)) return 1;
    uint8_t h[0x20];
    if (pkgRead(where, extra, 0, sizeof(h), h) != sizeof(h)) return 0;
    const uint32_t count = be32p(h + 0x10), table = be32p(h + 0x18);
    if (!count || count > 4096 || !table) return 0;
    static uint8_t ents[4096 * 32];
    if (pkgRead(where, extra, table, (size_t)count * 32, ents) != (size_t)count * 32) return 0;
    uint32_t off = 0, size = 0;
    for (uint32_t j = 0; j < count; j++) {
        const uint8_t* e = ents + j * 32;
        if (be32p(e) != 0x1200) continue;
        if (be32p(e + 8) & 0x80000000u) break;  // encrypted
        off = be32p(e + 16);
        size = be32p(e + 20);
        break;
    }
    if (!off || !size || size > (4u << 20)) {
        logLine("pkg: %s: no icon in the clear", contentId);
        return 0;
    }
    const size_t mapped = ((size_t)size + 0x3FFF) & ~(size_t)0x3FFF;
    uint8_t* png = (uint8_t*)sysMap(mapped);
    if (!png) return 0;
    int ok = 0;
    if (pkgRead(where, extra, off, size, png) == size && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' &&
        png[3] == 'G') {
        char dir[MAX_PATH], tmp[MAX_PATH];
        fmt(dir, sizeof(dir), "%s/icons", SC_BGM_DIR);
        sysMkdir(dir, 0777);
        fmt(tmp, sizeof(tmp), "%s.part", out);
        const int fd = sysOpen(tmp, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
        if (fd >= 0) {
            ok = sysWrite(fd, png, size) == (int)size;
            sysClose(fd);
            ok = ok && sysRename(tmp, out) == 0;
            if (!ok) sysUnlink(tmp);
        }
    }
    sysUnmap(png, mapped);
    logLine("pkg: %s: icon, %u bytes at 0x%x: %s", contentId, (unsigned)size, (unsigned)off, ok ? out : "not kept");
    return ok;
}

// The package (a URL, or a path on the console) registered with the
// service and started: its task id, or a negative code.
// The form of a path on the console the service is given: 0 the system's
// (/data/...), 1 the shell's own (/user/data/...). As /data/..., 1.04's and
// 1.05's package was taken, then failed 2 s later with 0x80990004; as
// /user/data/... it installed (1.05). pkgItem tries /data/... if that fails.
static int g_pkgUserPaths = 1;  // 1.05: /data/... failed 0x80990004, /user/data/... installed

// A package's path or URL as long as the client library lets an app give
// one for an update (its update.txt is 1200 bytes): a signed or CDN URL is
// longer than MAX_PATH (1.51).
#define PKG_SOURCE_MAX 1280

// pkgFollow's answer when the service has moved no byte for 10 min (1.51:
// was 0, which self-update took as installed).
#define PKG_STALLED (-4)

static int32_t pkgRegister(const char* where, const char* extra, const char* name, const char* given) {
    if (!bgftSetup()) return -1;
    if (g_bgft.user == -1) bgftUser();
    const int local = where[0] == '/';
    char path[PKG_SOURCE_MAX];
    fmt(path, sizeof(path), "%s",
        local && !g_pkgUserPaths && !memcmp(where, "/user/data/", 11) ? where + 5 : where);
    if (local) logLine("pkg: given to the service as %s", path);
    PkgHead head;
    if (!(local ? pkgHeadLocal(where, &head) : pkgHeadUrl(where, extra, &head))) {
        logLine("pkg: %s: not a package (no CNT header)", local ? where : "a URL");
        return -2;
    }
    logLine("pkg: %s, type %s, %lu bytes", head.contentId, head.type ? head.type : "not one we take",
            (unsigned long)head.size);
    if (!head.type) return -3;
    BgftParamEx ex;
    memset(&ex, 0, sizeof(ex));
    BgftParam* p = &ex.params;
    p->userId = g_bgft.user;
    p->entitlementType = 5;
    p->id = head.contentId;
    p->contentUrl = path;
    p->contentName = name && name[0] ? name : head.contentId;
    static char icon[MAX_PATH];
    // The app's for it first (1.29), else the package's own (1.28).
    p->iconPath = given ? given : pkgIcon(where, extra, head.contentId, icon, sizeof(icon)) ? icon : "";
    p->playgoScenarioId = "0";
    p->packageType = head.type;
    p->packageSubType = "";
    p->packageSize = head.size;
    int32_t task = -1;
    int rc;
    if (local) {
        p->option = 0x8;  // FORCE_UPDATE: over what is installed
        ex.slot = 0;
        rc = g_bgft.regEx(&ex, &task);
    } else {
        p->option = 0x10000;  // DISABLE_CDN_QUERY_PARAM: the URL as it is
        rc = g_bgft.reg(p, &task);
    }
    logLine("pkg: registered: 0x%08X, task %d", (unsigned)rc, task);
    if (rc == (int)0x80990086 && g_bgft.find) {
        // Being downloaded already: that task.
        rc = g_bgft.find(head.contentId, 0, &task);
        logLine("pkg: already a task: 0x%08X, task %d", (unsigned)rc, task);
        return rc < 0 ? rc : task;
    }
    if (rc < 0) return rc;
    rc = g_bgft.start(task);
    logLine("pkg: started: 0x%08X", (unsigned)rc);
    return rc < 0 ? rc : task;
}

// Its progress until it ends (or stop says so): 0 every byte transferred,
// 1 stopped, PKG_STALLED after 10 min with no byte, or the service's error.
// bytes/size kept as it goes.
static int pkgFollow(int32_t task, uint64_t* bytes, uint64_t* size, volatile int* stop) {
    uint64_t lastChange = sysNowUs(), lastBytes = 0;
    for (;;) {
        if (stop && *stop) return 1;
        BgftProgress pr;
        memset(&pr, 0, sizeof(pr));
        const int rc = g_bgft.progress(task, &pr);
        if (rc < 0) {
            logLine("pkg: task %d progress: 0x%08X", task, (unsigned)rc);
            return rc;
        }
        if (pr.errorResult) {
            logLine("pkg: task %d: error 0x%08X", task, (unsigned)pr.errorResult);
            return pr.errorResult;
        }
        const uint64_t len = pr.lengthTotal ? pr.lengthTotal : pr.length;
        const uint64_t got = pr.lengthTotal ? pr.transferredTotal : pr.transferred;
        if (len) *size = len;
        *bytes = got;
        if (got != lastBytes) {
            lastBytes = got;
            lastChange = sysNowUs();
        }
        if (len && got >= len) return 0;
        if (sysNowUs() - lastChange > 600000000u) {  // 10 min without a byte
            logLine("pkg: task %d: no progress for 10 min, left to the PS4", task);
            return PKG_STALLED;
        }
        for (int s = 0; s < 20 && !(stop && *stop); s++) sysSleepMs(100);
    }
}

// After every byte has come, the install: the service's answers watched for
// up to secs s for an error (1.51: self-update waited a blind 15 s and
// called it installed). Which progress bits mean "installed" is not known
// for certain (README: finished tasks answered 0x188 and 0x213), so the
// bits are only logged as they change; an error, or the progress call
// failing, is what fails it. 0, or the error.
static int pkgSettle(int32_t task, int secs) {
    uint32_t lastBits = 0xFFFFFFFFu;
    const uint64_t until = sysNowUs() + (uint64_t)secs * 1000000u;
    while (sysNowUs() < until) {
        BgftProgress pr;
        memset(&pr, 0, sizeof(pr));
        const int rc = g_bgft.progress(task, &pr);
        if (rc < 0) {
            logLine("pkg: task %d installing: progress 0x%08X", task, (unsigned)rc);
            return rc;
        }
        if (pr.errorResult) {
            logLine("pkg: task %d installing: error 0x%08X", task, (unsigned)pr.errorResult);
            return pr.errorResult;
        }
        if (pr.bits != lastBits) {
            lastBits = pr.bits;
            logLine("pkg: task %d installing: bits 0x%x, preparing %d%%, local copy %d%%", task, pr.bits,
                    pr.preparingPercent, pr.localCopyPercent);
        }
        sysSleepMs(1000);
    }
    return 0;
}

// ── Self-update ──────────────────────────────────────────────────────────────
//
// update.txt (the client library's scUpdateInstallPkg):
//   title=<the app's title id>
//   source=<its package: a path, or a URL>
//   relaunch=<1: start the app again once installed>
// then "update_start". Written as it ends, update-result.txt:
//   result=<0, or the code>
//   why=<what happened>

#define UPDATE_FILE SC_BGM_DIR "/update.txt"
#define UPDATE_RESULT SC_BGM_DIR "/update-result.txt"

static volatile int g_updateRunning;

// key=value's value from a file's text, or "".
static void statusFieldBgm(const char* text, const char* key, char* out, size_t cap) {
    const size_t n = strlen(key);
    out[0] = 0;
    for (const char* p = text; *p;) {
        if (!memcmp(p, key, n) && p[n] == '=') {
            p += n + 1;
            size_t k = 0;
            while (*p && *p != '\n' && *p != '\r' && k + 1 < cap) out[k++] = *p++;
            out[k] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

static void updateResult(int rc, const char* why) {
    char text[512];
    fmt(text, sizeof(text), "result=%d\nwhy=%s\n", rc, why);
    writeText(UPDATE_RESULT, text);
    logLine("update: %d, %s", rc, why);
}

typedef struct {
    uint32_t size;
    int32_t userId, appOpt;
    uint64_t crashReport, checkFlag;
} LaunchParam;
typedef int (*FnLaunch)(const char*, const char**, LaunchParam*);

static void* updateMain(void* arg) {
    (void)arg;
    static char text[4096];
    static char source[PKG_SOURCE_MAX];
    char title[16] = "", v[8] = "";
    source[0] = 0;
    if (readText(UPDATE_FILE, text, sizeof(text)) > 0) {
        statusFieldBgm(text, "title", title, sizeof(title));
        statusFieldBgm(text, "source", source, sizeof(source));
        statusFieldBgm(text, "relaunch", v, sizeof(v));
    }
    if (!source[0]) {
        updateResult(-1, "update.txt names no package");
        g_updateRunning = 0;
        return NULL;
    }
    // The app goes as it asks: a moment for it to have gone.
    sysSleepMs(3000);
    notify("Updating %s", title[0] ? title : "the app");
    // The app's /data path is /user/data here.
    static char where[PKG_SOURCE_MAX + 8];
    fmt(where, sizeof(where), "%s%s", !memcmp(source, "/data/", 6) ? "/user" : "", source);
    const int32_t task = pkgRegister(where, NULL, title, NULL);
    if (task < 0) {
        char why[96];
        fmt(why, sizeof(why), "the PS4 would not take the package (0x%08X)", (unsigned)task);
        updateResult(task, why);
        notify("Update failed (0x%08X)", (unsigned)task);
        g_updateRunning = 0;
        return NULL;
    }
    uint64_t bytes = 0, size = 0;
    int rc = pkgFollow(task, &bytes, &size, NULL);
    if (rc == PKG_STALLED) {
        char why[128];
        fmt(why, sizeof(why), "the PS4's download stopped moving (%lu of %lu bytes after 10 min with none)",
            (unsigned long)bytes, (unsigned long)size);
        updateResult(rc, why);
        notify("Update failed: the download stopped");
        g_updateRunning = 0;
        return NULL;
    }
    // Every byte has come: the install itself, watched for an error.
    if (!rc) rc = pkgSettle(task, 30);
    if (rc) {
        char why[96];
        fmt(why, sizeof(why), "the PS4's download or install failed (0x%08X)", (unsigned)rc);
        updateResult(rc, why);
        notify("Update failed (0x%08X)", (unsigned)rc);
        g_updateRunning = 0;
        return NULL;
    }
    updateResult(0, "installed (the PS4 reported no error in 30 s after the download)");
    if (v[0] == '1' && title[0]) {
        FnLaunch launch = (FnLaunch)anySym(0, "sceSystemServiceLaunchApp");
        LaunchParam lp;
        memset(&lp, 0, sizeof(lp));
        lp.size = sizeof(lp);
        lp.userId = g_bgft.user;
        const int lr = launch ? launch(title, NULL, &lp) : -1;
        logLine("update: %s started again: 0x%08X", title, (unsigned)lr);
    } else {
        notify("Update installed: %s", title[0] ? title : "the app");
    }
    g_updateRunning = 0;
    return NULL;
}

static void updateStart(void) {
    if (g_updateRunning) return;
    g_updateRunning = 1;
    if (threadStart(updateMain, NULL, "ScBgmUpdate") < 0) {
        g_updateRunning = 0;
        updateResult(-1, "no thread");
    }
}


