/**
 * The Music section's "+" (R3, "Add to Your Music") as each app wants it
 * (0.86), inside the player module (bgm.c includes this).
 *
 * The music watch answers the shell's library requests from liked.txt (a
 * path or URL a line) and tells the player of each press (plus_save=<track>,
 * plus_delete=<track>, the library's number). Here:
 *
 *   - every press is added to plus-events.txt, for the app to read when it
 *     comes back (the client library's scReadPlusEvents):
 *       <ms>\t<save|delete>\t<the app's id for the track>\t<path or URL>
 *   - with "#plus.mode http" in queue.txt (the client library's ScPlus), the
 *     app's own server is told: #plus.save / #plus.delete, URL templates
 *     sent with #plus.method (GET by default, or POST, PUT, DELETE, PATCH),
 *     #plus.body and #plus.type, and #plus.headers ("Name: value" lines,
 *     %-encoded). Placeholders: {id}, {title}, {artist}, {album}, {file}
 *     (URL-encoded in a URL, and in a body whose #plus.type is
 *     application/x-www-form-urlencoded; escaped as in a JSON string in any
 *     other body).
 *   - #plus.contains, a URL template asked when a track begins: a 2xx answer
 *     (whose body holds #plus.match, if given) makes the track saved, any
 *     other answer not, liked.txt kept so, which is what lights the "+".
 *
 * The requests run one at a time on a thread of their own (ScBgmPlus),
 * never holding up the music. Each job is filled in when it is queued, on
 * the player's thread, and carries its own copies of everything it needs
 * (1.51): a reload (poolReset, plusReset) while a request is going frees
 * the templates and track strings the thread used to read.
 */

// A job's room (1.51: 2048 for a URL or body, cut silently, overflowed by a
// %-encoded {file} URL).
#define PLUS_URL 4096
#define PLUS_BODY 4096
#define PLUS_HEADERS 4096
#define PLUS_MATCH 512

static struct {
    int http;  // #plus.mode http
    const char *save, *del, *contains, *match, *method, *body, *type, *headers;
} g_plus;

static void plusReset(void) {
    g_plus.http = 0;
    g_plus.save = g_plus.del = g_plus.contains = g_plus.match = "";
    g_plus.method = g_plus.body = g_plus.type = g_plus.headers = "";
}

// "#plus.<key> <value>" from queue.txt: 1 if it was one. The value, kept in
// g_pool, %-decoded.
static int plusLine(const char* line, int len) {
    if (len < 7 || memcmp(line, "#plus.", 6)) return 0;
    int k = 6;
    while (k < len && line[k] != ' ') k++;
    const char* key = line + 6;
    const int keyLen = k - 6;
    static char value[PLUS_URL];  // the player's thread only
    int vn = len - k - 1;
    if (vn < 0) vn = 0;
    if (vn >= (int)sizeof(value)) {
        // Cut, a template would send to the wrong place (1.51: cut silently).
        char keyText[24];
        int i = 0;
        for (; i < keyLen && i + 1 < (int)sizeof(keyText); i++) keyText[i] = key[i];
        keyText[i] = 0;
        logLine("plus: #plus.%s: %d bytes, more than %d: not used", keyText, vn, (int)sizeof(value) - 1);
        return 1;
    }
    memcpy(value, line + k + 1, (size_t)vn);
    value[vn] = 0;
    textDecode(value);
#define KEY(name) (keyLen == (int)sizeof(name) - 1 && !memcmp(key, name, sizeof(name) - 1))
    const char* kept = poolCopy(value);
    if (KEY("mode")) g_plus.http = !memcmp(value, "http", 5);
    else if (KEY("save")) g_plus.save = kept;
    else if (KEY("delete")) g_plus.del = kept;
    else if (KEY("contains")) g_plus.contains = kept;
    else if (KEY("match")) g_plus.match = kept;
    else if (KEY("method")) g_plus.method = kept;
    else if (KEY("body")) g_plus.body = kept;
    else if (KEY("type")) g_plus.type = kept;
    else if (KEY("headers")) g_plus.headers = kept;
#undef KEY
    return 1;
}

// A template filled in for track lib: its values URL-encoded (url 1) or
// escaped as in a JSON string (0). 1 if it all fitted in out, 0 if cut.
static int plusFill(const char* tmpl, int lib, int url, char* out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    char idText[24];
    const char* p = tmpl;
    int whole = 1;
    for (; *p && o + 4 < cap;) {
        const char* v = NULL;
        int skip = 0;
        if (!memcmp(p, "{id}", 4)) {
            if (g_given[lib].id[0]) {
                v = g_given[lib].id;
            } else {
                fmt(idText, sizeof(idText), "sc%d", lib);
                v = idText;
            }
            skip = 4;
        } else if (!memcmp(p, "{title}", 7)) {
            v = g_lib[lib].title;
            skip = 7;
        } else if (!memcmp(p, "{artist}", 8)) {
            v = g_lib[lib].artist;
            skip = 8;
        } else if (!memcmp(p, "{album}", 7)) {
            v = g_lib[lib].album;
            skip = 7;
        } else if (!memcmp(p, "{file}", 6)) {
            v = g_tracks[lib];
            skip = 6;
        }
        if (!v) {
            out[o++] = *p++;
            continue;
        }
        p += skip;
        for (; *v && o + 4 < cap; v++) {
            const unsigned char c = (unsigned char)*v;
            const int plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                              c == '-' || c == '_' || c == '.' || c == '~';
            if (!url && (c == '"' || c == '\\')) {
                out[o++] = '\\';  // a body's: escaped as in a JSON string (its default type)
                out[o++] = (char)c;
            } else if (!url && c < 0x20) {
                out[o++] = ' ';
            } else if (!url || plain) {
                out[o++] = (char)c;
            } else {
                out[o++] = '%';
                out[o++] = hex[c >> 4];
                out[o++] = hex[c & 15];
            }
        }
        if (*v) whole = 0;
    }
    if (*p) whole = 0;
    out[o] = 0;
    return whole;
}

// 1 if a body of this Content-Type is form-encoded (its values then
// URL-encoded, not JSON-escaped).
static int plusFormType(const char* type) {
    static const char want[] = "application/x-www-form-urlencoded";
    for (size_t i = 0; i < sizeof(want) - 1; i++) {
        char c = type[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != want[i]) return 0;
    }
    return 1;
}

typedef int (*FnHttpReq2)(int, const char*, const char*, uint64_t);

// One request: its HTTP status, or a negative code; its body's start in
// body (if given). method "" for GET.
static int plusRequest(const char* method, const char* url, const char* body, const char* type,
                       const char* headers, char* reply, size_t replyCap) {
    if (reply && replyCap) reply[0] = 0;
    if (!httpSetup()) return -1;
    static FnHttpReq2 req2;
    static int looked;
    if (!looked) {
        looked = 1;
        req2 = (FnHttpReq2)anySym(libLoad("libSceHttp.sprx"), "sceHttpCreateRequestWithURL2");
    }
    const char* m = method && method[0] ? method : "GET";
    const size_t bodyLen = body ? strlen(body) : 0;
    const int tmpl = g_http.tmpl(g_http.ctx, "ShellCompanion/" SC_BGM_VERSION, 2, 1);
    if (tmpl < 0) return tmpl;
    int conn = g_http.conn(tmpl, url, 1), req = -1, code = -2;
    if (conn >= 0) {
        // With a method's name (sceHttpCreateRequestWithURL2), else its number.
        static const char* const names[] = {"GET", "POST", "HEAD", "OPTIONS", "PUT", "DELETE"};
        int num = -1;
        for (int i = 0; i < 6; i++)
            if (libSame(m, names[i])) num = i;
        if (req2) req = req2(conn, m, url, bodyLen);
        else if (num >= 0) req = g_http.req(conn, num, url, bodyLen);
    }
    if (req >= 0) {
        if (g_http.connectTimeout) g_http.connectTimeout(req, 10u * 1000000u);
        if (g_http.recvTimeout) g_http.recvTimeout(req, 15u * 1000000u);
        httpHeaders(req, headers);
        if (bodyLen && g_http.addHeader) g_http.addHeader(req, "Content-Type", type && type[0] ? type : "application/json", 0);
        const int rc = g_http.send(req, bodyLen ? body : NULL, bodyLen);
        if (rc >= 0) {
            g_http.status(req, &code);
            size_t got = 0;
            while (reply && got + 1 < replyCap) {
                const int n = g_http.read(req, reply + got, replyCap - 1 - got);
                if (n <= 0) break;
                got += (size_t)n;
            }
            if (reply && replyCap) reply[got] = 0;
        } else {
            code = rc;
        }
        g_http.delReq(req);
    }
    if (conn >= 0) g_http.delConn(conn);
    g_http.delTmpl(tmpl);
    return code;
}

// liked.txt with path in it or not, as the music watch keeps it.
static void plusLiked(const char* path, int on) {
    char* text;
    size_t mapped;
    const int64_t len = readMapped(SC_BGM_DIR "/liked.txt", &text, &mapped);
    const size_t pn = strlen(path);
    int had = 0;
    for (int64_t at = 0; at < len;) {
        int64_t end = at;
        while (end < len && text[end] != '\n') end++;
        if ((size_t)(end - at) == pn && !memcmp(text + at, path, pn)) had = 1;
        at = end + 1;
    }
    if (had == on) {
        if (text) sysUnmap(text, mapped);
        return;
    }
    const int fd = sysOpen(SC_BGM_DIR "/liked.txt.tmp", SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    if (fd >= 0) {
        for (int64_t at = 0; at < len;) {
            int64_t end = at;
            while (end < len && text[end] != '\n') end++;
            if (end > at && !((size_t)(end - at) == pn && !memcmp(text + at, path, pn))) {
                sysWrite(fd, text + at, (size_t)(end - at));
                sysWrite(fd, "\n", 1);
            }
            at = end + 1;
        }
        if (on) {
            sysWrite(fd, path, pn);
            sysWrite(fd, "\n", 1);
        }
        sysClose(fd);
        sysRename(SC_BGM_DIR "/liked.txt.tmp", SC_BGM_DIR "/liked.txt");
    }
    if (text) sysUnmap(text, mapped);
}

// The jobs, one at a time: 1 save, 2 delete, 3 contains, each filled in
// when queued, on the player's thread (plusPressed, plusTrackBegun), with
// copies of all it needs: nothing in g_pool or g_lib is read by the thread.
#define PLUS_JOBS 16
typedef struct {
    int kind;
    char url[PLUS_URL], body[PLUS_BODY], headers[PLUS_HEADERS], match[PLUS_MATCH];
    char method[16], type[128];
    char path[MAX_PATH], title[META_LEN];
} PlusJob;
static struct {
    int head, tail;  // head the player's thread's, tail the "+" thread's
    PlusJob* job;    // PLUS_JOBS of them, mapped at the first
    int started;
} g_plusQ;

static void* plusMain(void* arg) {
    (void)arg;
    static char reply[4096];
    for (;;) {
        while (__atomic_load_n(&g_plusQ.head, __ATOMIC_ACQUIRE) == g_plusQ.tail) sysSleepMs(100);
        const PlusJob* j = &g_plusQ.job[g_plusQ.tail % PLUS_JOBS];
        char shown[160];
        urlShown(j->url, shown, sizeof(shown));
        if (j->kind == 3) {
            const int code = plusRequest("GET", j->url, NULL, NULL, j->headers, reply, sizeof(reply));
            const int saved = code >= 200 && code < 300 && (!j->match[0] || hasText(reply, j->match));
            plusLiked(j->path, saved);
            logLine("plus: contains %s: HTTP %d: %s", shown, code, saved ? "saved" : "not saved");
        } else {
            const int code = plusRequest(j->method, j->url, j->body[0] ? j->body : NULL, j->type, j->headers, NULL, 0);
            logLine("plus: %s %s: HTTP %d", j->kind == 1 ? "save" : "delete", shown, code);
            if (code < 200 || code > 299)
                notify("%s failed (%d): %s", j->kind == 1 ? "Adding" : "Removing", code, j->title);
        }
        __atomic_store_n(&g_plusQ.tail, g_plusQ.tail + 1, __ATOMIC_RELEASE);  // the slot is free again
    }
    return NULL;
}

// A copy of s into out, 1 if whole.
static int plusCopy(char* out, size_t cap, const char* s) {
    return fmt(out, cap, "%s", s ? s : "") < (int)cap;
}

// The player's thread only (it alone changes g_pool, g_lib and g_plus).
static void plusQueue(int kind, int lib) {
    if (lib < 0 || lib >= g_libCount) return;
    const char* tmpl = kind == 1 ? g_plus.save : kind == 2 ? g_plus.del : g_plus.contains;
    if (!tmpl[0]) return;
    if (!g_plusQ.job) {
        g_plusQ.job = (PlusJob*)sysMap(sizeof(PlusJob) * PLUS_JOBS);
        if (!g_plusQ.job) return;
    }
    if (!g_plusQ.started) g_plusQ.started = threadStart(plusMain, NULL, "ScBgmPlus") >= 0;
    if (g_plusQ.head - __atomic_load_n(&g_plusQ.tail, __ATOMIC_ACQUIRE) >= PLUS_JOBS) {
        logLine("plus: %d request(s) waiting already: %s not sent", PLUS_JOBS, g_lib[lib].title);
        return;
    }
    PlusJob* j = &g_plusQ.job[g_plusQ.head % PLUS_JOBS];
    j->kind = kind;
    const int form = plusFormType(g_plus.type);
    int whole = plusFill(tmpl, lib, 1, j->url, sizeof(j->url));
    j->body[0] = 0;
    if (kind != 3) whole &= plusFill(g_plus.body, lib, form, j->body, sizeof(j->body));
    whole &= plusCopy(j->headers, sizeof(j->headers), g_plus.headers);
    whole &= plusCopy(j->match, sizeof(j->match), g_plus.match);
    whole &= plusCopy(j->method, sizeof(j->method), g_plus.method);
    whole &= plusCopy(j->type, sizeof(j->type), g_plus.type);
    whole &= plusCopy(j->path, sizeof(j->path), g_tracks[lib]);
    plusCopy(j->title, sizeof(j->title), g_lib[lib].title);
    if (!whole) {
        // Cut, it would be sent to the wrong place or with the wrong values.
        logLine("plus: %s for %s too long for a request: not sent",
                kind == 1 ? "save" : kind == 2 ? "delete" : "contains", j->title);
        if (kind != 3) notify("%s failed (too long): %s", kind == 1 ? "Adding" : "Removing", j->title);
        return;
    }
    __atomic_store_n(&g_plusQ.head, g_plusQ.head + 1, __ATOMIC_RELEASE);
}

// A press of "+" (plus_save=<lib>, plus_delete=<lib> from the music watch).
static void plusPressed(int save, int lib) {
    if (lib < 0 || lib >= g_libCount) return;
    char line[1024];
    int n = fmt(line, sizeof(line), "%lu\t%s\t%s\t%s\n", (unsigned long)(sysNowUs() / 1000u), save ? "save" : "delete",
                      g_given[lib].id[0] ? g_given[lib].id : "", g_tracks[lib]);
    // fmt answers the length it wanted (1.51: that many were written, past
    // line's end when cut); a cut line still ends in its newline.
    if (n >= (int)sizeof(line)) {
        n = (int)sizeof(line) - 1;
        line[n - 1] = '\n';
    }
    const int fd = n > 0 ? sysOpen(SC_BGM_DIR "/plus-events.txt", SC_O_WRONLY | SC_O_CREAT, 0666) : -1;
    if (fd >= 0) {
        sysSeek(fd, 0, 2);
        sysWrite(fd, line, (size_t)n);
        sysClose(fd);
    }
    logLine("plus: %s %s", save ? "saved" : "removed", g_lib[lib].title);
    if (g_plus.http) plusQueue(save ? 1 : 2, lib);
}

// A track begun: whether the app's server has it saved, if it says how.
static void plusTrackBegun(int lib) {
    if (g_plus.http && g_plus.contains[0]) plusQueue(3, lib);
}
