/**
 * vita-shell-companion's client: the files an app writes for the plugin and
 * reads back (shell_companion.h, docs/PROTOCOL.md).
 */

#include "shell_companion.h"

#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Text into f with the characters the plugin splits on as %XX: '%' itself,
// '|' and tab (between fields), and line ends and other control
// characters. The rest goes as it is, so URLs stay as short as they were.
static void putEncoded(FILE* f, const char* s) {
    static const char kHex[] = "0123456789ABCDEF";
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        if (*p == '%' || *p == '|' || *p < 0x20 || *p == 0x7F) {
            fputc('%', f);
            fputc(kHex[*p >> 4], f);
            fputc(kHex[*p & 15], f);
        } else {
            fputc(*p, f);
        }
    }
}

// The system clock in seconds, as written= has it: it runs on from one
// process to the next, so the app can tell the plugin's files from older
// ones.
static void putWritten(FILE* f) {
    const SceUInt64 us = sceKernelGetSystemTimeWide();
    fprintf(f, "written=%llu.%06u\n", (unsigned long long)(us / 1000000u),
            (unsigned)(us % 1000000u));
}

// A file in the data folder opened for writing beside its name: the open
// file, with its name and the one beside it in path and part.
static FILE* openPart(const char* name, char* path, char* part, size_t size) {
    if (scPath(name, path, size) < 0) return NULL;
    snprintf(part, size, "%s.tmp", path);
    return fopen(part, "wb");
}

// The file ended, closed and moved in. 0, or -1.
static int finish(FILE* f, const char* path, const char* part) {
    fputs("end\n", f);
    const int ok = !ferror(f);
    if (fclose(f) != 0 || !ok) {
        remove(part);
        return -1;
    }
    remove(path);
    if (rename(part, path) != 0) {
        remove(part);
        return -1;
    }
    return 0;
}

static void putReport(FILE* f, const char* key, const ScReport* r) {
    if (!r->url || !r->url[0]) return;
    fprintf(f, "%s=", key);
    putEncoded(f, r->url);
    fputc('\n', f);
    if (r->method && r->method[0]) fprintf(f, "%s.method=%s\n", key, r->method);
    if (r->body && r->body[0]) {
        fprintf(f, "%s.body=", key);
        putEncoded(f, r->body);
        fputc('\n', f);
    }
    if (r->type && r->type[0]) {
        fprintf(f, "%s.type=", key);
        putEncoded(f, r->type);
        fputc('\n', f);
    }
}

int scWriteQueue(const ScQueue* q, const ScTrack* tracks, uint32_t count) {
    char path[256], part[256];
    if (count > SC_MAX_TRACKS) count = SC_MAX_TRACKS;
    FILE* f = openPart(SC_QUEUE, path, part, sizeof(path));
    if (!f) return -1;
    putWritten(f);
    fprintf(f, "index=%u\npositionMs=%u\nrepeat=%d\nofflineSkip=%d\n", (unsigned)q->index,
            (unsigned)q->positionMs, q->repeat, q->offlineSkip ? 1 : 0);
    if (q->headers && q->headers[0]) {
        fputs("headers=", f);
        putEncoded(f, q->headers);
        fputc('\n', f);
    }
    putReport(f, "report.state", &q->state);
    if (q->state.url && q->state.url[0] && q->stateEveryS)
        fprintf(f, "report.state.every=%u\n", (unsigned)q->stateEveryS);
    putReport(f, "report.played", &q->played);
    for (uint32_t i = 0; i < count; i++) {
        const ScTrack* t = &tracks[i];
        const char* const fields[SC_TRACK_FIELDS] = {
            t->id,     NULL,      t->local,  t->url,      t->ext,     t->altUrl,
            t->altPrep, t->title, t->artist, t->album,    t->coverUrl, t->coverPng};
        fputs("track=", f);
        for (int k = 0; k < SC_TRACK_FIELDS; k++) {
            if (k) fputc('|', f);
            if (k == SC_TRACK_DURATION_MS)
                fprintf(f, "%u", (unsigned)t->durationMs);
            else
                putEncoded(f, fields[k]);
        }
        fputc('\n', f);
    }
    return finish(f, path, part);
}

int scWriteDownloads(int notify, const char* headers, const ScDownload* items, uint32_t count) {
    char path[256], part[256];
    if (count > SC_MAX_DOWNLOADS) count = SC_MAX_DOWNLOADS;
    FILE* f = openPart(SC_DOWNLOADS, path, part, sizeof(path));
    if (!f) return -1;
    putWritten(f);
    fprintf(f, "notify=%d\n", notify ? 1 : 0);
    if (headers && headers[0]) {
        fputs("headers=", f);
        putEncoded(f, headers);
        fputc('\n', f);
    }
    for (uint32_t i = 0; i < count; i++) {
        const ScDownload* d = &items[i];
        fputs("item=", f);
        putEncoded(f, d->id);
        fputc('\t', f);
        putEncoded(f, d->file);
        fprintf(f, "\t%llu\t", (unsigned long long)d->size);
        putEncoded(f, d->title);
        fputc('\t', f);
        putEncoded(f, d->kind);
        for (int k = 0; k < 3; k++) {
            fputc('\t', f);
            putEncoded(f, d->args[k]);
        }
        fputc('\n', f);
    }
    return finish(f, path, part);
}

// A file of the plugin's read whole, ended with 0, into a malloc'd buffer;
// NULL when it is not there or has no "end" line.
static char* readWhole(const char* name) {
    char path[256];
    if (scPath(name, path, sizeof(path)) < 0) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char* buf = (char*)malloc(cap);
    while (buf) {
        if (n + 1 >= cap) {
            char* more = (char*)realloc(buf, cap * 2);
            if (!more) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = more;
            cap *= 2;
        }
        const size_t got = fread(buf + n, 1, cap - 1 - n, f);
        if (got == 0) break;
        n += got;
    }
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    if (!strstr(buf, "\nend\n") && strncmp(buf, "end\n", 4) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

// The value of key on its line in text, into out, cut to fit. 1 when there.
static int value(const char* text, const char* key, char* out, size_t size) {
    const size_t klen = strlen(key);
    for (const char* p = text; *p;) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t n = 0;
            for (; *p && *p != '\n' && *p != '\r'; p++)
                if (n + 1 < size) out[n++] = *p;
            out[n] = 0;
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (size) out[0] = 0;
    return 0;
}

int scReadQueueStatus(ScQueueStatus* out) {
    memset(out, 0, sizeof(*out));
    char* text = readWhole(SC_QUEUE_STATUS);
    if (!text) return -1;
    char num[24];
    value(text, "written", out->written, sizeof(out->written));
    value(text, "queue", out->queue, sizeof(out->queue));
    value(text, "id", out->id, sizeof(out->id));
    value(text, "state", out->state, sizeof(out->state));
    value(text, "index", num, sizeof(num));
    out->index = atoi(num);
    value(text, "positionMs", num, sizeof(num));
    out->positionMs = (uint32_t)strtoul(num, NULL, 10);
    value(text, "durationMs", num, sizeof(num));
    out->durationMs = (uint32_t)strtoul(num, NULL, 10);
    value(text, "fetching", num, sizeof(num));
    out->fetching = num[0] == '1';
    value(text, "paused", num, sizeof(num));
    out->paused = num[0] == '1';
    free(text);
    return 0;
}

// Up to the next tab of *p into out, cut to fit, and past it.
static void tabField(const char** p, char* out, size_t size) {
    size_t n = 0;
    for (; **p && **p != '\t' && **p != '\n'; (*p)++)
        if (n + 1 < size) out[n++] = **p;
    out[n] = 0;
    if (**p == '\t') (*p)++;
}

int scReadDownloadsStatus(int* running, int* paused, ScDownloadStatus* items, uint32_t cap,
                          uint32_t* count) {
    *count = 0;
    *running = *paused = 0;
    char* text = readWhole(SC_DOWNLOADS_STATUS);
    if (!text) return -1;
    char num[24];
    value(text, "running", num, sizeof(num));
    *running = num[0] == '1';
    value(text, "paused", num, sizeof(num));
    *paused = num[0] == '1';
    for (const char* p = text; *p && *count < cap;) {
        if (strncmp(p, "item=", 5) == 0) {
            p += 5;
            ScDownloadStatus* d = &items[(*count)++];
            memset(d, 0, sizeof(*d));
            tabField(&p, d->id, sizeof(d->id));
            tabField(&p, d->state, sizeof(d->state));
            tabField(&p, num, sizeof(num));
            d->bytes = strtoull(num, NULL, 10);
            tabField(&p, num, sizeof(num));
            d->size = strtoull(num, NULL, 10);
            tabField(&p, d->queueId, sizeof(d->queueId));
            tabField(&p, d->itemId, sizeof(d->itemId));
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    free(text);
    return 0;
}

int scReadUpdateResult(int* result, char* why, size_t whySize) {
    *result = 0;
    if (whySize) why[0] = 0;
    char* text = readWhole(SC_UPDATE_RESULT);
    if (!text) return -1;
    char num[24];
    value(text, "result", num, sizeof(num));
    *result = (int)strtoul(num, NULL, 16);
    value(text, "why", why, whySize);
    free(text);
    return 0;
}
