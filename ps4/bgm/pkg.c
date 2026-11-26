/**
 * Packages (.pkg) through the PS4's own download service, BGFT (0.89),
 * inside the player module (bgm.c includes this after plus.c):
 *
 *   - a download list's "pkg" items (downloads.c): a package by URL is
 *     registered with the service, which downloads and installs it as the
 *     Store's are, its entry and progress bar in Notifications > Downloads;
 *     a package on the console (/data/..., /mnt/usb...) is installed from
 *     where it is ("by storage");
 *   - self-update (update.txt, the "update_start" command, the client
 *     library's scUpdateInstallPkg): the app's own package installed once
 *     the app has gone, the result written to update-result.txt, the app
 *     started again.
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

// Its first 4 KB by HTTP (Range), with the list's headers and extra.
static int pkgHeadUrl(const char* url, const char* extra, PkgHead* out) {
    static uint8_t h[0x1000];
    if (!httpSetup()) return 0;
    const int tmpl = g_http.tmpl(g_http.ctx, "ShellCompanion/" SC_BGM_VERSION, 2, 1);
    if (tmpl < 0) return 0;
    const int conn = g_http.conn(tmpl, url, 1);
    const int req = conn >= 0 ? g_http.req(conn, 0, url, 0) : -1;
    size_t got = 0;
    if (req >= 0) {
        if (g_http.connectTimeout) g_http.connectTimeout(req, 15u * 1000000u);
        if (g_http.recvTimeout) g_http.recvTimeout(req, 30u * 1000000u);
        dlHeaders(req, g_dls.headers);
        dlHeaders(req, extra);
        if (g_http.addHeader) g_http.addHeader(req, "Range", "bytes=0-4095", 0);
        int code = 0;
        if (g_http.send(req, NULL, 0) >= 0 && (g_http.status(req, &code), code == 200 || code == 206)) {
            while (got < sizeof(h)) {
                const int n = g_http.read(req, h + got, sizeof(h) - got);
                if (n <= 0) break;
                got += (size_t)n;
            }
        }
        g_http.delReq(req);
    }
    if (conn >= 0) g_http.delConn(conn);
    g_http.delTmpl(tmpl);
    return got && pkgParse(h, got, out);
}

// The package (a URL, or a path on the console) registered with the
// service and started: its task id, or a negative code.
static int32_t pkgRegister(const char* where, const char* extra, const char* name) {
    if (!bgftSetup()) return -1;
    if (g_bgft.user == -1) bgftUser();
    const int local = where[0] == '/';
    char path[MAX_PATH];
    // The service's paths are the system's: /user/data is /data.
    fmt(path, sizeof(path), "%s", local && !memcmp(where, "/user/data/", 11) ? where + 5 : where);
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
    p->iconPath = "";
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

// Its progress until it ends (or stop says so): 0 done, 1 stopped, or the
// error. bytes/size kept as it goes.
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
            return 0;
        }
        for (int s = 0; s < 20 && !(stop && *stop); s++) sysSleepMs(100);
    }
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
    char title[16] = "", source[MAX_PATH] = "", v[8] = "";
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
    char where[MAX_PATH];
    fmt(where, sizeof(where), "%s%s", !memcmp(source, "/data/", 6) ? "/user" : "", source);
    const int32_t task = pkgRegister(where, NULL, title);
    if (task < 0) {
        char why[96];
        fmt(why, sizeof(why), "the PS4 would not take the package (0x%08X)", (unsigned)task);
        updateResult(task, why);
        notify("Update failed (0x%08X)", (unsigned)task);
        g_updateRunning = 0;
        return NULL;
    }
    uint64_t bytes = 0, size = 0;
    const int rc = pkgFollow(task, &bytes, &size, NULL);
    if (rc) {
        char why[96];
        fmt(why, sizeof(why), "the PS4's download or install failed (0x%08X)", (unsigned)rc);
        updateResult(rc, why);
        notify("Update failed (0x%08X)", (unsigned)rc);
        g_updateRunning = 0;
        return NULL;
    }
    // Installed once the copy is whole; a moment more for the install itself.
    sysSleepMs(15000);
    updateResult(0, "installed");
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

// A download list's "pkg" item: its URL (or path) to the service, followed
// until it is installed. GET_* as downloads.c's. Its notifications are the
// PS4's own (Notifications > Downloads), so none of ours for it.
static int pkgItem(int i) {
    __typeof__(g_dls.item[0])* it = &g_dls.item[i];
    it->state = DLS_GOING;
    dlStatus();
    const int32_t task = pkgRegister(it->arg[0], it->arg[1], it->title);
    if (task < 0) return task == -2 || task == -3 ? GET_GONE : GET_RETRY;
    it->task = task;
    dlStatus();
    const int rc = pkgFollow(task, &it->bytes, &it->size, &g_dls.takeBack);
    return rc == 0 ? GET_DONE : rc == 1 ? GET_STOPPED : GET_RETRY;
}

#ifdef SC_DEBUG  // the experiment: SC_DEBUG builds only (0.95)
// ── An experiment: media through the service (0.90) ─────────────────────────
//
// The service has task subtypes for photos, music and video besides
// packages. Whether it takes a plain MP3 (or MP4) by URL is not known: this
// registers one with each of a few package types, logs each answer, and
// for the first taken, starts it, follows it for up to 3 minutes and lists
// the folders it might have gone to. The "bgft_probe" command; its URL from
// bgft-probe.txt (its first line), else a test MP3.

static void probeList(const char* dir, int depth) {
    const int fd = sysOpen(dir, SC_O_RDONLY | SC_O_DIRECTORY, 0);
    if (fd < 0) {
        logLine("probe: %s: not opened (%d)", dir, fd);
        return;
    }
    static uint8_t buf[0x2000];
    int n, shown = 0;
    while ((n = sysGetdents(fd, buf, sizeof(buf))) > 0 && shown < 60) {
        for (int at = 0; at < n && shown < 60;) {
            const uint16_t reclen = le16(buf + at + 4);
            const uint8_t type = buf[at + 6], namlen = buf[at + 7];
            if (!reclen) break;
            char name[256];
            memcpy(name, buf + at + 8, namlen);
            name[namlen] = 0;
            at += reclen;
            if (name[0] == '.') continue;
            char path[512];
            fmt(path, sizeof(path), "%s/%s", dir, name);
            int64_t size = -1;
            if (type != 4) {
                const int f = sysOpen(path, SC_O_RDONLY, 0);
                if (f >= 0) {
                    size = sysSeek(f, 0, 2);
                    sysClose(f);
                }
            }
            logLine("probe:   %s%s %ld", path, type == 4 ? "/" : "", (long)size);
            shown++;
            if (type == 4 && depth > 0) probeList(path, depth - 1);
        }
    }
    sysClose(fd);
}

// A web server on the PS4 itself, for the experiment (0.94): one file,
// served at http://127.0.0.1:<port>/probe.mp3 to whoever asks (the download
// service, from another process), each request logged in full, so what the
// service asks for is seen without a PC. GET and HEAD, Range, then closed.
typedef int (*FnNetSocket)(const char*, int, int, int);
typedef int (*FnNetBind)(int, const void*, uint32_t);
typedef int (*FnNetListen)(int, int);
typedef int (*FnNetAccept)(int, void*, uint32_t*);
typedef int (*FnNetIo)(int, void*, size_t, int);
typedef int (*FnNetClose)(int);
static struct {
    FnNetSocket socket;
    FnNetBind bind;
    FnNetListen listen;
    FnNetAccept accept;
    FnNetIo recv, send;
    FnNetClose close;
    char file[MAX_PATH];
    int port, fd;
} g_srv;

static void srvSend(int c, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n) {
        const int k = g_srv.send(c, (void*)b, n > 65536 ? 65536 : n, 0);
        if (k <= 0) return;
        b += k;
        n -= (size_t)k;
    }
}

static void* srvMain(void* arg) {
    (void)arg;
    static char req[4096];
    static uint8_t buf[65536];
    for (;;) {
        uint8_t from[16];
        uint32_t fromLen = sizeof(from);
        const int c = g_srv.accept(g_srv.fd, from, &fromLen);
        if (c < 0) {
            logLine("probe server: accept 0x%08X, stopped", (unsigned)c);
            return NULL;
        }
        int got = 0;
        while (got < (int)sizeof(req) - 1) {
            const int k = g_srv.recv(c, req + got, sizeof(req) - 1 - (size_t)got, 0);
            if (k <= 0) break;
            got += k;
            req[got] = 0;
            if (hasText(req, "\r\n\r\n")) break;
        }
        req[got > 0 ? got : 0] = 0;
        // The request, its lines logged.
        for (char* p = req; *p;) {
            char* e = p;
            while (*e && *e != '\n') e++;
            const char keep = *e;
            *e = 0;
            if (e > p && e[-1] == '\r') e[-1] = 0;
            if (*p) logLine("probe server < %s", p);
            *e = keep;
            p = *e ? e + 1 : e;
        }
        const int head = !memcmp(req, "HEAD", 4);
        const int fd = sysOpen(g_srv.file, SC_O_RDONLY, 0);
        const int64_t size = fd >= 0 ? sysSeek(fd, 0, 2) : -1;
        char hdr[512];
        if (size < 0) {
            const int n = fmt(hdr, sizeof(hdr), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            srvSend(c, hdr, (size_t)n);
            logLine("probe server > 404");
        } else {
            // Range: bytes=a-b, bytes=a-
            uint64_t from0 = 0, to = (uint64_t)size - 1;
            int ranged = 0;
            for (const char* p = req; *p; p++)
                if (!memcmp(p, "\nRange: bytes=", 14) || !memcmp(p, "\nrange: bytes=", 14)) {
                    const char* q = p + 14;
                    from0 = 0;
                    while (*q >= '0' && *q <= '9') from0 = from0 * 10 + (uint64_t)(*q++ - '0');
                    if (*q == '-' && q[1] >= '0' && q[1] <= '9') {
                        to = 0;
                        for (q++; *q >= '0' && *q <= '9'; q++) to = to * 10 + (uint64_t)(*q - '0');
                    }
                    if (to >= (uint64_t)size) to = (uint64_t)size - 1;
                    ranged = 1;
                    break;
                }
            const uint64_t len = from0 <= to ? to - from0 + 1 : 0;
            const int n = ranged ? fmt(hdr, sizeof(hdr),
                                       "HTTP/1.1 206 Partial Content\r\nContent-Type: audio/mpeg\r\nContent-Length: %lu\r\n"
                                       "Content-Range: bytes %lu-%lu/%ld\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n",
                                       (unsigned long)len, (unsigned long)from0, (unsigned long)to, (long)size)
                                 : fmt(hdr, sizeof(hdr),
                                       "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: %ld\r\n"
                                       "Accept-Ranges: bytes\r\nConnection: close\r\n\r\n", (long)size);
            srvSend(c, hdr, (size_t)n);
            uint64_t sent = 0;
            if (!head && len) {
                sysSeek(fd, (int64_t)from0, 0);
                while (sent < len) {
                    const size_t want = len - sent > sizeof(buf) ? sizeof(buf) : (size_t)(len - sent);
                    const int k = sysRead(fd, buf, want);
                    if (k <= 0) break;
                    srvSend(c, buf, (size_t)k);
                    sent += (uint64_t)k;
                }
            }
            logLine("probe server > %s, %lu bytes sent", ranged ? "206" : "200", (unsigned long)sent);
        }
        if (fd >= 0) sysClose(fd);
        g_srv.close(c);
    }
}

// The server started on a port of 127.0.0.1, serving file: the port, or 0.
static int srvStart(const char* file) {
    if (g_srv.port) {
        fmt(g_srv.file, sizeof(g_srv.file), "%s", file);
        return g_srv.port;
    }
    const int net = libLoad("libSceNet.sprx");
    g_srv.socket = (FnNetSocket)anySym(net, "sceNetSocket");
    g_srv.bind = (FnNetBind)anySym(net, "sceNetBind");
    g_srv.listen = (FnNetListen)anySym(net, "sceNetListen");
    g_srv.accept = (FnNetAccept)anySym(net, "sceNetAccept");
    g_srv.recv = (FnNetIo)anySym(net, "sceNetRecv");
    g_srv.send = (FnNetIo)anySym(net, "sceNetSend");
    g_srv.close = (FnNetClose)anySym(net, "sceNetSocketClose");
    if (!g_srv.socket || !g_srv.bind || !g_srv.listen || !g_srv.accept || !g_srv.recv || !g_srv.send || !g_srv.close) {
        logLine("probe server: libSceNet's socket functions missing");
        return 0;
    }
    g_srv.fd = g_srv.socket("ScProbe", 2 /* AF_INET */, 1 /* STREAM */, 0);
    if (g_srv.fd < 0) {
        logLine("probe server: socket 0x%08X", (unsigned)g_srv.fd);
        return 0;
    }
    for (int port = 38521; port < 38531; port++) {
        uint8_t addr[16];
        memset(addr, 0, sizeof(addr));
        addr[0] = 16;  // sin_len
        addr[1] = 2;   // AF_INET
        addr[2] = (uint8_t)(port >> 8);
        addr[3] = (uint8_t)port;
        addr[4] = 127;
        addr[7] = 1;  // 127.0.0.1
        const int b = g_srv.bind(g_srv.fd, addr, sizeof(addr));
        if (b >= 0) {
            const int l = g_srv.listen(g_srv.fd, 8);
            logLine("probe server: 127.0.0.1:%d, listen 0x%08X", port, (unsigned)l);
            if (l < 0) return 0;
            g_srv.port = port;
            fmt(g_srv.file, sizeof(g_srv.file), "%s", file);
            if (threadStart(srvMain, NULL, "ScBgmProbeSrv") < 0) return 0;
            return port;
        }
    }
    logLine("probe server: no port could be bound");
    return 0;
}

static void* probeMain(void* arg) {
    (void)arg;
    static char url[MAX_PATH];
    char text[MAX_PATH];
    url[0] = 0;
    const int n = readText(SC_BGM_DIR "/bgft-probe.txt", text, sizeof(text));
    if (n > 0) {
        int k = 0;
        while (k < n && text[k] != '\n' && text[k] != '\r') k++;
        text[k] = 0;
        if (isUrl(text)) fmt(url, sizeof(url), "%s", text);
    }
    if (!url[0]) {
        // No URL given (0.94): a file of the console's, served from here,
        // every request the service makes logged ("probe server <").
        const char* file = SC_BGM_MUSIC "/yoursong.mp3";
        if (!sysExists(file))
            for (int i = 0; i < g_libCount && !sysExists(file); i++)
                if (!isUrl(g_tracks[i])) file = g_tracks[i];
        const int port = srvStart(file);
        if (port) fmt(url, sizeof(url), "http://127.0.0.1:%d/probe.mp3", port);
        else fmt(url, sizeof(url), "%s", "http://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3");
        logLine("probe: serving %s", file);
    }
    char shown[160];
    urlShown(url, shown, sizeof(shown));
    logLine("probe: media through the download service: %s", shown);
    if (!bgftSetup()) {
        logLine("probe: the service is not there");
        return NULL;
    }
    if (g_bgft.user == -1) bgftUser();
    // Its size, as a package installer gives a package's (0.93: 0.92
    // registered with none, and the PS4 said it was not on the server):
    // HEAD, its Content-Length.
    uint64_t size = 0;
    if (httpSetup()) {
        const int tmpl = g_http.tmpl(g_http.ctx, "ShellCompanion/" SC_BGM_VERSION, 2, 1);
        const int conn = tmpl >= 0 ? g_http.conn(tmpl, url, 1) : -1;
        const int req = conn >= 0 ? g_http.req(conn, 2 /* HEAD */, url, 0) : -1;
        int code = 0, known = -1;
        if (req >= 0 && g_http.send(req, NULL, 0) >= 0) {
            g_http.status(req, &code);
            g_http.length(req, &known, &size);
        }
        if (known != 0) size = 0;
        logLine("probe: HEAD: HTTP %d, %lu bytes", code, (unsigned long)size);
        if (req >= 0) g_http.delReq(req);
        if (conn >= 0) g_http.delConn(conn);
        if (tmpl >= 0) g_http.delTmpl(tmpl);
    }
    // A content id of its own each run (0.94): an earlier run's task is
    // otherwise taken again (0x80990086) and followed instead.
    static char probeId[40];
    fmt(probeId, sizeof(probeId), "IV0000-SCMP00001_00-PROBE%011lu", (unsigned long)(sysNowUs() / 1000u % 100000000000u));
    logLine("probe: content id %s", probeId);
    // Package types to try: none, empty, and guesses at a media one.
    // NULL last: the service may take a missing one worst.
    static const char* const types[] = {"", "PS4MUSIC", "MUSIC", "PS4AUDIO", "AUDIO", "PS4VIDEO", "VIDEO", "audio/mpeg", NULL};
    const int nTypes = (int)(sizeof(types) / sizeof(types[0]));
    int32_t task = -1;
    for (int t = 0; t < nTypes && task < 0; t++) {
        for (int ent = 0; ent < 2 && task < 0; ent++) {
            BgftParam p;
            memset(&p, 0, sizeof(p));
            p.userId = g_bgft.user;
            p.entitlementType = ent ? 0 : 5;
            p.id = probeId;
            p.contentUrl = url;
            p.contentName = "Shell Companion test";
            p.iconPath = "";
            p.playgoScenarioId = "0";
            p.option = 0x10000;  // DISABLE_CDN_QUERY_PARAM
            p.packageType = types[t];
            p.packageSubType = "";
            p.packageSize = size;
            int32_t id = -1;
            const int rc = g_bgft.reg(&p, &id);
            logLine("probe: type %s%s%s, entitlement %d: 0x%08X, task %d", types[t] ? "\"" : "",
                    types[t] ? types[t] : "NULL", types[t] ? "\"" : "", p.entitlementType, (unsigned)rc, id);
            if (rc >= 0 && id >= 0) task = id;
            if (rc == (int)0x80990086 && g_bgft.find) {
                // Registered by an earlier run: that task, followed.
                const int fr = g_bgft.find(p.id, 0, &id);
                logLine("probe: taken already, an earlier run's: 0x%08X, task %d", (unsigned)fr, id);
                if (fr >= 0 && id >= 0) task = id;
            }
        }
    }
    if (task < 0) {
        logLine("probe: no type was taken: media can't go through the service this way");
        notify("Download service test: not taken (see bgm.log)");
        return NULL;
    }
    const int st = g_bgft.start(task);
    logLine("probe: task %d started: 0x%08X", task, (unsigned)st);
    notify("Download service test: task %d taken, see Notifications > Downloads", task);
    // (0.91 asked sceBgftServiceIntDownloadGetTaskInfo here, with null
    // pointers for the title and icon: it writes into buffers it is given,
    // and the shell went down. Not asked now.)
    for (int s = 0; s < 90; s++) {
        BgftProgress pr;
        memset(&pr, 0, sizeof(pr));
        const int rc = g_bgft.progress(task, &pr);
        logLine("probe: progress 0x%08X: bits 0x%x, error 0x%08X, %lu of %lu (total %lu of %lu), %u s left",
                (unsigned)rc, pr.bits, (unsigned)pr.errorResult, (unsigned long)pr.transferred,
                (unsigned long)pr.length, (unsigned long)pr.transferredTotal, (unsigned long)pr.lengthTotal,
                pr.restSec);
        if (rc < 0 || pr.errorResult) break;
        if (pr.length && pr.transferred >= pr.length) break;
        sysSleepMs(2000);
    }
    // Where it may have gone.
    static const char* const dirs[] = {"/user/av_contents", "/user/download", "/user/data/download",
                                       "/user/av_contents/music", "/user/av_contents/video_temp",
                                       "/system_data/priv/bgft", "/user/system/download"};
    for (int d = 0; d < (int)(sizeof(dirs) / sizeof(dirs[0])); d++) probeList(dirs[d], 2);
    logLine("probe: done");
    return NULL;
}

static void probeStart(void) {
    static volatile int running;
    if (running) return;
    running = 1;
    if (threadStart(probeMain, NULL, "ScBgmProbe") < 0) running = 0;
}
#endif  // SC_DEBUG
