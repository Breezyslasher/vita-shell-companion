/**
 * Background downloads (0.82), inside the player module (bgm.c includes this
 * after its HTTP code): an app's list of files, fetched in the shell after
 * the app has gone, as the Vita plugin's (docs/PROTOCOL.md §5). Its files:
 *
 *   downloads.txt   the list, written by the app (the client library's
 *                   scWriteDownloads), its fields %-encoded where they may
 *                   hold a tab, a line break or a %:
 *                     notify=<1: notifications>
 *                     headers=<header lines, \r\n between, sent with every request>
 *                     item=<id>\t<file>\t<size>\t<title>\t<kind>\t<arg>\t<arg>\t<arg>
 *                     end
 *   cmd             dl_start (begin, or begin again from the list), dl_takeback
 *   downloads-status.txt  every 2 s and at each change:
 *                     running=0|1
 *                     paused=0
 *                     item=<id>\t<going|waiting|done|gone|failed>\t<bytes>\t<size>\t\t[\t<done>/<pages>]
 *                     end
 *
 * Kinds: url (arg 1 the URL, arg 2 extra header lines) and pages (arg 1 a URL
 * template with {page} or {page:N}, arg 2 "count" or "count,first", arg 3
 * extra header lines; the file a template too, each page its own file).
 * A partial file is continued with Range; 200 to a Range rewrites it; 416 is
 * complete; 503 is waited for (every 10 s, up to an hour); 404 and 410 are
 * gone; anything else is tried again after 5, 15 and 60 s, any bytes come
 * resetting the count.
 */

#define DL_LIST SC_BGM_DIR "/downloads.txt"
#define DL_STATUS SC_BGM_DIR "/downloads-status.txt"
#define DL_MAX 200
#define DL_TEXT 1024

enum { DLS_WAITING, DLS_GOING, DLS_DONE, DLS_GONE, DLS_FAILED };
static const char* const g_dlStateName[] = {"waiting", "going", "done", "gone", "failed"};

static struct {
    volatile int start, takeBack;  // asked for (runCommand), seen by dlMain
    volatile int running;
    int notify;
    char headers[2048];
    int count;
    struct {
        char id[64], file[DL_TEXT], title[128], kind[16];
        char arg[3][DL_TEXT];
        uint64_t size, bytes;
        int state;
        int task;  // a package's task with the PS4's download service (0.89), 0 if none
        int pagesDone, pages;
    } item[DL_MAX];
    uint64_t lastStatus;
    int quarter;          // the last progress notification's (1-3), 0 none yet
    uint64_t lastNotice;  // when it was shown
    int current;          // the item being fetched
} g_dls;

static uint8_t g_dlBuf[64 * 1024];

static void dllRepair(void);  // dllist.c (1.02)
static void dlListLeftovers(void);  // dllist.c (1.04)

// %XX back to its byte.
static void dlDecode(char* s) {
    char* o = s;
    for (const char* p = s; *p; p++) {
        if (p[0] == '%' && p[1] && p[2]) {
            int v = 0, ok = 1;
            for (int k = 1; k <= 2; k++) {
                const char c = p[k];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= c - '0';
                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                else ok = 0;
            }
            if (ok) {
                *o++ = (char)v;
                p += 2;
                continue;
            }
        }
        *o++ = *p;
    }
    *o = 0;
}

static uint64_t dlNum(const char* s) {
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

// The list, read: its items waiting. 1 if there was one.
static int dlRead(void) {
    static char text[DL_MAX * 3 * 1024];
    const int n = readText(DL_LIST, text, sizeof(text));
    g_dls.count = 0;
    g_dls.notify = 1;
    g_dls.headers[0] = 0;
    if (n <= 0) return 0;
    for (int at = 0; at < n;) {
        int end = at;
        while (end < n && text[end] != '\n' && text[end] != '\r') end++;
        text[end] = 0;
        char* line = text + at;
        if (!memcmp(line, "notify=", 7)) {
            g_dls.notify = line[7] != '0';
        } else if (!memcmp(line, "headers=", 8)) {
            fmt(g_dls.headers, sizeof(g_dls.headers), "%s", line + 8);
            dlDecode(g_dls.headers);
        } else if (!memcmp(line, "item=", 5) && g_dls.count < DL_MAX) {
            // Tab-separated fields, each decoded.
            char* f[8] = {0};
            int k = 0;
            char* p = line + 5;
            f[k++] = p;
            for (; *p && k < 8; p++)
                if (*p == '\t') {
                    *p = 0;
                    f[k++] = p + 1;
                }
            for (int i = 0; i < 8; i++)
                if (f[i]) dlDecode(f[i]);
            if (k >= 6 && f[1][0] && f[5][0]) {
                const int i = g_dls.count++;
                memset(&g_dls.item[i], 0, sizeof(g_dls.item[i]));
                fmt(g_dls.item[i].id, sizeof(g_dls.item[i].id), "%s", f[0]);
                // The shell's path: /data is /user/data inside it.
                fmt(g_dls.item[i].file, DL_TEXT, "%s%s", !memcmp(f[1], "/data/", 6) ? "/user" : "", f[1]);
                g_dls.item[i].size = dlNum(f[2]);
                fmt(g_dls.item[i].title, sizeof(g_dls.item[i].title), "%s", f[3]);
                fmt(g_dls.item[i].kind, sizeof(g_dls.item[i].kind), "%s", f[4]);
                for (int a = 0; a < 3; a++)
                    if (5 + a < k) fmt(g_dls.item[i].arg[a], DL_TEXT, "%s", f[5 + a]);
            }
        }
        at = end + 1;
    }
    return g_dls.count > 0;
}

static void dlNotify(const char* f, ...);

// The whole list's progress, 0-1000: by bytes where every size is known,
// else by items (the item being fetched by its own bytes if its size is).
static int dlPermille(void) {
    uint64_t have = 0, total = 0;
    int sized = 1;
    for (int i = 0; i < g_dls.count; i++) {
        if (!g_dls.item[i].size && g_dls.item[i].state != DLS_DONE) sized = 0;
        have += g_dls.item[i].bytes;
        total += g_dls.item[i].size ? g_dls.item[i].size : g_dls.item[i].bytes;
    }
    if (sized && total) return (int)(have * 1000u / total);
    if (!g_dls.count) return 0;
    int done = 0;
    for (int i = 0; i < g_dls.count; i++) done += g_dls.item[i].state == DLS_DONE || g_dls.item[i].state == DLS_GONE;
    int part = 0;
    const int c = g_dls.current;
    if (c >= 0 && c < g_dls.count && g_dls.item[c].size && g_dls.item[c].state == DLS_GOING)
        part = (int)(g_dls.item[c].bytes * 1000u / g_dls.item[c].size);
    return (done * 1000 + part) / g_dls.count;
}

// A notification at each quarter of the whole (25, 50, 75%), at most one
// every 20 s (0.85: the PS4 shows notifications, not the Vita's one that
// updates).
static void dlProgress(void) {
    const int pm = dlPermille();
    const int q = pm / 250;
    if (q <= g_dls.quarter || q >= 4 || sysNowUs() - g_dls.lastNotice < 20000000u) return;
    g_dls.quarter = q;
    g_dls.lastNotice = sysNowUs();
    const int c = g_dls.current;
    if (g_dls.count > 1 && c >= 0 && c < g_dls.count)
        dlNotify("Downloading: %d%% (%d of %d)\n%s", pm / 10, c + 1, g_dls.count, g_dls.item[c].title);
    else if (c >= 0 && c < g_dls.count)
        dlNotify("Downloading: %d%%\n%s", pm / 10, g_dls.item[c].title);
}

static void dlStatus(void) {
    static char text[DL_MAX * 128 + 64];
    size_t at = (size_t)fmt(text, sizeof(text), "running=%d\npaused=0\n", g_dls.running);
    for (int i = 0; i < g_dls.count && at + 128 < sizeof(text); i++) {
        at += (size_t)fmt(text + at, sizeof(text) - at, "item=%s\t%s\t%lu\t%lu\t", g_dls.item[i].id,
                          g_dlStateName[g_dls.item[i].state], (unsigned long)g_dls.item[i].bytes,
                          (unsigned long)g_dls.item[i].size);
        if (g_dls.item[i].task > 0) at += (size_t)fmt(text + at, sizeof(text) - at, "%d", g_dls.item[i].task);
        text[at++] = '\t';
        if (g_dls.item[i].pages)
            at += (size_t)fmt(text + at, sizeof(text) - at, "\t%d/%d", g_dls.item[i].pagesDone,
                              g_dls.item[i].pages);
        text[at++] = '\n';
    }
    at += (size_t)fmt(text + at, sizeof(text) - at, "end\n");
    writeText(DL_STATUS, text);
    g_dls.lastStatus = sysNowUs();
    dllRepair();  // placeholders the service failed, replaced (dllist.c, 1.02)
}

// Header lines ("Name: value", \r\n or \n between) added to the request.
static void dlHeaders(int req, const char* lines) {
    if (!g_http.addHeader || !lines) return;
    char name[128], value[1024];
    for (const char* p = lines; *p;) {
        const char* e = p;
        while (*e && *e != '\r' && *e != '\n') e++;
        const char* colon = p;
        while (colon < e && *colon != ':') colon++;
        if (colon < e && colon > p && (size_t)(colon - p) < sizeof(name)) {
            memcpy(name, p, (size_t)(colon - p));
            name[colon - p] = 0;
            const char* v = colon + 1;
            while (v < e && *v == ' ') v++;
            size_t vn = (size_t)(e - v);
            if (vn >= sizeof(value)) vn = sizeof(value) - 1;
            memcpy(value, v, vn);
            value[vn] = 0;
            g_http.addHeader(req, name, value, 0 /* overwrite */);
        }
        p = e;
        while (*p == '\r' || *p == '\n') p++;
    }
}

static int64_t fileSize(const char* path) {
    const int fd = sysOpen(path, SC_O_RDONLY, 0);
    if (fd < 0) return -1;
    const int64_t n = sysSeek(fd, 0, 2);
    sysClose(fd);
    return n;
}

// The folder a file goes in, its last level made.
static void dlFolder(const char* file) {
    char dir[DL_TEXT];
    fmt(dir, sizeof(dir), "%s", file);
    char* slash = NULL;
    for (char* p = dir; *p; p++)
        if (*p == '/') slash = p;
    if (slash && slash != dir) {
        *slash = 0;
        sysMkdir(dir, 0777);
    }
}

enum { GET_DONE, GET_GONE, GET_RETRY, GET_WAIT, GET_STOPPED };

// One URL into file, continued from its size: GET_*. *got counts the bytes
// that came (for the retries' count); progress the item's, for the status.
static int dlGet(const char* url, const char* file, const char* extra, uint64_t* got, uint64_t* progress,
                 uint64_t* size) {
    char shown[160];
    urlShown(url, shown, sizeof(shown));
    *got = 0;
    if (!httpSetup()) return GET_RETRY;
    int64_t have = fileSize(file);
    if (have < 0) have = 0;
    if (*size && (uint64_t)have >= *size) return GET_DONE;
    int result = GET_RETRY, fd = -1;
    const int tmpl = g_http.tmpl(g_http.ctx, "ShellCompanion/" SC_BGM_VERSION, 2, 1);
    if (tmpl < 0) return GET_RETRY;
    if (g_http.httpsOff && sysExists(SC_BGM_DIR "/https-noverify")) g_http.httpsOff(tmpl, 0x01 | 0x04 | 0x08 | 0x10 | 0x20);
    const int conn = g_http.conn(tmpl, url, 1);
    const int req = conn >= 0 ? g_http.req(conn, 0, url, 0) : -1;
    if (req < 0) goto out;
    if (g_http.connectTimeout) g_http.connectTimeout(req, 15u * 1000000u);
    if (g_http.recvTimeout) g_http.recvTimeout(req, 30u * 1000000u);
    dlHeaders(req, g_dls.headers);
    dlHeaders(req, extra);
    if (have > 0 && g_http.addHeader) {
        char range[48];
        fmt(range, sizeof(range), "bytes=%lu-", (unsigned long)have);
        g_http.addHeader(req, "Range", range, 0);
    }
    const int rc = g_http.send(req, NULL, 0);
    if (rc < 0) {
        logLine("download %s: not sent: 0x%08X", shown, (unsigned)rc);
        goto out;
    }
    int code = 0, known = -1;
    uint64_t len = 0;
    g_http.status(req, &code);
    g_http.length(req, &known, &len);
    logLine("download %s: HTTP %d, %s %lu, from %ld", shown, code, known == 0 ? "length" : "no length",
            (unsigned long)len, (long)have);
    if (code == 416) {
        result = GET_DONE;
        goto out;
    }
    if (code == 404 || code == 410) {
        result = GET_GONE;
        goto out;
    }
    if (code == 503) {
        result = GET_WAIT;
        goto out;
    }
    if (code != 200 && code != 206) goto out;
    if (code == 200) have = 0;  // the whole file again: written from its start
    if (known == 0 && len) *size = (uint64_t)have + len;
    dlFolder(file);
    fd = sysOpen(file, SC_O_WRONLY | SC_O_CREAT | (have ? 0 : SC_O_TRUNC), 0666);
    if (fd < 0) {
        logLine("download %s: %s not opened (%d)", shown, file, fd);
        goto out;
    }
    if (have) sysSeek(fd, have, 0);
    *progress = (uint64_t)have;
    for (;;) {
        if (g_dls.takeBack) {
            result = GET_STOPPED;
            goto out;
        }
        const int n = g_http.read(req, g_dlBuf, sizeof(g_dlBuf));
        if (n < 0) {
            logLine("download %s: read failed at %lu: 0x%08X", shown, (unsigned long)*progress, (unsigned)n);
            goto out;
        }
        if (n == 0) break;
        if (sysWrite(fd, g_dlBuf, (size_t)n) != n) {
            logLine("download %s: writing %s failed", shown, file);
            goto out;
        }
        *got += (uint64_t)n;
        *progress += (uint64_t)n;
        if (sysNowUs() - g_dls.lastStatus > 2000000u) {
            dlStatus();
            dlProgress();
        }
    }
    // Ended: whole if its length was known and all came, or not known.
    result = !*size || *progress >= *size ? GET_DONE : GET_RETRY;
out:
    if (fd >= 0) sysClose(fd);
    if (req >= 0) g_http.delReq(req);
    if (conn >= 0) g_http.delConn(conn);
    g_http.delTmpl(tmpl);
    return result;
}

// One URL, waited for and tried again as the protocol says: GET_DONE,
// GET_GONE, GET_STOPPED, or GET_RETRY when its tries are spent.
static int dlFetch(const char* url, const char* file, const char* extra, uint64_t* progress, uint64_t* size,
                   int* state) {
    static const uint32_t backoff[3] = {5, 15, 60};
    int tries = 0;
    uint32_t waited = 0;
    for (;;) {
        uint64_t got = 0;
        *state = DLS_GOING;
        const int r = dlGet(url, file, extra, &got, progress, size);
        if (r == GET_DONE || r == GET_GONE || r == GET_STOPPED) return r;
        if (got) tries = 0;
        uint32_t wait;
        if (r == GET_WAIT) {
            if (waited >= 3600) return GET_RETRY;
            wait = 10;
            waited += 10;
        } else {
            if (tries >= 3) return GET_RETRY;
            wait = backoff[tries++];
        }
        *state = DLS_WAITING;
        dlStatus();
        for (uint32_t s = 0; s < wait * 10; s++) {
            if (g_dls.takeBack) return GET_STOPPED;
            sysSleepMs(100);
        }
    }
}

// A pages template's {page} or {page:N} filled in with page.
static int pageFill(const char* tmpl, int page, char* out, size_t cap) {
    size_t o = 0;
    int found = 0;
    for (const char* p = tmpl; *p && o + 12 < cap;) {
        if (!memcmp(p, "{page}", 6)) {
            o += (size_t)fmt(out + o, cap - o, "%d", page);
            p += 6;
            found = 1;
        } else if (!memcmp(p, "{page:", 6) && p[6] >= '1' && p[6] <= '9' && p[7] == '}') {
            char f[8] = {'%', '0', p[6], 'd', 0};
            o += (size_t)fmt(out + o, cap - o, f, page);
            p += 8;
            found = 1;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return found;
}

static void dlNotify(const char* f, ...) {
    if (!g_dls.notify) return;
    char text[256];
    va_list ap;
    va_start(ap, f);
    fmtv(text, sizeof(text), f, ap);
    va_end(ap);
    notify("%s", text);
}

static int pkgItem(int i);  // pkg.c (0.89)
static void dlListBegin(void);  // dllist.c (0.96)
static void dlListEnd(void);

static void dlItem(int i) {
    __typeof__(g_dls.item[0])* it = &g_dls.item[i];
    if (it->state == DLS_DONE || it->state == DLS_GONE) return;
    int r = GET_RETRY;
    if (!memcmp(it->kind, "url", 4)) {
        r = dlFetch(it->arg[0], it->file, it->arg[1], &it->bytes, &it->size, &it->state);
    } else if (!memcmp(it->kind, "pages", 6)) {
        int count = 0, first = 0;
        const char* p = it->arg[1];
        while (*p >= '0' && *p <= '9') count = count * 10 + (*p++ - '0');
        if (*p == ',')
            for (p++; *p >= '0' && *p <= '9'; p++) first = first * 10 + (*p - '0');
        if (count > 1000) count = 1000;
        it->pages = count;
        it->pagesDone = 0;
        dlFolder(it->file);
        r = GET_DONE;
        for (int k = 0; k < count && r == GET_DONE; k++) {
            char url[DL_TEXT], file[DL_TEXT], part[DL_TEXT + 8];
            if (!pageFill(it->arg[0], first + k, url, sizeof(url)) ||
                !pageFill(it->file, first + k, file, sizeof(file))) {
                logLine("download %s: its templates have no {page}", it->id);
                r = GET_GONE;
                break;
            }
            if (sysExists(file)) {  // a page whole already
                it->pagesDone++;
                continue;
            }
            fmt(part, sizeof(part), "%s.part", file);
            uint64_t pageSize = 0, pageBytes = 0;
            r = dlFetch(url, part, it->arg[2], &pageBytes, &pageSize, &it->state);
            if (r == GET_DONE) {
                sysRename(part, file);
                it->pagesDone++;
                it->bytes += pageBytes;
            }
        }
    } else if (!memcmp(it->kind, "pkg", 4)) {
        r = pkgItem(i);
    } else {
        logLine("download %s: kind \"%s\" not known here", it->id, it->kind);
        r = GET_GONE;
    }
    it->state = r == GET_DONE ? DLS_DONE : r == GET_GONE ? DLS_GONE : r == GET_STOPPED ? DLS_WAITING : DLS_FAILED;
    logLine("download %s (%s): %s, %lu bytes", it->id, it->title, g_dlStateName[it->state], (unsigned long)it->bytes);
}

static void* dlMain(void* arg) {
    (void)arg;
    dlListLeftovers();
    for (;;) {
        while (!g_dls.start) sysSleepMs(200);
        g_dls.start = 0;
        g_dls.takeBack = 0;
        if (!dlRead()) {
            logLine("downloads: no list (" DL_LIST ")");
            continue;
        }
        g_dls.running = 1;
        g_dls.quarter = 0;
        g_dls.lastNotice = sysNowUs();
        g_dls.current = 0;
        dlStatus();
        // The app goes as it hands them over: a moment for it to close its files.
        sysSleepMs(2000);
        dlListBegin();  // its files in Notifications > Downloads
        logLine("downloads: %d item(s)", g_dls.count);
        dlNotify("Downloading %d item%s in the background", g_dls.count, g_dls.count == 1 ? "" : "s");
        int done = 0, failed = 0;
        for (int i = 0; i < g_dls.count && !g_dls.takeBack && !g_dls.start; i++) {
            g_dls.current = i;
            dlItem(i);
            dlStatus();
            dlProgress();
            if (g_dls.item[i].state == DLS_DONE) {
                done++;
                if (g_dls.count > 1) dlNotify("Downloaded: %s (%d of %d)", g_dls.item[i].title, i + 1, g_dls.count);
            } else if (g_dls.item[i].state != DLS_WAITING) {
                failed++;
            }
        }
        g_dls.running = 0;
        dlStatus();
        if (g_dls.takeBack) {
            logLine("downloads: taken back by the app");
        } else if (!g_dls.start) {
            logLine("downloads: done, %d of %d (%d not)", done, g_dls.count, failed);
            if (failed) dlNotify("Downloads: %d done, %d failed", done, failed);
            else dlNotify("Downloads done (%d)", done);
        }
        dlListEnd();
    }
    return NULL;
}
