/**
 * vita-shell-companion's client: loading the plugin and its messages
 * (shell_companion.h). The files are sc_files.c's.
 */

#include "shell_companion.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>

// Not declared by every SDK's headers.
int sceAppMgrGetIdByName(SceUID* pid, const char* name);

// By NID (sc_link_stub.S): SceThreadmgr's pipe open and close, which the
// SDK's headers and stubs leave out, and taiHEN's loader into another
// process, with its arguments as taihen.h's tai_module_args_t has them.
SceUID scStubOpenMsgPipe(const char* name);
int scStubCloseMsgPipe(SceUID uid);
typedef struct TaiModuleArgs {
    size_t size;
    SceUID pid;
    size_t args;
    void* argp;
    int flags;
} TaiModuleArgs;
SceUID scStubTaiLoadStartModuleForPidForUser(const char* path, TaiModuleArgs* args);

#define SHELL_TITLEID "NPXS19999"
// Plugins of later protocols serve this one too: looked for up to this many
// versions on.
#define LATER_VERSIONS 8

_Static_assert(sizeof(ScMessage) == 1556, "sc_protocol.h's message, as the plugin takes it");
_Static_assert(sizeof(ScPress) == 16, "sc_protocol.h's press, as the plugin sends it");

// A message pipe moves a whole message or none of it with this mode, and
// without it as much as fits (Vita3K's kernel/types.h:
// SCE_KERNEL_MSG_PIPE_MODE_FULL; waiting is mode 0).
#define MSG_PIPE_MODE_FULL 0x00000001
// The plugin takes each message the moment it comes, so one that cannot go in
// this long finds no plugin taking it, and is dropped.
#define SEND_TIMEOUT_US (200 * 1000)

static SceUID g_lock = -1;
static SceUID g_to = -1, g_from = -1;
// Who the app is (scInit).
static char g_titleId[12];
static char g_dataDir[160];
static char g_name[64];
static uint32_t g_features;
static char g_bundled[256];
// Under g_lock: 1.5 KB, off the callers' stacks.
static ScMessage g_message;
static ScPress g_press;
// Under g_lock: the plugin has answered this app's HELLO (SC_REPLY_HELLO).
static int g_answered;

static void lock(void) {
    if (g_lock >= 0) sceKernelLockMutex(g_lock, 1, NULL);
}
static void unlock(void) {
    if (g_lock >= 0) sceKernelUnlockMutex(g_lock, 1);
}

static void copy(char* out, size_t size, const char* s) {
    snprintf(out, size, "%s", s ? s : "");
}

int scInit(const char* titleId, const char* dataDir, const char* name, uint32_t features,
           const char* bundledPluginPath) {
    if (!titleId || !titleId[0] || !dataDir || !dataDir[0]) return -1;
    if (g_lock < 0) g_lock = sceKernelCreateMutex("ShellCompanionClient", 0, 0, NULL);
    lock();
    copy(g_titleId, sizeof(g_titleId), titleId);
    copy(g_dataDir, sizeof(g_dataDir), dataDir);
    size_t n = strlen(g_dataDir);
    while (n > 0 && g_dataDir[n - 1] == '/') g_dataDir[--n] = 0;
    copy(g_name, sizeof(g_name), name && name[0] ? name : titleId);
    copy(g_bundled, sizeof(g_bundled), bundledPluginPath);
    g_features = features;
    unlock();
    sceIoMkdir(g_dataDir, 0777);
    return 0;
}

int scPath(const char* name, char* out, size_t size) {
    if (!g_dataDir[0]) return -1;
    const int n = snprintf(out, size, "%s/%s", g_dataDir, name);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

int scIsOpen(void) {
    lock();
    const int open = g_to >= 0 && g_from >= 0;
    unlock();
    return open;
}

// UTF-8 to the UTF-16 the plugin takes, ending in 0, cut to fit; what is not
// UTF-8 becomes U+FFFD.
static void toUtf16(const char* s, uint16_t* out, size_t cap) {
    const unsigned char* p = (const unsigned char*)(s ? s : "");
    size_t n = 0;
    while (*p && n + 1 < cap) {
        unsigned c = *p++;
        int more = 0;
        if (c >= 0xF0 && c <= 0xF4) {
            c &= 0x07;
            more = 3;
        } else if (c >= 0xE0 && c <= 0xEF) {
            c &= 0x0F;
            more = 2;
        } else if (c >= 0xC2 && c <= 0xDF) {
            c &= 0x1F;
            more = 1;
        } else if (c >= 0x80) {
            c = 0xFFFD;
        }
        for (; more > 0; more--) {
            if ((*p & 0xC0) != 0x80) {
                c = 0xFFFD;
                break;
            }
            c = (c << 6) | (*p++ & 0x3F);
        }
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        if (c >= 0x10000) {
            if (n + 2 >= cap) break;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (c >> 10));
            out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    out[n] = 0;
}

// Under g_lock: g_message cleared for cmd, with the app's title on it.
static void beginLocked(uint32_t cmd, uint32_t arg) {
    memset(&g_message, 0, sizeof(g_message));
    g_message.cmd = cmd;
    g_message.arg = arg;
    memcpy(g_message.titleId, g_titleId, sizeof(g_message.titleId));
}

// Under g_lock: g_message to the pipe `to`.
static int sendTo(SceUID to) {
    unsigned int timeout = SEND_TIMEOUT_US;
    return sceKernelSendMsgPipe(to, &g_message, sizeof(g_message), MSG_PIPE_MODE_FULL, NULL,
                                &timeout);
}

// Under g_lock: the HELLO, into g_message.
static void helloLocked(void) {
    beginLocked(SC_CMD_HELLO, g_features);
    toUtf16(g_dataDir, g_message.text1, 256);
    toUtf16(g_name, g_message.text2, 256);
}

// A plugin's pipe to it of this protocol or a later one: the pipe, and the
// version found in *version; else the code of the last open.
static SceUID openToPlugin(unsigned* version) {
    SceUID to = -1;
    for (unsigned v = SC_PROTOCOL; v < SC_PROTOCOL + LATER_VERSIONS && to < 0; v++) {
        char name[48];
        snprintf(name, sizeof(name), SC_PIPE_TO_PLUGIN_FMT, v);
        to = scStubOpenMsgPipe(name);
        if (to >= 0) *version = v;
    }
    return to;
}

// Loads the plugin into SceShell, as ElevenMPV-A does its own.
static SceUID loadIntoShell(const char* path) {
    SceUID shell = -1;
    const int rc = sceAppMgrGetIdByName(&shell, SHELL_TITLEID);
    if (rc < 0) return rc;
    TaiModuleArgs args;
    memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.pid = shell;
    return scStubTaiLoadStartModuleForPidForUser(path, &args);
}

int scOpen(char* what, size_t size) {
    if (!g_titleId[0]) {
        snprintf(what, size, "scInit has not run");
        return -1;
    }
    lock();
    if (g_to >= 0 && g_from >= 0) {
        snprintf(what, size, "the plugin is open already");
        unlock();
        return 0;
    }
    unsigned version = SC_PROTOCOL;
    SceUID to = openToPlugin(&version);
    int n = 0;
    if (to >= 0) {
        n = snprintf(what, size, "the plugin (protocol %u) was in SceShell already", version);
    } else {
        SceIoStat st;
        const int stat = g_bundled[0] ? sceIoGetstat(g_bundled, &st) : -1;
        if (stat < 0) {
            snprintf(what, size, "no plugin in SceShell, and none at \"%s\"", g_bundled);
            unlock();
            return stat;
        }
        const SceUID module = loadIntoShell(g_bundled);
        if (module < 0) {
            snprintf(what, size, "loading the plugin (%s) into SceShell: 0x%08X", g_bundled,
                     (unsigned)module);
            unlock();
            return module;
        }
        to = openToPlugin(&version);
        n = snprintf(what, size, "loaded the plugin (%s) into SceShell (module 0x%08X)", g_bundled,
                     (unsigned)module);
    }
    char fromName[48];
    snprintf(fromName, sizeof(fromName), SC_PIPE_FROM_PLUGIN_FMT, version);
    const SceUID from = scStubOpenMsgPipe(fromName);
    if (to < 0 || from < 0) {
        if (n >= 0 && (size_t)n < size)
            snprintf(what + n, size - (size_t)n, ", but its pipes would not open: 0x%08X, 0x%08X",
                     (unsigned)to, (unsigned)from);
        if (to >= 0) scStubCloseMsgPipe(to);
        if (from >= 0) scStubCloseMsgPipe(from);
        unlock();
        return to < 0 ? to : from;
    }
    helloLocked();
    const int rc = sendTo(to);
    if (rc < 0) {
        if (n >= 0 && (size_t)n < size)
            snprintf(what + n, size - (size_t)n, ", but it took no hello: 0x%08X", (unsigned)rc);
        scStubCloseMsgPipe(to);
        scStubCloseMsgPipe(from);
        unlock();
        return rc;
    }
    g_to = to;
    g_from = from;
    // Its answer, written first: up to half a second; a plugin before 0.2.0
    // gives none. A press that comes meanwhile is for an app gone before.
    g_answered = 0;
    for (int i = 0; i < 25 && !g_answered; i++) {
        memset(&g_press, 0, sizeof(g_press));
        if (sceKernelTryReceiveMsgPipe(g_from, &g_press, sizeof(g_press), MSG_PIPE_MODE_FULL,
                                       NULL) < 0) {
            sceKernelDelayThread(20 * 1000);
            continue;
        }
        g_answered = g_press.press == SC_REPLY_HELLO &&
                     strncmp(g_press.titleId, g_titleId, sizeof(g_press.titleId)) == 0;
    }
    if (!g_answered && n >= 0 && (size_t)n < size)
        snprintf(what + n, size - (size_t)n, "; it gave no answer to hello (before 0.2.0)");
    unlock();
    return 0;
}

int scPluginInfo(ScPluginInfo* out) {
    lock();
    const int answered = g_answered;
    unlock();
    if (!answered) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return scReadPluginInfo(out);
}

// Under g_lock: g_message, as filled in, to the plugin this process opened.
static int sendLocked(void) {
    if (g_to < 0) return -1;
    return sendTo(g_to);
}

static int sendCommand(uint32_t cmd, uint32_t arg) {
    lock();
    beginLocked(cmd, arg);
    const int rc = sendLocked();
    unlock();
    return rc;
}

// Under g_lock: g_message, as filled in, to the plugin, whether or not this
// process has opened it: only to one that is there, as one this would load
// has carried nothing on. A plugin this process has not opened gets the
// HELLO first, which the plugin may have from an earlier run anyway.
static int sendToPresentLocked(void) {
    if (g_to >= 0) return sendLocked();
    unsigned version = SC_PROTOCOL;
    const SceUID to = openToPlugin(&version);
    if (to < 0) return to;
    static ScMessage held;
    held = g_message;
    helloLocked();
    int rc = sendTo(to);
    g_message = held;
    if (rc >= 0) rc = sendTo(to);
    scStubCloseMsgPipe(to);
    return rc;
}

int scRowActivate(void) {
    lock();
    int rc = -1;
    if (g_to >= 0) {
        beginLocked(SC_CMD_ROW_ACTIVATE, 0);
        rc = sceKernelTrySendMsgPipe(g_to, &g_message, sizeof(g_message), MSG_PIPE_MODE_FULL,
                                     NULL);
    }
    unlock();
    return rc;
}

int scRowRelease(void) {
    return sendCommand(SC_CMD_ROW_RELEASE, 0);
}

int scRowSong(const char* title, const char* artist, const char* album) {
    lock();
    beginLocked(SC_CMD_ROW_SONG, 0);
    toUtf16(title, g_message.text1, 256);
    toUtf16(artist, g_message.text2, 256);
    toUtf16(album, g_message.text3, 256);
    const int rc = sendLocked();
    unlock();
    return rc;
}

int scRowPlaying(int playing) {
    return sendCommand(SC_CMD_ROW_PLAYING, playing ? 1 : 0);
}

int scRowArt(uint32_t serial) {
    return sendCommand(SC_CMD_ROW_ART, serial);
}

int scPoll(void) {
    lock();
    int press = SC_NONE;
    while (g_from >= 0 && press == SC_NONE) {
        memset(&g_press, 0, sizeof(g_press));
        if (sceKernelTryReceiveMsgPipe(g_from, &g_press, sizeof(g_press), MSG_PIPE_MODE_FULL,
                                       NULL) < 0)
            break;
        // A press for another app, one that had the row before, is dropped.
        if (strncmp(g_press.titleId, g_titleId, sizeof(g_press.titleId)) != 0) continue;
        if (g_press.press == SC_REPLY_HELLO) {
            g_answered = 1;  // late, or for a HELLO sent again
            continue;
        }
        press = g_press.press == SC_PRESS_PLAY_PAUSE ? SC_PLAY_PAUSE
              : g_press.press == SC_PRESS_NEXT       ? SC_NEXT
              : g_press.press == SC_PRESS_PREVIOUS   ? SC_PREVIOUS
                                                     : SC_NONE;
    }
    unlock();
    return press;
}

int scMusicPlay(void) {
    return sendCommand(SC_CMD_MUSIC_PLAY, 0);
}

int scMusicTakeBack(void) {
    lock();
    beginLocked(SC_CMD_MUSIC_TAKE_BACK, 0);
    const int rc = sendToPresentLocked();
    unlock();
    return rc;
}

int scDownloadsStart(void) {
    return sendCommand(SC_CMD_DL_START, 0);
}

int scDownloadsTakeBack(void) {
    lock();
    beginLocked(SC_CMD_DL_TAKE_BACK, 0);
    const int rc = sendToPresentLocked();
    unlock();
    return rc;
}

static int sendLines(uint32_t cmd, uint32_t arg, const char* first, const char* second) {
    lock();
    beginLocked(cmd, arg);
    toUtf16(first, g_message.text1, 256);
    toUtf16(second, g_message.text2, 256);
    const int rc = sendLocked();
    unlock();
    return rc;
}

int scDownloadProgress(const char* line1, const char* line2, uint32_t permille) {
    return sendLines(SC_CMD_DL_PROGRESS, permille, line1, line2);
}

int scDownloadProgressEnd(const char* line1, const char* line2) {
    return sendLines(SC_CMD_DL_PROGRESS_END, 0, line1, line2);
}

int scDownloadPauseAsked(void) {
    char path[200];
    if (scPath(SC_DOWNLOADS_PAUSE, path, sizeof(path)) < 0) return 0;
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) return 0;
    sceIoRemove(path);
    return 1;
}

int scUpdateInstall(void) {
    return sendCommand(SC_CMD_UPDATE_INSTALL, 0);
}

int scProbeStream(int mode, const char* sourceWav) {
    lock();
    beginLocked(SC_CMD_PROBE_STREAM, (uint32_t)mode);
    toUtf16(sourceWav, g_message.text1, 256);
    const int rc = sendLocked();
    unlock();
    return rc;
}

int scSendspinStart(void) {
    return sendCommand(SC_CMD_SENDSPIN_START, 0);
}

int scSendspinTakeBack(void) {
    lock();
    beginLocked(SC_CMD_SENDSPIN_TAKE_BACK, 0);
    const int rc = sendToPresentLocked();
    unlock();
    return rc;
}
