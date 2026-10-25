/**
 * vita-shell-companion: a plugin inside SceShell that any homebrew app can
 * load, so that its work goes on after it closes. Each app registers itself
 * (SC_CMD_HELLO) with the features it wants, and its data folder, where all
 * its files for the plugin live (docs/PROTOCOL.md):
 *
 * - The Music row of the quick menu (hold PS): the app's song, its cover,
 *   and buttons that work, while the app plays.
 * - Background music: the app's queue played on through SceShell's own music
 *   service once the app has gone, tracks not on the card fetched from the
 *   URLs the app gave, and the app's server told where the music is through
 *   the report templates it gave.
 * - Background downloads, by URL, with one progress notification of
 *   SceShell's that the app's own downloads show through too.
 * - Self-update: the app's update installed once it has gone, as the
 *   system's installer refuses a title while it runs.
 *
 * The row is SceShell's, and only code inside SceShell can change it. An app
 * loads this module into SceShell with taiHEN (client/shell_companion.c) the
 * first time it wants it in a boot, and it stays until the Vita restarts.
 * The two talk through the message pipes sc_protocol.h lays out.
 *
 * Nothing here knows any particular server: every request it makes is a URL
 * an app wrote, with the header lines the app wrote for it.
 *
 * It began as VitaPlex's quick menu plugin, which does for the row what
 * ElevenMPV-A's shell plugin does (GrapheneCt/ElevenMPV-A, GPL-3.0), in its
 * way: it finds the quick menu through power_manage_plugin, the row's
 * buttons and two lines of text by their id hashes, and while the quick menu
 * is open puts the song on the lines and its own callbacks on the buttons.
 * It differs where that plugin fell short:
 *
 * - It changes SceShell's widgets on SceShell's own UI thread, through PAF's
 *   main thread call list, as ElevenMPV-A's app does its own. Its own threads
 *   watch the quick menu, take the apps' messages and write the log.
 * - It switches the buttons on and lights them and their icons, which
 *   SceShell draws faded with no music of its own: the icons' own alpha,
 *   0.3, is set to 1, and put back when the row is SceShell's again.
 * - It takes the row 200 ms after the quick menu opens, as ElevenMPV-A's
 *   plugin about does: the song put on the lines at once did not show. While
 *   the quick menu stays open it puts back what SceShell changes: buttons
 *   switched off, other text.
 * - While the app plays, the play icon shows ⏸; and the Music heading's icon
 *   shows the song's cover, which the app writes to a file (SC_ART_FMT).
 * - A press is sent without waiting, so an app that stopped reading cannot
 *   hold SceShell up, and the row is given back by itself when no app has
 *   asked for it again in SC_ROW_TIMEOUT_S.
 *
 * The comments name VitaPlex's builds and logs where they found out how
 * SceShell behaves; plugin/README-vitaplex.md has that history.
 *
 * Built with vitasdk against the PAF headers scripts/fetch_paf_headers.sh
 * fetches; its PAF imports are in paf_stubs.S. Logs to SC_LOG, one file a
 * boot.
 */

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>

#include <limits>  // before PAF's headers: their vector uses it without including it
#include <stdarg.h>
#include <stdint.h>

#include <paf.h>

#include "mp3_frames.h"
#include "sendspin_wire.h"
#include "sc_protocol.h"

// ── taiHEN, by NID (taihen_stub.S) ──────────────────────────────────────────
// As taihen.h has them.
typedef uintptr_t TaiHookRef;
typedef struct TaiHookArgs {
    size_t size;
    const char* module;
    uint32_t library_nid;
    uint32_t func_nid;
    const void* hook_func;
} TaiHookArgs;
typedef struct TaiHookUser {
    uintptr_t next;
    void* func;
    void* old;
} TaiHookUser;
extern "C" SceUID scHookFunctionImport(TaiHookRef* hook, TaiHookArgs* args);
extern "C" int scGetModuleExportFunc(const char* module, uint32_t libnid,
                                             uint32_t funcnid, uintptr_t* func);
// Not declared by every SDK's headers.
extern "C" int sceAppMgrGetIdByName(SceUID* pid, const char* name);


// No C runtime here: what the compiler may call for it, from the kernel's.
// new and delete are PAF's (built with -fno-sized-deallocation, so delete is
// always the one PAF exports), as in ElevenMPV-A's plugin.
extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    return sceClibMemcpy(dst, src, n);
}
extern "C" void* memset(void* dst, int c, size_t n) {
    return sceClibMemset(dst, c, n);
}
// What a pure virtual function's slot holds in the vtables of PAF's abstract
// classes this module emits; never called.
extern "C" void __cxa_pure_virtual() {
    for (;;) sceKernelDelayThread(1000000);
}

namespace {

// ── The log ─────────────────────────────────────────────────────────────────
// Lines are put together where they happen, SceShell's UI thread included,
// and kept in a ring; the watch thread writes them to the card, so that no
// line waits for the card on a thread SceShell draws with.
#define SC_LOG_DIR "ux0:data/ShellCompanion"
const char* const kLogPath = SC_LOG_DIR "/plugin.log";
const char* const kLogPrevPath = SC_LOG_DIR "/plugin.prev.log";
const int kLogMax = 512 * 1024;
const int kRingLines = 128;
const int kLineMax = 320;

struct RingLine {
    int n;
    char text[kLineMax];
};
RingLine g_ring[kRingLines];
int g_ringHead, g_ringCount, g_ringDropped;
SceUID g_ringLock = -1;
SceUID g_log = -1;
int g_logged;

void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logLine(const char* fmt, ...) {
    if (g_ringLock < 0) return;
    char line[kLineMax];
    SceDateTime t;
    sceClibMemset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    int n = sceClibSnprintf(line, sizeof(line), "%02u:%02u:%02u.%03u ", t.hour, t.minute,
                            t.second, t.microsecond / 1000);
    va_list ap;
    va_start(ap, fmt);
    n += sceClibVsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    sceKernelLockMutex(g_ringLock, 1, NULL);
    if (g_ringCount < kRingLines) {
        RingLine& r = g_ring[(g_ringHead + g_ringCount) % kRingLines];
        sceClibMemcpy(r.text, line, n);
        r.n = n;
        g_ringCount++;
    } else {
        g_ringDropped++;
    }
    sceKernelUnlockMutex(g_ringLock, 1);
}

// The watch thread's: the lines kept so far, to the card.
void logDrain() {
    if (g_log < 0) return;
    int wrote = 0;
    for (;;) {
        RingLine line;
        int dropped = 0;
        sceKernelLockMutex(g_ringLock, 1, NULL);
        const bool any = g_ringCount > 0;
        if (any) {
            line = g_ring[g_ringHead];
            g_ringHead = (g_ringHead + 1) % kRingLines;
            g_ringCount--;
        } else {
            dropped = g_ringDropped;
            g_ringDropped = 0;
        }
        sceKernelUnlockMutex(g_ringLock, 1);
        if (!any) {
            if (dropped) {
                char note[64];
                const int n = sceClibSnprintf(note, sizeof(note), "(%d lines not kept)\n", dropped);
                sceIoWrite(g_log, note, n);
                wrote++;
            }
            break;
        }
        if (g_logged <= kLogMax) {
            sceIoWrite(g_log, line.text, line.n);
            g_logged += line.n;
            wrote++;
        }
    }
    if (wrote) sceIoSyncByFd(g_log, 0);  // a SceShell crash keeps what came before it
}

void logOpen() {
    g_ringLock = sceKernelCreateMutex("ShellCompanion::Log", 0, 0, NULL);
    sceIoMkdir(SC_LOG_DIR, 0777);
    sceIoRemove(kLogPrevPath);
    sceIoRename(kLogPath, kLogPrevPath);
    g_log = sceIoOpen(kLogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
}

// FNV-1a over UTF-16, to tell texts apart.
uint32_t hashText(const uint16_t* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; s++) h = (h ^ *s) * 16777619u;
    return h;
}

bool startsWith(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++)
        if (*s != *prefix) return false;
    return true;
}

bool same(const char* a, const char* b) {
    for (; *a && *a == *b; a++, b++) {
    }
    return *a == *b;
}

// src, cut to fit, into dst, always ended with 0.
void copyText(char* dst, const char* src, int cap) {
    int n = 0;
    for (; src[n] && n < cap - 1; n++) dst[n] = src[n];
    dst[n] = 0;
}

// ── State ───────────────────────────────────────────────────────────────────
// The Music row's parts, by the id hashes SceShell's quick menu gives them:
// the buttons and lines as ElevenMPV-A's main.h and main.cpp have them, the
// rest as the plugin's log listed the row (build 2022).
const uint32_t kHashRew = 0x9F6A44DA;
const uint32_t kHashPlay = 0x56734CD1;
const uint32_t kHashFf = 0x6CA674D6;
const uint32_t kHashTextTop = 0x66FDAFE3;
const uint32_t kHashTextBottom = 0xF099B450;
// 64x64 planes in the row where the buttons are, listed just before each:
// their icons, by all appearances.
const uint32_t kHashIconRew = 0xD639AB27;
const uint32_t kHashIconPlay = 0xE057A7D0;
const uint32_t kHashIconFf = 0x6FA104CC;
// One more there, at play's place and sized 0x0: the pause icon. SceShell's
// own row shows it in play's stead, by size, while its Music app plays
// (build 2025).
const uint32_t kHashIconPause = 0xB26E4562;
// The row's heading's icon, "Music"'s: a plane at its group's left, 0x0 and
// drawn at its texture's 80x80, as the other rows' icons are.
const uint32_t kHashHeadingIcon = 0xE219E0DE;
// A third line at the title's place, "There are no songs.", which SceShell
// keeps at an own alpha of 0 (build 2028's timeline, log c1930c5a).
const uint32_t kHashNoSongs = 0x5608C548;
// The song's lines' own alphas as SceShell has them as the quick menu opens,
// before it fades them out with no music of its own: the title's 1, the
// artist and album's 0.8 (log c1930c5a, where they went to 0 by 429 ms).
const float kTopAlpha = 1.0f, kBottomAlpha = 0.8f;

// SceAppMgr's app state, as ElevenMPV-A's plugin asks for it (its build of
// sceAppMgrGetAppState passes 0x03570000).
const uint32_t kAppStateVersion = 0x03570000;
// Its threads' priority, the SDK's SCE_KERNEL_LOWEST_PRIORITY_USER.
const int kThreadPriority = 191;
// A message pipe moves a whole message or none of it with this mode (Vita3K's
// kernel/types.h, SCE_KERNEL_MSG_PIPE_MODE_FULL; waiting is mode 0).
const int kPipeModeFull = 0x00000001;
// How long after the quick menu is seen open the row is taken. ElevenMPV-A's
// plugin waits 100 ms and then looks for the buttons, and its song showed;
// the song this one put on the lines at once did not (build 2022).
const SceUInt64 kTakeAfterUs = 200 * 1000;
// The row is looked at ten times a second while the quick menu is open.
const SceUInt64 kTickUs = 100 * 1000;
// PAF's Plugin keeps the pool its textures come from here (ScePaf-RE's
// plugin.h, surface_pool; private there).
const int kPluginSurfacePool = 0xD4;

// No object here may need constructing as the module loads: it has no C
// runtime to run constructors, so such an object stays all zeros. Build
// 2023's white was, and set the song's lines transparent. CMake fails the
// build if the module has any (.init_array).
paf::math::v4 white() {
    return paf::math::v4(1.0f, 1.0f, 1.0f, 1.0f);
}

paf::ui::Widget* g_impose;               // the quick menu's root
paf::graph::SurfacePool* g_surfacePool;  // power_manage_plugin's, for the cover
SceUID g_toPlugin = -1, g_fromPlugin = -1;
SceUID g_lock = -1;  // everything below, and the widgets

// ── The apps ────────────────────────────────────────────────────────────────
// Each app that has said SC_CMD_HELLO this boot, by its title id. An entry is
// never removed, so its index names the app for good: a HELLO again from the
// same title updates its folder, name and features in place. Under g_lock;
// a thread that works for an app takes a copy of its entry as it begins.
struct App {
    char titleId[12];
    char folder[96];   // its data folder, no '/' at its end
    char name[48];     // UTF-8
    uint32_t features;  // SC_FEAT_*
};
App g_apps[SC_MAX_APPS];
int g_appCount;

// The app at index i, or NULL. Under g_lock.
const App* appAt(int i) {
    return i >= 0 && i < g_appCount ? &g_apps[i] : NULL;
}

// A path in an app's folder, into out.
void appPath(const App* a, const char* name, char* out, int cap) {
    sceClibSnprintf(out, cap, "%s/%s", a->folder, name);
}

// The row's owner: an app (kOwnerApp), whose index is g_ownerApp; or
// SceShell itself (kOwnerShell) while it plays the queue g_ownerApp handed
// it.
const uint32_t kOwnerApp = 1;
const uint32_t kOwnerShell = 3;
// The serials of the covers SceShell shows as it carries a queue on. An
// app's serials are milliseconds since boot, which never reach this bit.
const uint32_t kShellArt = 0x80000000u;

// From the apps, by the receiving thread, and as SceShell carries a queue
// on, by the music thread:
int g_active;
uint32_t g_owner;
int g_ownerApp = -1;
SceUInt64 g_activatedUs;  // the last SC_CMD_ROW_ACTIVATE
int g_playing = -1;
uint16_t g_title[256], g_artist[256], g_album[256];
uint32_t g_artSerial;  // the cover at g_artPath, 0 for none
char g_artPath[200];   // the file g_artSerial names
int g_releaseDue;      // the row given back, to be left as SceShell had it
// The music thread's (startMusicThread), woken for a queue handed over, for a
// press while SceShell carries the queue on, and to stop that.
SceUID g_musicSema = -1;
// From SceShell's UI thread (onPress), atomic: a press on the row for the
// music thread while SceShell carries the queue on, SC_PRESS_*.
int g_carryPress;
// The row's owner's title, for its presses: written under g_lock as the row
// changes hands, read by onPress without it, as SceShell's UI thread never
// waits for the plugin's lock.
char g_pressTitle[12];

// The quick menu, by the watch thread:
int g_open;            // it is open
SceUInt64 g_openedUs;  // when it was seen open
int g_openSerial;      // opens so far; the row's tick sees a new one by it

// The row, by its tick (rowTick), on SceShell's UI thread: found anew each
// tick, so that nothing is kept of a row SceShell has since done away with.
paf::ui::Widget *g_rew, *g_play, *g_ff, *g_textTop, *g_textBottom, *g_noSongs;
paf::ui::Widget *g_iconRew, *g_iconPlay, *g_iconFf, *g_iconPause, *g_headingIcon;
uint32_t g_topHash, g_bottomHash;  // the lines as this plugin set them
int g_tickOpenSerial;              // the open this tick's state is for
SceUInt64 g_lastTickUs;
int g_applied;    // the row is ours this time it is open
int g_mainTicks;  // times SceShell's UI thread has called mainTick
int g_fallback;   // it never has: the watch thread ticks the row itself
// Once a boot: PAF's widget layout is its headers' (checkLayout).
int g_layoutChecked, g_layoutOk;

// What became of the row in an open of the quick menu, logged as it closes
// (noteClosed): the first opens of a boot, and the first after the row
// changes hands, when SceShell kept the song hidden (build 2049).
struct OpenNote {
    int logIt;       // to be logged as it closes
    int foundMs;     // the row's buttons first found, -1 if never
    int takenMs;     // the row taken, -1 if not
    uint32_t owner;  // whose it was taken for
    int ownerApp;
    // What the plugin put back or changed after taking it.
    int buttons, lines, shown, playIcon, cover, lit;
    int firstPutMs;  // the first of those, -1 if none
    int snapped;     // the lines as they were 1 s in, in snap
    char snap[220];
};
OpenNote g_note;
// Opens still to be logged: the first of a boot, and a few more each time the
// row changes hands (notesWanted).
int g_notesLeft = 6;

// Ms the quick menu has been open, at now: a 32-bit division (splitUs has
// why), an open of over an hour reading as 71 minutes.
int msOpen(SceUInt64 now) {
    const SceUInt64 us = now - g_openedUs;
    return us > 0xFFFFFFFFull ? (int)(0xFFFFFFFFu / 1000u) : (int)((uint32_t)us / 1000u);
}

// The row has changed hands: its next opens are logged. Under g_lock.
void notesWanted() {
    if (g_notesLeft < 3) g_notesLeft = 3;
}

void lock() {
    sceKernelLockMutex(g_lock, 1, NULL);
}
void unlock() {
    sceKernelUnlockMutex(g_lock, 1);
}

// An app's name for the log, or "?". Under g_lock.
const char* appName(int i) {
    const App* a = appAt(i);
    return a ? a->name : "?";
}

const char* ownerName(uint32_t owner, int app) {
    return owner == kOwnerShell ? "SceShell" : appName(app);
}

// Members PAF's headers keep protected: read, and a dimmed icon's alphas
// written, only once the layout has been checked (checkLayout).
struct WidgetPeek : paf::ui::Widget {
    static WidgetPeek* of(paf::ui::Widget* w) {
        return static_cast<WidgetPeek*>(w);
    }
    static const paf::math::v4& size(paf::ui::Widget* w) {
        return of(w)->m_size;
    }
    // Its meta alpha, and the product with its parents' that it is drawn
    // with, as Widget::SetMetaAlpha_core would set them. That export is not
    // one to count on: ScePaf-RE has it from a prototype's debug data, and a
    // NID missing on the console would keep the whole plugin from loading.
    static void setMetaAlpha(paf::ui::Widget* w, float alpha) {
        paf::ui::Widget* parent = w->GetParent();
        float parentSum = parent ? of(parent)->m_meta_a_sum : 1.0f;
        if (!(parentSum >= 0.0f && parentSum <= 1.0f)) parentSum = 1.0f;
        of(w)->m_meta_alpha = alpha;
        of(w)->m_meta_a_sum = parentSum * alpha;
    }
    // Its own alpha, between its meta alpha's product and its colour (0x1AC):
    // an int ScePaf-RE's headers leave unnamed (m_unk_1AC), a float its older
    // ones call alpha. The row's icons had 0.3 there in build 2026, with all
    // their other alphas at 1.0, and the buttons and their group 1.0.
    // ElevenMPV-A's app dims a page behind another with SetAlpha(0.3f).
    static uint32_t ownAlphaRaw(paf::ui::Widget* w) {
        return (uint32_t)of(w)->m_unk_1AC;
    }
    // As a float, or -1 if it is not one from 0 to 1.
    static float ownAlpha(paf::ui::Widget* w) {
        const uint32_t raw = ownAlphaRaw(w);
        float f;
        sceClibMemcpy(&f, &raw, sizeof(f));
        return f >= 0.0f && f <= 1.0f ? f : -1.0f;
    }
    static void setOwnAlpha(paf::ui::Widget* w, float f) {
        uint32_t raw;
        sceClibMemcpy(&raw, &f, sizeof(raw));
        of(w)->m_unk_1AC = (int32_t)raw;
    }
};
struct ButtonPeek : paf::ui::ButtonBase {
    static int state(paf::ui::ButtonBase* b) {
        return (int)static_cast<ButtonPeek*>(b)->m_btn_state;
    }
};
// What a widget is drawn with (Widget::GetDrawObj, a virtual call): PAF's
// graph::DrawObj has a colour of its own besides the widget's. Its layout
// is the headers' if its size reads as its widget's (drawLayoutOk), as every
// one did in build 2024. Its visible bit read 0 for all of them, the row's
// heading too, so it is not read.
struct DrawPeek : paf::graph::DrawObj {
    static DrawPeek* of(paf::graph::DrawObj* d) {
        return static_cast<DrawPeek*>(d);
    }
    static uint32_t color(paf::graph::DrawObj* d) {
        return of(d)->m_color;
    }
    static void setColor(paf::graph::DrawObj* d, uint32_t color) {
        of(d)->m_color = color;
    }
    static int size(paf::graph::DrawObj* d, int i) {
        return (int)reinterpret_cast<const float*>(&of(d)->m_size)[i];
    }
};

// A vector's x or y, rounded: vitasdk-paf-component's v4 keeps its four floats
// to itself.
int coord(const paf::math::v4& v, int i) {
    return (int)reinterpret_cast<const float*>(&v)[i];
}

// ── Textures ────────────────────────────────────────────────────────────────
// An intrusive_ptr to a texture that the plugin fills itself and never
// destroys: setting it adds no reference and nothing takes one away, so it
// calls neither Surface::AddRef nor Release, imports the plugin does without.
struct TexPtr : paf::intrusive_ptr<paf::graph::Surface> {
    void set(paf::graph::Surface* s) {
        m_ptr = s;
    }
};
union TexRef {
    TexPtr ptr;
    explicit TexRef(paf::graph::Surface* s) : ptr() {
        ptr.set(s);
    }
    ~TexRef() {}
};


// A widget's texture, with the reference GetTexture adds kept for good: the
// plugin's own hold on it, so it outlives any row SceShell makes.
paf::graph::Surface* holdTexture(paf::ui::Widget* w) {
    TexRef ref(NULL);
    w->GetTexture(ref.ptr);
    return ref.ptr.get();
}

// A widget's texture in its first draw object, to compare with: the
// reference GetTexture adds is dropped again without ever freeing the
// texture from this thread, should SceShell have let go of it meanwhile.
paf::graph::Surface* currentTexture(paf::ui::Widget* w) {
    TexRef ref(NULL);
    w->GetTexture(ref.ptr);
    paf::graph::Surface* s = ref.ptr.get();
    if (s) s->UnsafeRelease();
    return s;
}

// Gives a widget a texture; the widget adds a reference of its own.
void giveTexture(paf::ui::Widget* w, paf::graph::Surface* s) {
    TexRef ref(s);
    w->SetTexture(ref.ptr);
}

// ── The row ─────────────────────────────────────────────────────────────────
paf::ui::Widget* find(uint32_t hash) {
    return g_impose->FindChild(paf::IDParam(hash), 0);
}

// The row's widgets, by their ids: on its tick. true when its buttons are
// there; they come a moment after the quick menu.
bool findRow() {
    g_rew = find(kHashRew);
    g_play = find(kHashPlay);
    g_ff = find(kHashFf);
    g_textTop = find(kHashTextTop);
    g_textBottom = find(kHashTextBottom);
    g_noSongs = find(kHashNoSongs);
    g_iconRew = find(kHashIconRew);
    g_iconPlay = find(kHashIconPlay);
    g_iconFf = find(kHashIconFf);
    g_iconPause = find(kHashIconPause);
    g_headingIcon = find(kHashHeadingIcon);
    return g_rew && g_play && g_ff;
}

void onPress(int32_t type, paf::ui::Handler* self, paf::ui::Event* e, void* data) {
    (void)type;
    (void)self;
    (void)e;
    const uint32_t press = (uint32_t)(uintptr_t)data;
    const char* name = press == SC_PRESS_PLAY_PAUSE ? "play/pause"
                     : press == SC_PRESS_NEXT      ? "next"
                                                   : "previous";
    if (!g_active) {
        logLine("press: %s, with the row given back; dropped", name);
        return;
    }
    // SceShell's own music: for the music thread, which is woken for it.
    if (__atomic_load_n(&g_owner, __ATOMIC_SEQ_CST) == kOwnerShell) {
        __atomic_store_n(&g_carryPress, (int)press, __ATOMIC_SEQ_CST);
        if (g_musicSema >= 0) sceKernelSignalSema(g_musicSema, 1);
        return;
    }
    // For the app that owns the row, by its title: one app runs at a time,
    // but a press meant for one that has since gone is not another's.
    ScPress msg;
    sceClibMemset(&msg, 0, sizeof(msg));
    msg.press = press;
    sceClibMemcpy(msg.titleId, g_pressTitle, sizeof(msg.titleId) - 1);
    // On SceShell's own thread: logged only when it goes wrong. The app logs
    // the presses it takes.
    const int rc = sceKernelTrySendMsgPipe(g_fromPlugin, &msg, sizeof(msg),
                                           kPipeModeFull, NULL);
    if (rc < 0) logLine("press: %s, not sent (0x%08X): nobody reading", name, (unsigned)rc);
}

// A button's switched-on bit and state (ButtonBase::ST_BTN_*: 0 normal,
// 1 dimmed), or -1 for both while the layout is not known to be the headers'.
int buttonEnabled(paf::ui::ButtonBase* b) {
    return g_layoutOk ? (int)b->IsEnable() : -1;
}
int buttonState(paf::ui::ButtonBase* b) {
    return g_layoutOk ? ButtonPeek::state(b) : -1;
}

// ── Lighting the buttons and icons ──────────────────────────────────────────
// SceShell draws the row's buttons and their icons faded with no music of its
// own, and opaque and white with its Music app playing (screenshots, build
// 2023), with every colour, alpha, texture and shader the timeline reads alike
// either way (builds 2023 to 2025). Build 2026's byte dumps found the icons'
// own alpha (WidgetPeek::ownAlpha) at 0.3. A button or icon whose own alpha,
// meta alpha or colour is faded has its colour set (SetColor_core) and those
// alphas set to 1; a faded draw object's colour is set to kLitColor. What was
// changed is put back when the row is SceShell's again, where it is still
// the plugin's. Colours have their alpha in the top byte on the console,
// whatever the headers' GetColor says: the quick menu's background reads
// FF282828, the volume slider 4CCCCCCC as it fades in, the song's lines draw
// with D8FFFFFF and CCFFFFFF (build 2026). kLitColor is white with a last
// byte of 0xFE nobody would see: kept apart from a white SceShell sets.
const uint32_t kLitColor = 0xFFFFFFFE;
const int kLitMax = 6;

struct Lit {
    paf::ui::Widget* w;
    paf::graph::DrawObj* d;  // its draw object, if its colour was set
    uint32_t was;            // that colour as SceShell had it
    float alphaWas;          // its own alpha as SceShell had it, -1 if left
};
Lit g_lit[kLitMax];  // what the plugin lit, while it is to be put back
int g_litCount;

// Whether a draw object's layout is the headers' as far as its colour goes:
// its size reads as its widget's, which sits just before it.
bool drawLayoutOk(paf::ui::Widget* w, paf::graph::DrawObj* d) {
    const paf::math::v4& size = WidgetPeek::size(w);
    const int dw = DrawPeek::size(d, 0) - coord(size, 0);
    const int dh = DrawPeek::size(d, 1) - coord(size, 1);
    return dw >= -1 && dw <= 1 && dh >= -1 && dh <= 1 && coord(size, 0) > 0;
}

// A faded white or grey: its three colours alike, not opaque white, not
// nearly transparent, and not the plugin's own.
bool faded(uint32_t c) {
    const uint32_t a = c >> 24, x = (c >> 16) & 0xFF, y = (c >> 8) & 0xFF, z = c & 0xFF;
    return x == y && y == z && a >= 0x10 && c != 0xFFFFFFFF && c != kLitColor;
}

// The record of a widget the plugin lit, made the first time; NULL if full.
Lit* litEntry(paf::ui::Widget* w) {
    for (int i = 0; i < g_litCount; i++)
        if (g_lit[i].w == w) return &g_lit[i];
    if (g_litCount == kLitMax) return NULL;
    Lit& l = g_lit[g_litCount++];
    l.w = w;
    l.d = NULL;
    l.was = 0;
    l.alphaWas = -1.0f;
    return &l;
}

// One button or icon lit; true when it changed anything. A widget hidden by
// its colour or an alpha is left so.
bool light(paf::ui::Widget* w) {
    if (!w || !g_layoutOk) return false;
    const uint32_t color = w->GetColor32();
    const float meta = w->GetMetaAlpha();
    const float own = WidgetPeek::ownAlpha(w);
    if ((color >> 24) < 0x10 || meta < 0.05f || (own >= 0.0f && own < 0.05f)) return false;
    paf::graph::DrawObj* d = w->GetDrawObj(0);
    const uint32_t drawn = d ? DrawPeek::color(d) : 0;
    const bool dimWidget = color != 0xFFFFFFFF || meta < 0.99f;
    const bool dimOwn = own >= 0.0f && own < 0.99f;
    const bool dimDrawn = d && faded(drawn);
    if (!dimWidget && !dimOwn && !dimDrawn) return false;
    if (meta < 0.99f) WidgetPeek::setMetaAlpha(w, 1.0f);
    const paf::math::v4 c = white();
    w->SetColor_core(c);
    // Its own alpha after SetColor_core, should that call set it too.
    if (dimOwn) {
        Lit* l = litEntry(w);
        if (l && l->alphaWas < 0.0f) l->alphaWas = own;
        WidgetPeek::setOwnAlpha(w, 1.0f);
    }
    // A draw object that was faded is set by hand even if SetColor_core lit
    // it: kLitColor marks it as the plugin's, to be put back. One whose
    // layout is not the headers' is left faded.
    if (dimDrawn) {
        Lit* l = drawLayoutOk(w, d) ? litEntry(w) : NULL;
        if (l) {
            if (!l->d) {
                l->d = d;
                l->was = drawn;
            }
            DrawPeek::setColor(d, kLitColor);
        }
    }
    return true;
}

bool lightAll() {
    bool changed = light(g_iconRew);
    changed |= light(g_iconPlay);
    changed |= light(g_iconFf);
    changed |= light(g_rew);
    changed |= light(g_play);
    changed |= light(g_ff);
    return changed;
}

// Puts back the own alphas and draw objects' colours the plugin changed,
// where they are still its: with the row's widgets found this tick
// (findRow). They must be the widgets lit, so a row SceShell made anew is
// left alone.
void unlightAll() {
    if (!g_litCount) return;
    for (int i = 0; i < g_litCount; i++) {
        const Lit& l = g_lit[i];
        paf::ui::Widget* const found[6] = {g_iconRew, g_iconPlay, g_iconFf, g_rew, g_play, g_ff};
        bool same = false;
        for (int j = 0; j < 6 && !same; j++) same = found[j] == l.w;
        if (!same) continue;
        if (l.alphaWas >= 0.0f && WidgetPeek::ownAlpha(l.w) >= 0.99f)
            WidgetPeek::setOwnAlpha(l.w, l.alphaWas);
        if (l.d && l.w->GetDrawObj(0) == l.d && DrawPeek::color(l.d) == kLitColor)
            DrawPeek::setColor(l.d, l.was);
    }
    g_litCount = 0;
}

// ── The play/pause icon ─────────────────────────────────────────────────────
// While the app plays, the play icon is given the pause icon's texture, and
// its own back when paused or when the row is SceShell's again (build 2025).
paf::graph::Surface *g_playTex, *g_pauseTex;  // held for good (holdTexture)
int g_pauseShown;  // the play icon has the pause icon's texture

// Holds the two icons' textures, the first time the row is taken in a boot:
// SceShell's own then, before the plugin has changed either.
void holdPlayTextures() {
    if (g_playTex || !g_iconPlay || !g_iconPause) return;
    g_playTex = holdTexture(g_iconPlay);
    g_pauseTex = holdTexture(g_iconPause);
}

// The play icon as the app's music is, or as SceShell's own row had it:
// true when it changed it.
bool showPlaying(bool pause) {
    if (!g_iconPlay || !g_playTex || !g_pauseTex) return false;
    paf::graph::Surface* want = pause ? g_pauseTex : g_playTex;
    if (currentTexture(g_iconPlay) == want) return false;
    giveTexture(g_iconPlay, want);
    g_pauseShown = pause;
    return true;
}

// ── The cover ───────────────────────────────────────────────────────────────
// The song's cover, in place of the Music heading's icon while the row is an
// app's. The app writes it as a PNG (SC_ART_FMT, SC_ART_SIZE square) in its
// folder and names it by a serial (SC_CMD_ROW_ART), which g_artPath is set
// from as it comes; SceShell names the ones it shows as it carries a queue
// on (kShellArt) the same way. It is loaded with PAF's own
// decoder (Surface::Load, from a LocalFile), the calls ElevenMPV-A's app
// loads its covers with, into power_manage_plugin's texture pool. The icon
// keeps the only reference to it, so it goes when the icon gets its own back
// or SceShell does away with the row. The icon shows either its own texture,
// which the plugin holds, or the cover the plugin gave it last.
paf::graph::Surface* g_headingTex;  // the heading's own icon, held for good
uint32_t g_artShown;                // the cover given to the icon last, 0 for none
uint32_t g_artFailed;               // a cover that would not load, not tried again
int g_artLogged;

// The heading icon's own texture, held the first time it is seen.
void holdHeadingTexture() {
    if (g_headingTex || !g_headingIcon) return;
    g_headingTex = holdTexture(g_headingIcon);
}

// A cover's file as a texture, with one reference: the caller's.
paf::graph::Surface* loadArt(uint32_t serial) {
    if (!g_surfacePool) return NULL;
    (void)serial;
    char path[sizeof(g_artPath)];
    sceClibSnprintf(path, sizeof(path), "%s", g_artPath);
    int32_t err = 0;
    paf::common::SharedPtr<paf::LocalFile> file =
        paf::LocalFile::Open(path, SCE_O_RDONLY, 0, &err);
    if (err < 0 || !file.get()) {
        if (g_artLogged < 6) {
            g_artLogged++;
            logLine("art: %s would not open: 0x%08X", path, (unsigned)err);
        }
        return NULL;
    }
    // Surface::Load takes a SharedPtr<File>, which PAF's SharedPtr will not
    // make of a SharedPtr<LocalFile>: the same two pointers are passed as
    // one, as ElevenMPV-A's app does, copied into a SharedPtr<File> that is
    // never destroyed, so the file's count is the one `file` holds.
    union AsFile {
        paf::common::SharedPtr<paf::File> ptr;
        AsFile() {}
        ~AsFile() {}
    } asFile;
    static_assert(sizeof(asFile) == sizeof(file), "a SharedPtr is a SharedPtr");
    sceClibMemcpy(&asFile, &file, sizeof(asFile));
    // The texture it returns is built where it is wanted, so that nothing
    // takes its reference away.
    union Loaded {
        paf::intrusive_ptr<paf::graph::Surface> ptr;
        Loaded(paf::graph::SurfacePool* pool, paf::common::SharedPtr<paf::File>& f)
            : ptr(paf::graph::Surface::Load(pool, f, NULL)) {}
        ~Loaded() {}
    } loaded(g_surfacePool, asFile.ptr);
    return loaded.ptr.get();
}

// The heading's own icon on it again, if it has a cover. true when it
// changed it.
bool showOwnIcon() {
    g_artShown = 0;
    if (!g_headingIcon || !g_headingTex || currentTexture(g_headingIcon) == g_headingTex)
        return false;
    giveTexture(g_headingIcon, g_headingTex);
    return true;
}

// The heading's icon as the row's owner wants it: the cover named last, or
// its own. A cover is loaded again for an icon SceShell has made anew, which
// shows its own. true when it changed it.
bool showArt() {
    if (!g_headingIcon) return false;
    holdHeadingTexture();
    if (!g_headingTex) return false;
    const uint32_t want = g_artSerial;
    if (want == 0) return showOwnIcon();
    const bool own = currentTexture(g_headingIcon) == g_headingTex;
    if (!own && g_artShown == want) return false;
    if (g_artFailed == want) return own ? false : showOwnIcon();
    paf::graph::Surface* art = loadArt(want);
    if (!art) {
        g_artFailed = want;
        logLine("art: %u not loaded", (unsigned)want);
        return own ? false : showOwnIcon();
    }
    giveTexture(g_headingIcon, art);
    art->UnsafeRelease();  // the icon's reference is the one left
    g_artShown = want;
    return true;
}

// ── Switching the buttons on ────────────────────────────────────────────────
// Each button switched on and put in its normal state: PlaneButton has a
// ChangeButtonState of its own, which may also put the button's normal look
// on it. ElevenMPV-A's way, Enable(false), and Enable(true) alone left them
// dimmed (builds 2019 to 2022).
void switchOn(paf::ui::Widget* w) {
    paf::ui::ButtonBase* b = static_cast<paf::ui::ButtonBase*>(w);
    b->Enable(true);
    b->ChangeButtonState(paf::ui::ButtonBase::ST_BTN_NORMAL);
}

void switchOnAll() {
    switchOn(g_rew);
    switchOn(g_play);
    switchOn(g_ff);
    lightAll();
}

// While the row is ours: a button SceShell has switched off or dimmed again
// since, as its own set-up of the row may do after ours, is switched on
// again. true when it changed anything.
bool watchButtons() {
    if (!g_layoutOk) return false;
    paf::ui::Widget* const buttons[3] = {g_rew, g_play, g_ff};
    bool changed = false;
    for (int i = 0; i < 3; i++) {
        paf::ui::ButtonBase* b = static_cast<paf::ui::ButtonBase*>(buttons[i]);
        // State 1 is dimmed; 2 to 4 are a press under way, left alone.
        if (buttonEnabled(b) == 1 && buttonState(b) != 1) continue;
        switchOn(buttons[i]);
        changed = true;
    }
    return changed;
}

// ── The lines ───────────────────────────────────────────────────────────────
void setTexts() {
    if (!g_textTop || !g_textBottom) return;
    paf::wstring top((const wchar_t*)g_title);
    paf::wstring bottom((const wchar_t*)g_artist);
    if (g_artist[0] && g_album[0]) bottom += L" / ";
    bottom += (const wchar_t*)g_album;
    g_topHash = hashText((const uint16_t*)top.c_str());
    g_bottomHash = hashText((const uint16_t*)bottom.c_str());
    const paf::math::v4 c = white();
    g_textTop->SetString(top);
    g_textTop->SetColor_core(c);
    g_textBottom->SetString(bottom);
    g_textBottom->SetColor_core(c);
}

// While the row is ours: the lines as the song is, should SceShell have put
// other text or another colour on them, or the app sent another song. true
// when it changed anything.
bool keepTexts() {
    if (!g_textTop || !g_textBottom || !g_layoutOk) return false;
    paf::wstring top, bottom;
    g_textTop->GetString(top);
    g_textBottom->GetString(bottom);
    const uint32_t topColor = g_textTop->GetColor32(), bottomColor = g_textBottom->GetColor32();
    // What the lines should hold, the song as it now is.
    paf::wstring wantTop((const wchar_t*)g_title);
    paf::wstring wantBottom((const wchar_t*)g_artist);
    if (g_artist[0] && g_album[0]) wantBottom += L" / ";
    wantBottom += (const wchar_t*)g_album;
    const bool newSong = hashText((const uint16_t*)wantTop.c_str()) != g_topHash ||
                         hashText((const uint16_t*)wantBottom.c_str()) != g_bottomHash;
    if (!newSong && hashText((const uint16_t*)top.c_str()) == g_topHash &&
        hashText((const uint16_t*)bottom.c_str()) == g_bottomHash && topColor == 0xFFFFFFFF &&
        bottomColor == 0xFFFFFFFF)
        return false;
    setTexts();
    return true;
}

// A widget's own alpha set to want, should it be another; one that does not
// read as a float from 0 to 1 is left alone. true when it changed it.
bool setOwnAlphaIf(paf::ui::Widget* w, float want) {
    if (!w) return false;
    const float a = WidgetPeek::ownAlpha(w);
    if (a < 0.0f || (a > want - 0.01f && a < want + 0.01f)) return false;
    WidgetPeek::setOwnAlpha(w, want);
    return true;
}

// SceShell's own alpha for "There are no songs." from before showLines hid
// it, to put back with the row; below 0 when there is nothing to put back.
// It stayed hidden once the row was SceShell's again in log 48ccef45.
float g_noSongsOwnAlpha = -1.0f;

// While the row is ours: the song's lines shown, and "There are no songs."
// hidden, by their own alphas. SceShell fades the lines out as the quick menu
// opens when it sees no music of its own, which it did on the first open
// after VitaPlex began to play: the song showed only from the second (build
// 2049). true when it changed anything.
bool showLines() {
    if (!g_layoutOk) return false;
    bool changed = setOwnAlphaIf(g_textTop, kTopAlpha);
    changed |= setOwnAlphaIf(g_textBottom, kBottomAlpha);
    if (g_noSongs && g_noSongsOwnAlpha < 0.0f) {
        const float a = WidgetPeek::ownAlpha(g_noSongs);
        if (a > 0.01f && a <= 1.0f) g_noSongsOwnAlpha = a;  // what is changed below
    }
    changed |= setOwnAlphaIf(g_noSongs, 0.0f);
    return changed;
}

// "There are no songs." as SceShell had it before showLines hid it.
void putNoSongsBack() {
    if (g_noSongsOwnAlpha < 0.0f) return;
    if (g_noSongs && g_layoutOk) WidgetPeek::setOwnAlpha(g_noSongs, g_noSongsOwnAlpha);
    g_noSongsOwnAlpha = -1.0f;
}

// A line as it is, for the open's note: whose text it holds, its own alpha,
// and its draw object's colour.
int noteLine(char* out, int cap, const char* name, paf::ui::Widget* w, uint32_t ours) {
    if (!w) return sceClibSnprintf(out, cap, "%s none", name);
    paf::wstring s;
    w->GetString(s);
    const uint16_t* text = (const uint16_t*)s.c_str();
    const char* whose = !text || !text[0] ? "empty" : hashText(text) == ours ? "ours" : "other";
    const float a = g_layoutOk ? WidgetPeek::ownAlpha(w) : -1.0f;
    paf::graph::DrawObj* d = w->GetDrawObj(0);
    return sceClibSnprintf(out, cap, "%s %s, alpha %d%%, drawn %08X", name, whose,
                           a < 0.0f ? -1 : (int)(a * 100.0f + 0.5f),
                           d ? (unsigned)DrawPeek::color(d) : 0u);
}

// The lines and buttons as they are 1 s into the open, into the note.
void noteLines() {
    const int cap = (int)sizeof(g_note.snap);
    int n = 0;
    // Each part where the last ended, the length kept within the buffer: a
    // part cut short returns the length it would have had.
    auto clamp = [&](int wrote) { n = wrote < 0 ? n : n + wrote < cap - 1 ? n + wrote : cap - 1; };
    clamp(noteLine(g_note.snap, cap, "title", g_textTop, g_topHash));
    clamp(noteLine(g_note.snap + n, cap - n, "; artist", g_textBottom, g_bottomHash));
    clamp(noteLine(g_note.snap + n, cap - n, "; no-songs", g_noSongs, 0));
    if (g_layoutOk && g_rew && g_play && g_ff)
        clamp(sceClibSnprintf(g_note.snap + n, cap - n, "; buttons on %d%d%d",
                              buttonEnabled(static_cast<paf::ui::ButtonBase*>(g_rew)),
                              buttonEnabled(static_cast<paf::ui::ButtonBase*>(g_play)),
                              buttonEnabled(static_cast<paf::ui::ButtonBase*>(g_ff))));
    g_note.snapped = 1;
}

// Once a boot: whether PAF's widget layout is the one its headers have, as
// far as the members read here go. The play button must report the id it
// was found by, and be among its parent's children.
void checkLayout(paf::ui::Widget* play) {
    g_layoutChecked = 1;
    const uint32_t id = play->GetName().GetIDHash();
    paf::ui::Widget* parent = id == kHashPlay ? play->GetParent() : NULL;
    const int siblings = parent ? parent->GetChildrenNum() : -1;
    int found = 0;
    if (siblings > 0 && siblings <= 64)
        for (int i = 0; i < siblings && !found; i++) found = parent->GetChild(i) == play;
    g_layoutOk = found;
    if (!found)
        logLine("row: the play button reports id 0x%08X and its parent %p has %d children: "
                "the widget layout is not the headers', so nothing is read from it",
                (unsigned)id, (void*)parent, siblings);
}

// ── Taking, keeping and giving back the row ─────────────────────────────────
// Takes the row, the quick menu having been open kTakeAfterUs. Another of
// SceShell's overlays, such as the LiveArea's, reads as the quick menu open
// too, with no row's buttons to be found. true when taken.
bool takeRow(SceUInt64 now, bool found) {
    if (!found) return false;
    if (!g_layoutChecked) checkLayout(g_play);
    switchOnAll();
    holdPlayTextures();
    showPlaying(g_playing == 1);
    g_rew->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                            (void*)(uintptr_t)SC_PRESS_PREVIOUS);
    g_play->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                             (void*)(uintptr_t)SC_PRESS_PLAY_PAUSE);
    g_ff->SetEventCallback(paf::ui::ButtonBase::CB_BTN_DECIDE, onPress,
                           (void*)(uintptr_t)SC_PRESS_NEXT);
    setTexts();
    showLines();
    showArt();
    g_applied = 1;
    g_note.takenMs = msOpen(now);
    g_note.owner = g_owner;
    g_note.ownerApp = g_ownerApp;
    return true;
}

// While the row is ours and the quick menu open, ten times a second: puts
// back what SceShell has changed since, and shows what the app has sent
// since, each noted for the open's log.
void keepRow(SceUInt64 now) {
    const int buttons = watchButtons(), lines = keepTexts(), shown = showLines(),
              playIcon = showPlaying(g_playing == 1), cover = showArt(), lit = lightAll();
    g_note.buttons += buttons;
    g_note.lines += lines;
    g_note.shown += shown;
    g_note.playIcon += playIcon;
    g_note.cover += cover;
    g_note.lit += lit;
    if (g_note.firstPutMs < 0 && (buttons || lines || shown || playIcon || cover || lit))
        g_note.firstPutMs = msOpen(now);
}

// The row given back with the quick menu open: it is left as SceShell's own
// set-up had it with no music of its own (build 2023's log): the lines empty,
// the buttons off and faded, play showing ▶ and the heading its own icon.
void releaseRow() {
    unlightAll();
    if (g_pauseShown) showPlaying(false);
    showOwnIcon();
    putNoSongsBack();
    const paf::wstring empty;
    if (g_textTop) g_textTop->SetString(empty);
    if (g_textBottom) g_textBottom->SetString(empty);
    paf::ui::Widget* const buttons[3] = {g_rew, g_play, g_ff};
    for (int i = 0; i < 3; i++) static_cast<paf::ui::ButtonBase*>(buttons[i])->Enable(false);
    g_applied = 0;
}

// The row SceShell's, with something of the plugin's left on it since it was
// last ours. true when it changed anything.
bool restoreRow() {
    if (!g_litCount && !g_pauseShown && !g_artShown && g_noSongsOwnAlpha < 0.0f) return false;
    putNoSongsBack();
    unlightAll();
    // Its play icon is put back only with the button off: with it on,
    // SceShell's own music is in the row, and ⏸ may be SceShell's.
    if (g_pauseShown && !static_cast<paf::ui::ButtonBase*>(g_play)->IsEnable()) showPlaying(false);
    g_pauseShown = 0;
    if (g_artShown) showOwnIcon();
    return true;
}

// ── The row's tick ──────────────────────────────────────────────────────────
// Each time round SceShell's UI loop (mainTick), under g_lock; the row itself
// is looked at ten times a second while the quick menu is open.
void rowTick(SceUInt64 now) {
    if (g_openSerial != g_tickOpenSerial) {
        // Opened since the last tick: what was kept of the last open goes.
        g_tickOpenSerial = g_openSerial;
        g_applied = 0;
        g_lastTickUs = 0;
    }
    if (!g_open) {
        g_applied = 0;
        return;
    }
    if (now - g_lastTickUs < kTickUs) return;
    g_lastTickUs = now;
    const bool found = findRow();
    if (!found) g_applied = 0;  // a row SceShell has done away with is not ours
    if (found && g_note.foundMs < 0) g_note.foundMs = msOpen(now);
    if (g_releaseDue) {
        g_releaseDue = 0;
        if (g_applied) releaseRow();
    }
    if (g_active && !g_applied && now - g_openedUs >= kTakeAfterUs)
        takeRow(now, found);
    else if (g_active && g_applied)
        keepRow(now);
    else if (!g_active && found)
        restoreRow();
    if (g_note.logIt && !g_note.snapped && found && now - g_openedUs >= 1000000u) noteLines();
}

// PAF's main thread call list's: SceShell's UI thread, each time round its
// loop. It never waits for the plugin's lock, so SceShell is never held up.
void mainTick(void* data) {
    (void)data;
    g_mainTicks++;
    if (g_fallback) return;
    if (sceKernelTryLockMutex(g_lock, 1) < 0) return;
    rowTick(sceKernelGetSystemTimeWide());
    unlock();
}

// ── SceShell's music service ────────────────────────────────────────────────
// VitaPlex's background player, up to build 2045, played through SceShell's
// music service from a process of its own, which the system ended as an app
// with extended memory started. Code in SceShell drives the same service
// (build 2032's test, docs/DESIGN_NOTES.md), so the queue is played from here
// once the app has gone. The service's client object (sceShellSvcGetSvcObj)
// is found with taiHEN, so that the plugin does not import it and takes
// whatever SceShell has: in build 2032 SceShell's own module SceShellSvc had
// it. It is spoken to with libShellAudio's calls (lib/libshellaudio), as the
// background player made them. The BGM port is left to the service, which
// took it for SceShell itself while it played for SceShell (build 2032): the
// libraries that take it (SceAppMgr) and that name a module (SceModulemgr)
// are ones no plugin known to load in SceShell imports, and one SceShell
// refused would keep the whole plugin out. All of it on a thread of its own,
// never SceShell's UI thread, which may be the one to answer the calls.
const uint32_t kShellSvcLibrary = 0x773242C7;  // SceShellSvc (src/utils's shellsvc_stub.S)
const uint32_t kGetSvcObj = 0xB31E7F1C;        // sceShellSvcGetSvcObj
const char* const kShellSvcPath = "vs0:sys/external/libshellsvc.suprx";
// The events libShellAudio sends (ShellAudio.h's SCE_MUSIC_EVENTID_*).
const int kEventPlay = 1, kEventStop = 2, kEventSeek = 0x11;

typedef void* (*GetSvcObj)(void);
// A method's arguments, in and out (libShellAudio's SceShellSvcAudioCustomParams).
struct SvcParams {
    const void* p[4][2];  // pointer, size
};
// The client's invokeSyncMethod, at 0x14 in its vtable.
typedef int (*SvcInvoke)(void* obj, int method, SvcParams* in, int inCount, int* res,
                         SvcParams* out, int outCount);
// Who the service is talking to: for the music player service of type 0,
// as the background player started it, 1 and 8.
struct SvcTracking {
    int unk;
    int t1, t2;
};

GetSvcObj g_getSvcObj;
int g_musicThread;       // its thread started

void setArg(SvcParams* a, int i, const void* p, uint32_t size) {
    a->p[i][0] = p;
    a->p[i][1] = (const void*)(uintptr_t)size;
}

// A method of SceShell's music service, as libShellAudio calls it. The call's
// code if it failed, else the service's answer.
int svcCall(int method, SvcParams* in, int inCount, SvcParams* out, int outCount) {
    void* obj = g_getSvcObj ? g_getSvcObj() : NULL;
    if (!obj) return -1;
    SvcInvoke invoke = (*(SvcInvoke* const*)obj)[5];
    int res = 0;
    const int ret = invoke(obj, method, in, inCount, &res, out, outCount);
    return ret != 0 ? ret : res;
}

const SvcTracking kMusicPlayer = {0, 1, 8};

int musicEvent(int event) {
    const int command[2] = {event, 0};
    SvcParams in;
    sceClibMemset(&in, 0, sizeof(in));
    setArg(&in, 0, command, sizeof(command));
    setArg(&in, 1, &kMusicPlayer, sizeof(kMusicPlayer));
    return svcCall(0x30006, &in, 2, NULL, 0);
}

int musicOpen(const char* path) {
    char opt[16];
    sceClibMemset(opt, 0, sizeof(opt));
    SvcParams in;
    sceClibMemset(&in, 0, sizeof(in));
    setArg(&in, 0, &kMusicPlayer, sizeof(kMusicPlayer));
    setArg(&in, 1, path, (uint32_t)sceClibStrnlen(path, 0x401));
    setArg(&in, 2, opt, sizeof(opt));
    return svcCall(0x30004, &in, 3, NULL, 0);
}

// Its tracking alone: the service's start (0x30000) and end (0x30001).
int musicTracked(int method) {
    SvcParams in;
    sceClibMemset(&in, 0, sizeof(in));
    setArg(&in, 0, &kMusicPlayer, sizeof(kMusicPlayer));
    return svcCall(method, &in, 1, NULL, 0);
}

// The play status (0x30007): the state at 0x10, the clock in ms at 0x2C.
int musicStatus(int* state, uint32_t* timeMs) {
    uint32_t buf[0x70 / 4];
    sceClibMemset(buf, 0, sizeof(buf));
    SvcParams in, out;
    sceClibMemset(&in, 0, sizeof(in));
    sceClibMemset(&out, 0, sizeof(out));
    setArg(&in, 0, &kMusicPlayer, sizeof(kMusicPlayer));
    setArg(&out, 0, buf, sizeof(buf));
    const int rc = svcCall(0x30007, &in, 1, &out, 1);
    *state = (int)buf[0x10 / 4];
    *timeMs = buf[0x2C / 4];
    return rc;
}

// Where the next seek event takes the song (0x3000B), as libShellAudio's
// sceMusicPlayerServiceSetSeekTime sets it.
int musicSeekTime(uint32_t ms) {
    SvcParams in;
    sceClibMemset(&in, 0, sizeof(in));
    setArg(&in, 0, &ms, sizeof(ms));
    setArg(&in, 1, &kMusicPlayer, sizeof(kMusicPlayer));
    return svcCall(0x3000B, &in, 2, NULL, 0);
}

// The track info (0x3001B): the length the shell gives its song, in ms at 8,
// of a buffer libShellAudio sizes at 0x1810.
uint32_t g_trackInfo[0x1810 / 4];  // the music thread's
int musicLength(uint32_t* ms) {
    sceClibMemset(g_trackInfo, 0, sizeof(g_trackInfo));
    SvcParams in, out;
    sceClibMemset(&in, 0, sizeof(in));
    sceClibMemset(&out, 0, sizeof(out));
    setArg(&in, 0, &kMusicPlayer, sizeof(kMusicPlayer));
    setArg(&out, 0, g_trackInfo, sizeof(g_trackInfo));
    const int rc = svcCall(0x3001B, &in, 1, &out, 1);
    const int len = (int)g_trackInfo[8 / 4];
    *ms = rc >= 0 && len > 0 ? (uint32_t)len : 0;
    return rc;
}

// sceShellSvcGetSvcObj in a module of SceShell's by the name of either library
// libshellsvc.suprx exports. 0 when found.
int findGetSvcObj(uintptr_t* fn, const char** found) {
    const char* const names[2] = {"SceShellSvc", "SceShellUtil"};
    int rc = -1;
    for (int i = 0; i < 2 && rc < 0; i++) {
        rc = scGetModuleExportFunc(names[i], kShellSvcLibrary, kGetSvcObj, fn);
        *found = names[i];
    }
    return rc;
}

// The client object's maker, from SceShell's own SceShellSvc, else from the
// library loaded into SceShell now. `who` begins each line it logs. true when
// found.
bool findSvcObj(const char* who) {
    if (g_getSvcObj) return true;
    uintptr_t fn = 0;
    const char* name = "";
    int rc = findGetSvcObj(&fn, &name);
    if (rc < 0) {
        int status = 0;
        const SceUID mod = sceKernelLoadStartModule(kShellSvcPath, 0, NULL, 0, NULL, &status);
        logLine("%s: %s loaded into SceShell: 0x%08X, its start 0x%08X", who, kShellSvcPath,
                (unsigned)mod, (unsigned)status);
        if (mod < 0) return false;
        rc = findGetSvcObj(&fn, &name);
        logLine("%s: sceShellSvcGetSvcObj in it: 0x%08X%s%s", who, (unsigned)rc,
                rc >= 0 ? ", in module " : " (not under either library's name)",
                rc >= 0 ? name : "");
        if (rc < 0) return false;
    }
    g_getSvcObj = (GetSvcObj)fn;
    return true;
}

// ── Sony's SSL and HTTP libraries, as SceShell has them ─────────────────────
// What SceShell fetches the queue's songs that are not on the memory card
// with (build 2040's test found them working there). Found with taiHEN as the
// music service is: imported, a library SceShell did not have would keep the
// whole plugin out. What holds for all of SceShell is left as it is: the TLS
// options and certificates, and the libraries' start, ended only when the
// plugin began them. The token goes as a header, never logged.
struct LibFn {
    const char* name;
    uint32_t nid;
    uintptr_t fn;  // found, or 0
};
const uint32_t kSslLibrary = 0xCAF5F2E9;   // SceSsl, in module SceLibSsl
const uint32_t kHttpLibrary = 0xE8F15CDE;  // SceHttp, in module SceLibHttp
enum { kSslInit, kSslTerm, kSslFns };
LibFn g_ssl[kSslFns] = {{"sceSslInit", 0x3C733316, 0}, {"sceSslTerm", 0x03CE6E3A, 0}};
enum {
    kHttpInit,
    kHttpTerm,
    kHttpCreateTemplate,
    kHttpDeleteTemplate,
    kHttpCreateConnection,
    kHttpDeleteConnection,
    kHttpCreateRequest,
    kHttpDeleteRequest,
    kHttpAddHeader,
    kHttpSend,
    kHttpStatus,
    kHttpLength,
    kHttpRead,
    kHttpResolveTimeOut,
    kHttpConnectTimeOut,
    kHttpSendTimeOut,
    kHttpRecvTimeOut,
    kHttpsSslError,
    kHttpFns
};
LibFn g_http[kHttpFns] = {
    {"sceHttpInit", 0x214926D9, 0},
    {"sceHttpTerm", 0xC9076666, 0},
    {"sceHttpCreateTemplate", 0x62241DAB, 0},
    {"sceHttpDeleteTemplate", 0xEC85ECFB, 0},
    {"sceHttpCreateConnectionWithURL", 0xC616C200, 0},
    {"sceHttpDeleteConnection", 0xF0F65C15, 0},
    {"sceHttpCreateRequestWithURL", 0xBD5DA1D0, 0},
    {"sceHttpDeleteRequest", 0x3D3D29AD, 0},
    {"sceHttpAddRequestHeader", 0x7B51B122, 0},
    {"sceHttpSendRequest", 0x9CA58B99, 0},
    {"sceHttpGetStatusCode", 0x27071691, 0},
    {"sceHttpGetResponseContentLength", 0xF580D304, 0},
    {"sceHttpReadData", 0x7EDE3979, 0},
    {"sceHttpSetResolveTimeOut", 0x8455B5B3, 0},
    {"sceHttpSetConnectTimeOut", 0x237CA86E, 0},
    {"sceHttpSetSendTimeOut", 0x8AE3F008, 0},
    {"sceHttpSetRecvTimeOut", 0x94BF196E, 0},
    {"sceHttpsGetSslError", 0x2B79BDE0, 0},
};
// As psp2/net/http.h has them.
typedef int (*FnUnsigned)(unsigned);
typedef int (*FnVoid)(void);
typedef int (*FnId)(int);
typedef int (*FnIdUnsigned)(int, unsigned);
typedef int (*FnTemplate)(const char*, int, int);
typedef int (*FnConnection)(int, const char*, int);
typedef int (*FnRequest)(int, int, const char*, unsigned long long);
typedef int (*FnHeader)(int, const char*, const char*, unsigned);
typedef int (*FnSend)(int, const void*, unsigned);
typedef int (*FnStatus)(int, int*);
typedef int (*FnLength)(int, unsigned long long*);
typedef int (*FnRead)(int, void*, unsigned);
typedef int (*FnSslError)(int, int*, unsigned*);
template <typename F>
F httpFn(int i) {
    return (F)g_http[i].fn;
}
const int kHttpVersion11 = 2;                    // SCE_HTTP_VERSION_1_1
const int kHttpGet = 0;                          // SCE_HTTP_METHOD_GET
const int kHttpPost = 1;                         // SCE_HTTP_METHOD_POST
const unsigned kHeaderOverwrite = 0;             // SCE_HTTP_HEADER_OVERWRITE
const int kHttpAlreadyInited = (int)0x80431020;  // SCE_HTTP_ERROR_ALREADY_INITED
const int kSslAlreadyInited = (int)0x80435020;   // what SceShell's gave (build 2040)
// The pools, should the plugin be what starts the libraries.
const unsigned kSslPool = 256 * 1024, kHttpPool = 64 * 1024;

// sceHttpCreateRequestWithURL2, which takes the method by name, for the
// methods the other has no number for (PATCH): looked for on its own, so
// that a firmware without it keeps everything else.
LibFn g_httpRequest2 = {"sceHttpCreateRequestWithURL2", 0x1B6B7983, 0};
typedef int (*FnRequest2)(int, const char*, const char*, unsigned long long);

// A library's functions, from SceShell's module of it, else from its file
// loaded into SceShell now. `who` begins each line it logs. true when every
// one is found.
bool findLibrary(const char* who, const char* module, uint32_t libnid, const char* path,
                 LibFn* fns, int count) {
    uintptr_t fn = 0;
    int rc = scGetModuleExportFunc(module, libnid, fns[0].nid, &fn);
    if (rc < 0) {
        int status = 0;
        const SceUID mod = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &status);
        logLine("%s: %s loaded into SceShell: 0x%08X, its start 0x%08X", who, path,
                (unsigned)mod, (unsigned)status);
        if (mod < 0) return false;
    }
    bool all = true;
    for (int i = 0; i < count; i++) {
        fn = 0;
        rc = scGetModuleExportFunc(module, libnid, fns[i].nid, &fn);
        fns[i].fn = rc >= 0 ? fn : 0;
        if (rc < 0) {
            all = false;
            logLine("%s: %s not in %s: 0x%08X", who, fns[i].name, module, (unsigned)rc);
        }
    }
    return all;
}

// Both libraries' functions, for `who`.
bool findLibraries(const char* who) {
    const bool all =
        findLibrary(who, "SceLibSsl", kSslLibrary, "vs0:sys/external/libssl.suprx", g_ssl,
                    kSslFns) &&
        findLibrary(who, "SceLibHttp", kHttpLibrary, "vs0:sys/external/libhttp.suprx", g_http,
                    kHttpFns);
    if (all) {
        uintptr_t fn = 0;
        const int rc = scGetModuleExportFunc("SceLibHttp", kHttpLibrary, g_httpRequest2.nid, &fn);
        g_httpRequest2.fn = rc >= 0 ? fn : 0;
        if (rc < 0) logLine("%s: %s not in SceLibHttp (0x%08X): no PATCH", who,
                            g_httpRequest2.name, (unsigned)rc);
    }
    return all;
}

// Both libraries started, unless SceShell has started them: their starts'
// codes, which say what is to be ended after (httpEnd). A start that failed
// otherwise than as begun already is logged. true when the HTTP library can
// be used. The fetch, the server's and the download threads each start them
// and end them as they go, and may run at once: the first to start them
// starts them, and the last to end ends them, so that none ends them under
// another (g_netLock).
SceUID g_netLock = -1;
// addHeaders' buffers, as the fetch, server and download threads may all
// send at once.
SceUID g_netHeadersLock = -1;
int g_netUsers;              // under g_netLock: threads between start and end
int g_netRcSsl, g_netRcHttp;  // under g_netLock: the first's starts
bool httpUsable(int rcHttp) {
    return rcHttp >= 0 || rcHttp == kHttpAlreadyInited;
}
bool httpStart(const char* who, int* rcSsl, int* rcHttp) {
    sceKernelLockMutex(g_netLock, 1, NULL);
    if (g_netUsers == 0) {
        g_netRcSsl = ((FnUnsigned)g_ssl[kSslInit].fn)(kSslPool);
        if (g_netRcSsl < 0 && g_netRcSsl != kSslAlreadyInited)
            logLine("%s: SSL library not started: 0x%08X", who, (unsigned)g_netRcSsl);
        g_netRcHttp = httpFn<FnUnsigned>(kHttpInit)(kHttpPool);
        if (!httpUsable(g_netRcHttp)) {
            logLine("%s: HTTP library not started: 0x%08X", who, (unsigned)g_netRcHttp);
            if (g_netRcSsl >= 0) ((FnVoid)g_ssl[kSslTerm].fn)();
            g_netRcSsl = -1;
        }
    }
    *rcSsl = g_netRcSsl;
    *rcHttp = g_netRcHttp;
    const bool usable = httpUsable(g_netRcHttp);
    if (usable) g_netUsers++;
    sceKernelUnlockMutex(g_netLock, 1);
    return usable;
}

// What httpStart started, ended once no thread uses it. Called with the codes
// it gave, whether or not it was called: nothing is ended for a start that
// failed or never was.
void httpEnd(int rcSsl, int rcHttp) {
    (void)rcSsl;
    if (!httpUsable(rcHttp)) return;
    sceKernelLockMutex(g_netLock, 1, NULL);
    if (g_netUsers > 0 && --g_netUsers == 0) {
        if (g_netRcHttp >= 0) httpFn<FnVoid>(kHttpTerm)();
        if (g_netRcSsl >= 0) ((FnVoid)g_ssl[kSslTerm].fn)();
    }
    sceKernelUnlockMutex(g_netLock, 1);
}

// A template with the time-outs VitaPlex's build 2040 test found to do: 5 s
// to resolve and to connect, 10 s to send and to receive. Its id, or the code
// it failed with.
int httpTemplate(const char* who, bool quick = false) {
    const int tmpl = httpFn<FnTemplate>(kHttpCreateTemplate)("ShellCompanion", kHttpVersion11, 1);
    if (tmpl < 0) {
        logLine("%s: template 0x%08X", who, (unsigned)tmpl);
        return tmpl;
    }
    // quick: for pushes to remotes on the network, which answer at once or
    // not at all, so that one gone holds the listener up a second at most.
    const unsigned near = quick ? 1000000u : 5000000u, far = quick ? 2000000u : 10000000u;
    const int rcResolve = httpFn<FnIdUnsigned>(kHttpResolveTimeOut)(tmpl, near),
              rcConnect = httpFn<FnIdUnsigned>(kHttpConnectTimeOut)(tmpl, near),
              rcSend = httpFn<FnIdUnsigned>(kHttpSendTimeOut)(tmpl, far),
              rcRecv = httpFn<FnIdUnsigned>(kHttpRecvTimeOut)(tmpl, far);
    if (rcResolve < 0 || rcConnect < 0 || rcSend < 0 || rcRecv < 0)
        logLine("%s: time-outs not set: 0x%08X 0x%08X 0x%08X 0x%08X", who,
                (unsigned)rcResolve, (unsigned)rcConnect, (unsigned)rcSend, (unsigned)rcRecv);
    return tmpl;
}

// What sceHttpsGetSslError's detail says (SCE_HTTPS_ERROR_SSL_*), for the log.
void sslDetail(unsigned detail, char* out, int cap) {
    static const char* const kWhy[6] = {"internal",      "invalid certificate",
                                        "not the server's name", "expired",
                                        "not yet valid", "unknown CA"};
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < 6 && n < cap; i++)
        if (detail & (1u << i)) n += sceClibSnprintf(out + n, cap - n, "%s%s", n ? ", " : ": ", kWhy[i]);
}

// A request's connection and the request, deleted (either may be -1).
void httpClose(int conn, int req) {
    if (req >= 0) httpFn<FnId>(kHttpDeleteRequest)(req);
    if (conn >= 0) httpFn<FnId>(kHttpDeleteConnection)(conn);
}

// The header lines in `headers` ("Name: value", separated by \r\n or \n, as
// an app wrote them) added to req; a line with no ':' is passed over. 0, or
// the code of the call that failed. Never logged: they carry tokens.
int addHeaders(int req, const char* headers) {
    if (!headers) return 0;
    static char name[64], value[1024];  // under g_netHeadersLock
    for (const char* p = headers; *p;) {
        int n = 0, v = 0;
        while (*p && *p != ':' && *p != '\r' && *p != '\n') {
            if (n < (int)sizeof(name) - 1) name[n++] = *p;
            p++;
        }
        const bool colon = *p == ':';
        if (colon) p++;
        while (*p == ' ') p++;
        while (*p && *p != '\r' && *p != '\n') {
            if (v < (int)sizeof(value) - 1) value[v++] = *p;
            p++;
        }
        while (*p == '\r' || *p == '\n') p++;
        while (n > 0 && name[n - 1] == ' ') n--;
        name[n] = value[v] = 0;
        if (!colon || n == 0) continue;
        sceKernelLockMutex(g_netHeadersLock, 1, NULL);
        const int rc = httpFn<FnHeader>(kHttpAddHeader)(req, name, value, kHeaderOverwrite);
        sceKernelUnlockMutex(g_netHeadersLock, 1);
        if (rc < 0) return rc;
    }
    return 0;
}

// What a request is, beyond its URL: its method, body and the header lines
// to send with it.
struct HttpAsk {
    const char* method;    // "GET" when NULL
    const char* headers;   // the app's header lines, or NULL
    const char* headers2;  // more of them (an item's own), or NULL
    const char* range;     // a Range header's value, or NULL
    const char* body;      // NULL for none
    uint32_t bodyLen;
    const char* type;      // the body's Content-Type, or NULL
};

// A request for url sent with the template tmpl, as `ask` says. `who` and
// `what` name it in the log, which has the call that failed, and why TLS
// refused it; with no `who`, nothing is logged. The URL is never logged:
// an app may put a token in it. 0, with *conn and *req to read from and
// then close (httpClose); else the code of the call that failed, with
// nothing left to close.
int httpOpen(int tmpl, const char* who, const char* what, const char* url, const HttpAsk& ask,
             int* conn, int* req) {
    *req = -1;
    *conn = httpFn<FnConnection>(kHttpCreateConnection)(tmpl, url, 1);
    if (*conn < 0) {
        if (who) logLine("%s: %s: connection 0x%08X", who, what, (unsigned)*conn);
        return *conn;
    }
    const char* const method = ask.method && ask.method[0] ? ask.method : "GET";
    const unsigned long long length = ask.body ? ask.bodyLen : 0;
    int number = -1;
    static const char* const kMethods[6] = {"GET", "POST", "HEAD", "OPTIONS", "PUT", "DELETE"};
    for (int i = 0; i < 6 && number < 0; i++)
        if (same(method, kMethods[i])) number = i;
    if (number >= 0) {
        *req = httpFn<FnRequest>(kHttpCreateRequest)(*conn, number, url, length);
    } else if (g_httpRequest2.fn) {
        *req = ((FnRequest2)g_httpRequest2.fn)(*conn, method, url, length);
    } else {
        *req = -1;
        if (who) logLine("%s: %s: %s cannot be sent here", who, what, method);
    }
    if (*req < 0) {
        const int rc = *req;
        if (who && number >= 0) logLine("%s: %s: request 0x%08X", who, what, (unsigned)rc);
        httpClose(*conn, -1);
        return rc;
    }
    int rc = addHeaders(*req, ask.headers);
    if (rc >= 0) rc = addHeaders(*req, ask.headers2);
    if (rc >= 0 && ask.type)
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "Content-Type", ask.type, kHeaderOverwrite);
    if (rc >= 0 && ask.range)
        rc = httpFn<FnHeader>(kHttpAddHeader)(*req, "Range", ask.range, kHeaderOverwrite);
    if (rc >= 0) rc = httpFn<FnSend>(kHttpSend)(*req, ask.body, ask.body ? ask.bodyLen : 0);
    if (rc < 0) {
        if (who) {
            int err = 0;
            unsigned detail = 0;
            const int rcSsl = httpFn<FnSslError>(kHttpsSslError)(*req, &err, &detail);
            char why[128];
            sslDetail(detail, why, sizeof(why));
            logLine("%s: %s: failed 0x%08X; TLS error 0x%08X, detail 0x%X%s (0x%08X)", who, what,
                    (unsigned)rc, (unsigned)err, detail, why, (unsigned)rcSsl);
        }
        httpClose(*conn, *req);
        return rc;
    }
    return 0;
}

// A GET with the app's header lines.
HttpAsk getWith(const char* headers) {
    HttpAsk a;
    sceClibMemset(&a, 0, sizeof(a));
    a.headers = headers;
    return a;
}

// ── SceShell carries the queue on ───────────────────────────────────────────
// (docs/PROTOCOL.md §4) An app hands its queue over as it goes
// (SC_CMD_MUSIC_PLAY): SceShell plays it from the track and place the app
// had got to, and then the queue's next track, and the one after, from here.
// Everything comes from the queue the app wrote in its folder (SC_QUEUE):
// each track's file on the card, or the URLs to fetch it and its cover from,
// the header lines to send with them, and the report templates that tell
// the app's server where the music is. The status (SC_QUEUE_STATUS) is
// written for the app to take the music back where SceShell has got to. The
// row is SceShell's meanwhile: the track's lines, its cover, and its
// buttons, which act on the music here. Its lines in the log begin "carry
// on:". The comments' player.c is VitaPlex's background player, which played
// a queue the same way from a process of its own, in VitaPlex's history.
const int kLookAhead = 20;        // songs looked at for the next on the card
// How long the music thread looks every 25 ms after the app's track is
// played, for the seek to its place (Carry::fastUntilUs).
const SceUInt64 kFastLookUs = 4 * 1000 * 1000;
const SceUInt kFastLookEveryUs = 25 * 1000;
// Its looks while the shell plays a song of the queue: every 100 ms, so that
// a song the power button stops is played again within one. With a look each
// half second the shell was seen playing again 0.5 to 1.6 s after it was
// seen stopped (logs d6e76980, e92d16a1). Each half second otherwise: as a
// song is fetched, and with no queue (musicLookUs).
const SceUInt kLookEveryUs = 100 * 1000;
const SceUInt kSlowLookEveryUs = 500 * 1000;
// A song the shell never starts is passed over after 15 s of looks.
const int kNeverStartedLooks = 150;
// A song stopped before its end is played again at once, and again every
// 0.6 s while it stays stopped: in log e92d16a1 one was still stopped a
// second after a play, and played after the next.
const uint32_t kReplayEveryUs = 600 * 1000;
// How long a song played again after a stop is watched for going on further
// back than where it stopped (Carry::backUs). In log 0790f7dc an M4A jumped
// back within 1.6 s of playing again once; the other times are bounded by
// the next look the log has, 3.8, 7.6 and 11 s on.
const uint32_t kBackWatchUs = 10 * 1000 * 1000;
// Stopping: the app taking the music back, ready to play it, the service
// stopped and ended and the place it stopped at written; the row given back,
// the place written and the music left; the music left to a process that
// took the row, with nothing written; something else in the shell, the
// status saying stopped; the queue played out, or nothing more on the card:
// the service stopped and ended.
enum { kEndTakeBack = 1, kEndRowBack, kEndYield, kEndOther, kEndOver, kEndStopped };

// Under g_lock:
int g_carry;             // 2 while SceShell carries a queue on, else 0
const char* g_carryStop; // why it is to stop, or NULL ...
int g_carryStopHow;      // ... and how (kEnd*)
int g_playQueueDue;      // an app has handed its queue over (SC_CMD_MUSIC_PLAY) ...
int g_playQueueApp;      // ... this one
int g_carryApp = -1;     // the app whose queue SceShell carries on
// A remote's commands for the queue SceShell carries on (the control
// listener's), queued under g_lock for the music thread (carryRemote).
enum { kRcPlay = 1, kRcPause, kRcToggle, kRcNext, kRcPrevious, kRcStop, kRcSeekTo, kRcForward,
       kRcBack, kRcSkipTo };
struct RemoteCmd {
    int cmd;       // kRc*
    uint32_t ms;   // kRcSeekTo's place; kRcForward's and kRcBack's step, 0 for the queue's;
                   // kRcSkipTo's place in the track
    char id[48];   // kRcSkipTo's track
};
const int kRemoteMax = 8;
RemoteCmd g_remote[kRemoteMax];  // under g_lock, from g_remoteFirst on
int g_remoteFirst, g_remoteCount;
// The queue's header lines, sent with every request for it: copied by the
// fetch and server threads as they begin.
char g_musicHeaders[2048];
uint32_t g_shellArts;    // kShellArt serials so far

// The music thread's alone. A track as the queue's track= line has it
// (SC_TRACK_*), its fields URL-decoded.
struct Track {
    int index;            // its place in the queue, from 0
    char id[48];          // the app's id for it, {id} in the reports
    char key[48];         // the same, kept to what a file name can hold: its files' names
    uint32_t durationMs;  // its length, 0 if unknown
    char local[160];      // a file of it on the card to play as it is, or ""
    char url[640];        // where to fetch it in its own format, or ""
    char ext[8];          // that format's extension, from the URL when not given
    char altUrl[640];     // a URL giving it as MP3, for a format the shell does not play
    char altPrep[512];    // fetched, and its answer passed over, just before altUrl
    char title[192], artist[128], album[128];  // UTF-8, for the row
    char coverUrl[512];   // its cover to fetch for the row, a PNG, or ""
    char coverPng[160];   // its cover on the card, a PNG, or ""
    char path[200];       // the file the shell is given
    int own;              // in the folder of songs (SC_SONGS): removed once two songs on
};
// A few songs remembered by their place in the queue: the last 8.
struct Ring {
    int v[8];
    int n;  // added so far
};

bool ringHas(const Ring* r, int i) {
    const int n = r->n < 8 ? r->n : 8;
    for (int k = 0; k < n; k++)
        if (r->v[k] == i) return true;
    return false;
}

void ringAdd(Ring* r, int i) {
    if (!ringHas(r, i)) r->v[r->n++ & 7] = i;
}

// A report template of the queue's (docs/PROTOCOL.md §4.3), "" for none.
struct ReportTpl {
    char url[1024];
    char method[8];
    char body[512];
    char type[64];
};

struct Carry {
    int app;             // whose queue it is (g_apps)
    char folder[96];     // that app's folder, its name, and its folder of songs
    char name[48];
    char songs[112];
    char queue[32];     // the queue's "written=", as it has it
    int repeat, count;
    // The app's setting (offlineSkip=): a track the server does not answer
    // for is passed over for the next track on the card, and with none,
    // held (carryOnCard).
    int offlineSkip;
    // The queue's buttons=seek: the row's ⏮ ⏭ (and a remote's back and
    // forward) seek by these within the track, as for an audiobook.
    int seekButtons;
    uint32_t seekForwardMs, seekBackMs;
    Track cur;          // the song with the shell
    Track prev;         // the one it had before, whose file stays for previous
    int started;        // seen playing, with its length (newSong)
    int menuPaused;     // the row's play/pause stopped it
    int resuming;       // the row's play pressed, the shell not yet playing
    int failures;       // songs in a row that would not play
    uint32_t maxTime, prevTime, lenMs, lastLenMs;
    uint32_t playMs;    // the clock as it last played, where it goes on from after a stop
    int havePrev, replays, lastState;
    int otherLogged;    // the shell seen playing something else in its place, logged
    int waitTicks;      // looks so far for it to play: looks, not the clock, which runs on
                        // through a sleep
    SceUInt64 songUs;      // when it was handed to the shell
    SceUInt64 stoppedUs;   // stopped before its end since, 0 if not
    SceUInt64 lastPlayUs, statusUs, checkUs;
    // A song not on the card, fetched to play next with the shell stopped
    // meanwhile: its place, -1 when none; the song; since when; the fetches
    // begun for it; why it is next (carryPlay's why); and since when the
    // shell has been seen playing something meanwhile, 0 if it is not.
    int waitIndex;
    Track wait;
    SceUInt64 waitUs;
    int waitTries;
    const char* waitWhy;
    SceUInt64 waitPlayUs;
    int waitArt;      // its cover on the row
    int held;         // paused as the server did not answer, fetched again on the row's play
    // A song's file fetched here and not yet played: its place (-1 for none),
    // key and file. Kept apart from g_fetch, which goes on to other jobs.
    int fetchedIndex;
    char fetchedKey[16];
    char fetchedPath[128];
    int fetchWant;   // a song to fetch once the fetch running has stopped, -1 if none
    int fetchEnds;   // g_fetch.ends as last seen
    int aheadDue;    // what to fetch next to be looked at (fetchAhead)
    Ring lost;         // songs the server has no file of for the shell
    Ring failedAhead;  // songs whose fetch failed ahead: fetched again only when next
    Ring coverTried;   // songs whose cover was asked for
    // The app's track as it handed the queue over (-1 for none), and where
    // in it: the shell is taken there once it plays (seekTo), and a seek to
    // there under way (0 for none), since when, and how many times asked.
    int startIndex;
    uint32_t startAtMs;
    uint32_t seekWantMs;
    SceUInt64 seekUs;
    int seekTries;
    // Until when the music thread looks every 25 ms, not every 100: from
    // the play of the app's track to its seek landing, so that the seek goes
    // as soon as the shell plays (kFastLookUs). 0 when not.
    SceUInt64 fastUntilUs;
    // A song played again after a stop: where it had stopped, and when it
    // played again, for kBackWatchUs. 0 when not watched.
    uint32_t backFromMs;
    SceUInt64 backUs;
    // What the server was last told of the music (report), and when.
    const char* reportState;
    SceUInt64 reportUs;
    // The queue's report templates, and how often the state is repeated.
    ReportTpl reportOn, playedOn;
    uint32_t reportEveryUs;
};
Carry g_c;
// The music's states as the server is told them (report).
const char* const kPlaying = "playing";
const char* const kPaused = "paused";
const char* const kBuffering = "buffering";
const char* const kStopped = "stopped";
void report(const Track* t, const char* state, uint32_t timeMs);
Track g_found;  // a song looked at for what comes next
Track g_scan;   // a song read from the queue as it is looked through (nextOnCard)
int g_statusErrors, g_statusReadErrors;

// No 64-bit division here: it would bring in libgcc's, built for another
// wchar_t. Microseconds as seconds and the rest, a byte at a time, each step
// a 32-bit division (the remainder stays below 1e6 * 256).
void splitUs(SceUInt64 us, uint32_t* secs, uint32_t* micros) {
    uint32_t q = 0, r = 0;
    for (int shift = 56; shift >= 0; shift -= 8) {
        const uint32_t cur = (r << 8) | (uint32_t)((us >> shift) & 0xFF);
        q = (q << 8) | (cur / 1000000u);
        r = cur % 1000000u;
    }
    *secs = q;
    *micros = r;
}

// Milliseconds as "m:ss", for the log.
struct Mmss {
    char s[16];
};
Mmss mmss(uint32_t ms) {
    Mmss m;
    sceClibSnprintf(m.s, sizeof(m.s), "%u:%02u", (unsigned)(ms / 60000),
                    (unsigned)(ms / 1000 % 60));
    return m;
}

uint32_t toUint(const char* s) {
    uint32_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (uint32_t)(*s - '0');
    return v;
}

int toInt(const char* s) {
    return *s == '-' ? -(int)toUint(s + 1) : (int)toUint(s);
}

// A time on the system clock as a note has it ("%.6f" seconds): the same two
// times as these whatever their last digits.
void toSecs(const char* s, uint32_t* secs, uint32_t* micros) {
    *secs = toUint(s);
    *micros = 0;
    while (*s >= '0' && *s <= '9') s++;
    if (*s != '.') return;
    s++;
    uint32_t m = 0;
    int digits = 0;
    for (; digits < 6 && *s >= '0' && *s <= '9'; s++, digits++) m = m * 10 + (uint32_t)(*s - '0');
    for (; digits < 6; digits++) m *= 10;
    *micros = m;
}

bool sameTime(const char* a, const char* b) {
    uint32_t as, am, bs, bm;
    toSecs(a, &as, &am);
    toSecs(b, &bs, &bm);
    return as == bs && am == bm;
}

// A file read whole into buf, as much as fits, ended with 0. Its length, or
// the error.
int readFile(const char* path, char* buf, int cap) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return fd;
    int n = 0;
    while (n < cap - 1) {
        const int got = sceIoRead(fd, buf + n, (SceSize)(cap - 1 - n));
        if (got <= 0) break;
        n += got;
    }
    sceIoClose(fd);
    buf[n] = 0;
    return n;
}

bool exists(const char* path) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    sceIoClose(fd);
    return true;
}

// The value of key in a note (handover.h's "key=value" lines) into out, cut
// to fit. true when it is there.
bool noteValue(const char* text, const char* key, char* out, int cap) {
    const int klen = (int)sceClibStrnlen(key, 64);
    for (const char* p = text; *p;) {
        if (startsWith(p, key) && p[klen] == '=') {
            p += klen + 1;
            int n = 0;
            for (; *p && *p != '\n' && *p != '\r'; p++)
                if (n < cap - 1) out[n++] = *p;
            out[n] = 0;
            return true;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    out[0] = 0;
    return false;
}

// The queue a line at a time, without its end; a line longer than the room
// for it is cut. POD, so that nothing needs constructing as the module loads.
struct LineReader {
    SceUID fd;
    char buf[2048];
    int len, pos;
};
LineReader g_reader;
char g_line[6144];  // the line read last: a track's URLs, URL-encoded, or the header lines

bool readerOpen(const char* path) {
    g_reader.fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    g_reader.len = g_reader.pos = 0;
    return g_reader.fd >= 0;
}

void readerClose() {
    if (g_reader.fd >= 0) sceIoClose(g_reader.fd);
    g_reader.fd = -1;
}

// The next line in g_line. false at the file's end.
bool readerNext() {
    int n = 0;
    bool any = false;
    for (;;) {
        if (g_reader.pos == g_reader.len) {
            const int got = sceIoRead(g_reader.fd, g_reader.buf, sizeof(g_reader.buf));
            g_reader.len = got > 0 ? got : 0;
            g_reader.pos = 0;
            if (g_reader.len == 0) break;
        }
        any = true;
        const char c = g_reader.buf[g_reader.pos++];
        if (c == '\n') break;
        if (c != '\r' && n < (int)sizeof(g_line) - 1) g_line[n++] = c;
    }
    g_line[n] = 0;
    return any;
}

// %XX in the n chars at `from` decoded into out. With whole, out is left ""
// when it does not fit, as a path or URL is never cut; else it is cut, never
// in the middle of a UTF-8 character. false when it did not fit.
bool urlDecode(const char* from, int n, char* out, int cap, bool whole) {
    int o = 0;
    bool cut = false;
    for (int i = 0; i < n; i++) {
        char c = from[i];
        if (c == '%' && i + 2 < n) {
            int v = 0, d = 0;
            for (int k = 1; k <= 2 && i + k < n; k++, d++) {
                const char h = from[i + k];
                const int x = h >= '0' && h <= '9'   ? h - '0'
                            : h >= 'a' && h <= 'f'   ? h - 'a' + 10
                            : h >= 'A' && h <= 'F'   ? h - 'A' + 10
                                                     : -1;
                if (x < 0) break;
                v = v * 16 + x;
            }
            if (d == 2) {
                c = (char)v;
                i += 2;
            }
        }
        if (o < cap - 1) {
            out[o++] = c;
        } else {
            cut = true;
            break;
        }
    }
    if (cut && whole) o = 0;
    if (cut && !whole) {
        while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) o--;
        if (o > 0 && ((unsigned char)out[o - 1] & 0x80)) o--;
    }
    out[o] = 0;
    return !cut;
}

// What the queue's head has beyond its numbers, read only as SceShell begins
// a queue (carryStartQueue): the rest of the time the head is passed over.
struct QueueHead {
    char headers[2048];  // URL-decoded: "Name: value" lines
    ReportTpl state, played;
    uint32_t everyS;     // report.state.every
};
QueueHead g_queueHead;  // the music thread's

// ── Remote control: the listener's set-up ───────────────────────────────────
// (docs/PROTOCOL.md §4.4) A queue may have SceShell listen for a remote
// while it plays: HTTP on a port of the app's choosing, answering the paths
// the app lists (control.route) with the commands they stand for and the
// replies the app wrote (control.reply.<name>), templates as the reports
// are; a UDP discovery answer, so that remotes find it; and the queue's
// state pushed to the remotes that subscribe. A Plex player's companion
// protocol (port 32500, GDM on 32412) is written this way by VitaPlex.
const int kRoutes = 20, kReplies = 4;
struct Route {
    char path[64];
    char cmds[96];   // commands, ',' between: name or name:param (control.route)
    char reply[16];  // the reply's name
};
struct Reply {
    char name[16];
    char body[1536];
};
struct ControlCfg {
    int port;                 // 0: no listener
    char headers[512];        // header lines on every reply, and on the pushes
    Route routes[kRoutes];
    int routeCount;
    Reply replies[kReplies];
    int replyCount;
    char notFound[16];        // the reply for any other path, "" for a plain 404
    int discoveryPort;        // 0: none
    char discoveryGroup[16];  // a multicast group to join, or ""
    char discoveryMatch[32];  // what a search begins with
    char discoveryReply[640];
    char helloTo[32];         // "a.b.c.d:port" the hello and bye go to, or ""
    char hello[512], bye[256];
    char pushUrl[256];        // a template, with {subscriberIp} and {subscriberPort}
    char pushMethod[8];
    char pushReply[16];       // the body's reply
    char pushType[48];
    uint32_t pushEveryMs;
};
ControlCfg g_ctl;   // written as a queue begins, while no listener runs; the listener's after
int g_ctlRunning;   // under g_lock: the listener's thread runs
int g_ctlStop;      // under g_lock: it is to stop

bool headValue(const char* key, char* out, int cap);

// value cut at each '|' in place, into up to n parts: how many.
int splitBar(char* v, char** part, int n) {
    int k = 0;
    part[k++] = v;
    for (char* p = v; *p && k < n; p++)
        if (*p == '|') {
            *p = 0;
            part[k++] = p + 1;
        }
    return k;
}

// A control.* line of the queue's head into c. Unknown ones are passed over.
void controlLine(ControlCfg* c) {
    static char v[1600];
    char* part[4];
    char key[64];
    int k = 0;
    while (g_line[k] && g_line[k] != '=' && k < (int)sizeof(key) - 1) {
        key[k] = g_line[k];
        k++;
    }
    key[k] = 0;
    if (g_line[k] != '=') return;
    if (!headValue(key, v, sizeof(v))) return;
    if (same(key, "control.port")) {
        c->port = (int)toUint(v);
    } else if (same(key, "control.headers")) {
        copyText(c->headers, v, sizeof(c->headers));
    } else if (startsWith(key, "control.reply.") && c->replyCount < kReplies) {
        Reply* r = &c->replies[c->replyCount++];
        copyText(r->name, key + 14, sizeof(r->name));
        copyText(r->body, v, sizeof(r->body));
    } else if (same(key, "control.route") && c->routeCount < kRoutes) {
        const int n = splitBar(v, part, 3);
        Route* r = &c->routes[c->routeCount++];
        copyText(r->path, part[0], sizeof(r->path));
        copyText(r->cmds, n > 1 ? part[1] : "", sizeof(r->cmds));
        copyText(r->reply, n > 2 ? part[2] : "", sizeof(r->reply));
    } else if (same(key, "control.notFound")) {
        copyText(c->notFound, v, sizeof(c->notFound));
    } else if (same(key, "control.discovery.port")) {
        c->discoveryPort = (int)toUint(v);
    } else if (same(key, "control.discovery.group")) {
        copyText(c->discoveryGroup, v, sizeof(c->discoveryGroup));
    } else if (same(key, "control.discovery.match")) {
        copyText(c->discoveryMatch, v, sizeof(c->discoveryMatch));
    } else if (same(key, "control.discovery.reply")) {
        copyText(c->discoveryReply, v, sizeof(c->discoveryReply));
    } else if (same(key, "control.discovery.hello")) {
        const int n = splitBar(v, part, 2);
        copyText(c->helloTo, part[0], sizeof(c->helloTo));
        copyText(c->hello, n > 1 ? part[1] : "", sizeof(c->hello));
    } else if (same(key, "control.discovery.bye")) {
        copyText(c->bye, v, sizeof(c->bye));
    } else if (same(key, "control.push")) {
        const int n = splitBar(v, part, 4);
        copyText(c->pushUrl, part[0], sizeof(c->pushUrl));
        copyText(c->pushMethod, n > 1 && part[1][0] ? part[1] : "POST", sizeof(c->pushMethod));
        copyText(c->pushReply, n > 2 ? part[2] : "", sizeof(c->pushReply));
        copyText(c->pushType, n > 3 ? part[3] : "", sizeof(c->pushType));
    } else if (same(key, "control.push.every")) {
        c->pushEveryMs = toUint(v) * 1000u;
    }
}

// The queue read through: its head lines, which come before its tracks, and
// each track in turn.
struct QueueScan {
    int index;          // the track read last, -1 before the first
    bool whole;         // its "end" read
    char written[32];
    int repeat;
    int start;          // the track to start with (index=), and where in it
    uint32_t startMs;
    int offlineSkip;    // the setting (offlineSkip=), 0 when missing
    int seekButtons;    // buttons=seek: ⏮ ⏭ seek within a track, not skip it
    uint32_t seekForwardMs, seekBackMs;
    QueueHead* head;    // the rest of the head, or NULL to pass it over
    ControlCfg* ctl;    // where control.* lines go, or NULL to pass them over
};

// The queue of the app the music thread plays for (g_c.folder).
bool scanOpen(QueueScan* s, QueueHead* head = NULL) {
    s->index = -1;
    s->whole = false;
    s->written[0] = 0;
    s->repeat = SC_REPEAT_OFF;
    s->start = 0;
    s->startMs = 0;
    s->offlineSkip = 0;
    s->seekButtons = 0;
    s->seekForwardMs = 30000;
    s->seekBackMs = 10000;
    s->head = head;
    s->ctl = NULL;
    if (head) {
        sceClibMemset(head, 0, sizeof(*head));
        head->everyS = 10;
    }
    char path[160];
    sceClibSnprintf(path, sizeof(path), "%s/%s", g_c.folder, SC_QUEUE);
    return readerOpen(path);
}

// A head line's value, URL-decoded, into out when the line is key=value.
bool headValue(const char* key, char* out, int cap) {
    const int klen = (int)sceClibStrnlen(key, 64);
    if (!startsWith(g_line, key) || g_line[klen] != '=') return false;
    const char* v = g_line + klen + 1;
    urlDecode(v, (int)sceClibStrnlen(v, sizeof(g_line)), out, cap, true);
    return true;
}

// A report template's lines (report.state, report.state.method, ...).
bool headReport(const char* key, ReportTpl* t) {
    char k[32];
    sceClibSnprintf(k, sizeof(k), "%s.method", key);
    if (headValue(k, t->method, sizeof(t->method))) return true;
    sceClibSnprintf(k, sizeof(k), "%s.body", key);
    if (headValue(k, t->body, sizeof(t->body))) return true;
    sceClibSnprintf(k, sizeof(k), "%s.type", key);
    if (headValue(k, t->type, sizeof(t->type))) return true;
    return headValue(key, t->url, sizeof(t->url));
}

// The next track's fields, after "track=", in *fields. false at the end.
bool scanNext(QueueScan* s, const char** fields) {
    while (readerNext()) {
        if (startsWith(g_line, "track=")) {
            s->index++;
            *fields = g_line + 6;
            return true;
        }
        if (startsWith(g_line, "written=")) {
            sceClibSnprintf(s->written, sizeof(s->written), "%s", g_line + 8);
        } else if (startsWith(g_line, "repeat=")) {
            s->repeat = (int)toUint(g_line + 7);
        } else if (startsWith(g_line, "index=")) {
            s->start = toInt(g_line + 6);
        } else if (startsWith(g_line, "positionMs=")) {
            s->startMs = toUint(g_line + 11);
        } else if (startsWith(g_line, "offlineSkip=")) {
            s->offlineSkip = toInt(g_line + 12) == 1;
        } else if (startsWith(g_line, "buttons=")) {
            s->seekButtons = same(g_line + 8, "seek");
        } else if (startsWith(g_line, "seekForwardMs=")) {
            s->seekForwardMs = toUint(g_line + 14);
        } else if (startsWith(g_line, "seekBackMs=")) {
            s->seekBackMs = toUint(g_line + 11);
        } else if (same(g_line, "end")) {
            s->whole = true;
        } else if (s->ctl && startsWith(g_line, "control.")) {
            controlLine(s->ctl);
        } else if (s->head) {
            char every[16];
            if (headValue("headers", s->head->headers, sizeof(s->head->headers))) {
            } else if (headValue("report.state.every", every, sizeof(every))) {
                s->head->everyS = toUint(every);
            } else if (startsWith(g_line, "report.state")) {
                headReport("report.state", &s->head->state);
            } else if (startsWith(g_line, "report.played")) {
                headReport("report.played", &s->head->played);
            }
        }
    }
    return false;
}

// Up to the next '|' of *p, URL-decoded into out, and past it: cut to fit,
// or with whole, left "" when it does not fit, as a path or URL is never
// cut.
void field(const char** p, char* out, int cap, bool whole = false) {
    const char* const from = *p;
    while (**p && **p != '|') (*p)++;
    urlDecode(from, (int)(*p - from), out, cap, whole);
    if (**p == '|') (*p)++;
}

// A track's fields as sc_protocol.h lays them out (SC_TRACK_*).
void parseTrack(const char* p, int index, Track* t) {
    sceClibMemset(t, 0, sizeof(*t));
    t->index = index;
    char num[16];
    field(&p, t->id, sizeof(t->id), true);
    sceClibMemcpy(t->key, t->id, sizeof(t->key));
    field(&p, num, sizeof(num));
    t->durationMs = toUint(num);
    field(&p, t->local, sizeof(t->local), true);
    field(&p, t->url, sizeof(t->url), true);
    field(&p, t->ext, sizeof(t->ext), true);
    field(&p, t->altUrl, sizeof(t->altUrl), true);
    field(&p, t->altPrep, sizeof(t->altPrep), true);
    field(&p, t->title, sizeof(t->title));
    field(&p, t->artist, sizeof(t->artist));
    field(&p, t->album, sizeof(t->album));
    field(&p, t->coverUrl, sizeof(t->coverUrl), true);
    field(&p, t->coverPng, sizeof(t->coverPng), true);
    // Its id names its files: kept to what a file name can hold.
    for (char* c = t->key; *c; c++) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.';
        if (!ok) *c = '_';
    }
}

// The queue's song at index into *t. true when it is there.
bool trackAt(int index, Track* t) {
    QueueScan s;
    if (index < 0 || !scanOpen(&s)) return false;
    const char* fields;
    bool found = false;
    while (!found && scanNext(&s, &fields)) {
        if (s.index == index) {
            parseTrack(fields, index, t);
            found = true;
        }
    }
    readerClose();
    return found;
}

// The extension of a path's last part, in lower case, with mp4 as m4a
// (player.c's extOf); "" with none, and cut to fit.
void extOf(const char* path, char* out, int cap) {
    const char* dot = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '.') dot = p;
        if (*p == '/') dot = NULL;
    }
    int n = 0;
    for (const char* p = dot ? dot + 1 : ""; *p && n < cap - 1; p++)
        out[n++] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
    out[n] = 0;
    if (same(out, "mp4")) sceClibSnprintf(out, cap, "m4a");
}

// A format the shell plays, by its extension (player.c's shellDecodes).
bool shellExt(const char* ext) {
    return same(ext, "mp3") || same(ext, "m4a") || same(ext, "aac") || same(ext, "wav");
}

bool shellPlays(const char* path) {
    char ext[8];
    extOf(path, ext, sizeof(ext));
    return shellExt(ext);
}

// Where t is on the card, into t->path: the file the app gave (local), if
// the shell plays its format. false when it is not.
bool onCard(Track* t) {
    if (!t->local[0] || !shellPlays(t->local) || !exists(t->local)) return false;
    sceClibSnprintf(t->path, sizeof(t->path), "%s", t->local);
    t->own = 0;
    return true;
}

// t's file in the folder of songs, whole, into t->path: fetched for it here,
// and kept as the queue went on or was taken back. false when there is none.
bool inFolder(Track* t) {
    static const char* const kExts[4] = {"mp3", "m4a", "aac", "wav"};
    if (!t->key[0]) return false;
    for (int i = 0; i < 4; i++) {
        sceClibSnprintf(t->path, sizeof(t->path), "%s/%s.%s", g_c.songs, t->key, kExts[i]);
        if (exists(t->path)) {
            t->own = 1;
            return true;
        }
    }
    t->path[0] = 0;
    return false;
}

// Whether t is on the card, as the app's file or in the folder of songs,
// with its file in t->path.
bool anyOnCard(Track* t) {
    return onCard(t) || inFolder(t);
}

// The song `steps` on from `from` in the queue's order, back for a negative
// count, repeat all going round; -1 past either end.
int stepFrom(int from, int steps) {
    const int i = from + steps;
    if (i >= 0 && i < g_c.count) return i;
    if (g_c.repeat != SC_REPEAT_ALL || g_c.count <= 0) return -1;
    return ((i % g_c.count) + g_c.count) % g_c.count;
}

// Whether the shell has a song of the length len (its own figure) where it
// should have t: the queue's length to 3 s, or with none in the queue,
// any length but the song before's. As player.c's shellHasNewSong.
bool hasSong(uint32_t len, const Track* t, uint32_t lenBefore) {
    if (len == 0) return false;
    if (t->durationMs > 0) {
        const int d = (int)len - (int)t->durationMs;
        return d > -3000 && d < 3000;
    }
    const int e = (int)len - (int)lenBefore;
    return lenBefore == 0 || e <= -3000 || e >= 3000;
}

// UTF-8 to the UTF-16 the row shows, ending in 0, cut to fit; what is not
// UTF-8 becomes U+FFFD. As client/shell_companion.c's toUtf16.
void fromUtf8(const char* s, uint16_t* out, int cap) {
    const unsigned char* p = (const unsigned char*)s;
    int n = 0;
    while (*p && n + 1 < cap) {
        uint32_t c = *p++;
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

// Writes text, n bytes, to path whole: beside it first, then moved in, so
// that nothing reads half of it. 0, or the code that stopped it.
int writeWhole(const char* path, const char* text, int n) {
    char part[220];
    sceClibSnprintf(part, sizeof(part), "%s.tmp", path);
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return fd;
    const int wrote = sceIoWrite(fd, text, (SceSize)n);
    const int rcClose = sceIoClose(fd);
    int rc = wrote == n ? rcClose : wrote < 0 ? wrote : -1;
    if (rc >= 0) {
        // rename does not replace a file on every target (player.c's moveIn).
        sceIoRemove(path);
        rc = sceIoRename(part, path);
    }
    if (rc < 0) sceIoRemove(part);
    return rc < 0 ? rc : 0;
}

// The status (docs/PROTOCOL.md §4), for t: the track with the shell, or one
// the shell waits for as it is fetched ("fetching=1").
void carryStatusOf(const Track* t, const char* state, uint32_t positionMs, bool fetching) {
    static char text[448], path[160];
    const SceUInt64 now = sceKernelGetSystemTimeWide();
    uint32_t secs, micros;
    splitUs(now, &secs, &micros);
    // paused=1 while the row's play/pause holds it, so that the app takes
    // the music back paused.
    const int n = sceClibSnprintf(
        text, sizeof(text),
        "written=%u.%06u\nqueue=%s\nindex=%d\nid=%s\npositionMs=%u\ndurationMs=%u\n"
        "state=%s\nfetching=%d\npaused=%d\nend\n",
        (unsigned)secs, (unsigned)micros, g_c.queue, t->index, t->key, (unsigned)positionMs,
        (unsigned)t->durationMs, state, fetching ? 1 : 0, g_c.menuPaused ? 1 : 0);
    sceClibSnprintf(path, sizeof(path), "%s/%s", g_c.folder, SC_QUEUE_STATUS);
    const int rc = writeWhole(path, text, n);
    if (rc < 0 && g_statusErrors < 3) {
        g_statusErrors++;
        logLine("carry on: status not written: 0x%08X", (unsigned)rc);
    }
    g_c.statusUs = now;
}

void carryStatus(const char* state, uint32_t positionMs) {
    carryStatusOf(&g_c.cur, state, positionMs, false);
}

// The file in the folder of songs of a track played, and its cover, once the shell
// is two songs on (carryPlay) or the queue is over: as player.c removed its
// own, but for the song before, which previous plays.
void removeOwn(const Track* t) {
    if (!t->own || !t->path[0]) return;
    const int rc = sceIoRemove(t->path);
    char cover[200];
    sceClibSnprintf(cover, sizeof(cover), "%s/%s.png", g_c.songs, t->key);
    sceIoRemove(cover);
    logLine("carry on: song %d's file removed: 0x%08X", t->index + 1, (unsigned)rc);
}

// Where t's cover is on the card, into out: the app's (coverPng), else the
// one fetched or copied for it beside the songs ("<id>.png"). false when it
// has neither.
bool coverPathIn(const char* songs, const Track* t, char* out, int cap) {
    char ext[8];
    extOf(t->coverPng, ext, sizeof(ext));
    if (same(ext, "png") && exists(t->coverPng)) {
        sceClibSnprintf(out, cap, "%s", t->coverPng);
        return true;
    }
    sceClibSnprintf(out, cap, "%s/%s.png", songs, t->key);
    return exists(out);
}

bool coverPath(const Track* t, char* out, int cap) {
    return coverPathIn(g_c.songs, t, out, cap);
}

uint8_t g_copyBuf[4096];  // the music thread's

// t's cover, if it has none on the card, as a copy of other's when the two
// have one cover URL, their album's: the songs of an album after
// the first fetched the same 9190 bytes each, and showed none while their
// files came (log 8e7a4b23). Written beside it first, as the fetch thread's
// are, under a name of its own. The music thread's. true when t's is there.
bool coverFrom(const Track* t, const Track* other) {
    char to[200], from[200], part[208];
    if (coverPath(t, to, sizeof(to))) return true;
    if (!t->coverUrl[0] || !other->key[0] || same(other->key, t->key) ||
        !same(other->coverUrl, t->coverUrl) || !coverPath(other, from, sizeof(from)))
        return false;
    sceClibSnprintf(to, sizeof(to), "%s/%s.png", g_c.songs, t->key);
    const SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    sceClibSnprintf(part, sizeof(part), "%s.copy", to);
    const SceUID out = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    bool ok = out >= 0;
    uint32_t bytes = 0;
    for (int n; ok && (n = sceIoRead(in, g_copyBuf, sizeof(g_copyBuf))) != 0;) {
        ok = n > 0 && sceIoWrite(out, g_copyBuf, (SceSize)n) == n;
        bytes += ok ? (uint32_t)n : 0;
    }
    sceIoClose(in);
    if (out >= 0 && sceIoClose(out) < 0) ok = false;
    ok = ok && bytes > 0;
    if (ok) {
        sceIoRemove(to);
        ok = sceIoRename(part, to) >= 0;
    }
    if (!ok) {
        sceIoRemove(part);
        return false;
    }
    logLine("carry on: song %d's cover is song %d's, the same URL", t->index + 1,
            other->index + 1);
    return true;
}

// The row as t is: its lines, ⏸, and its cover if it has one on the card.
void showTrack(const Track* t) {
    char cover[200];
    const bool art = coverPath(t, cover, sizeof(cover));
    lock();
    fromUtf8(t->title, g_title, 256);
    fromUtf8(t->artist, g_artist, 256);
    fromUtf8(t->album, g_album, 256);
    g_playing = 1;
    if (art) {
        sceClibSnprintf(g_artPath, sizeof(g_artPath), "%s", cover);
        g_artSerial = kShellArt | (++g_shellArts & ~kShellArt);
    } else {
        g_artSerial = 0;
    }
    unlock();
}

// The row's cover, once t's has come, its lines and ⏸ left as they are.
void showCover(const Track* t) {
    char cover[200];
    if (!coverPath(t, cover, sizeof(cover))) return;
    lock();
    sceClibSnprintf(g_artPath, sizeof(g_artPath), "%s", cover);
    g_artSerial = kShellArt | (++g_shellArts & ~kShellArt);
    unlock();
}

// Where in t the shell is to begin: the app's place in the track it handed
// over, until the shell has been taken there; 0 for any other.
uint32_t startAt(const Track* t) {
    return t->index == g_c.startIndex ? g_c.startAtMs : 0;
}

// ── Fetching the queue's songs ──────────────────────────────────────────────
// A track the queue goes on to that is not on the memory card is fetched
// from the URLs the queue gives for it, as VitaPlex's background player
// fetched one (player.c's prepareSong): url, when the shell plays its format,
// into the folder of songs (SC_SONGS), as "<id>.<ext>"; else altUrl, the
// track as MP3, framed as it comes (fetchMp3), as "<id>.mp3"; with its cover
// for the row (coverUrl, "<id>.png") first. Each stays while it is the song
// before, for previous, and goes after that (carryPlay); all stay as the app
// takes the music back, which plays on from them. A track's file there
// already, from a queue carried on before, is played as it is (inFolder).
// The track after the one with the shell is fetched while that one plays;
// one wanted at once (that one ended first, or next was pressed) is waited
// for, the shell stopped meanwhile. With nothing else to fetch, the covers
// the row is to show come next, for the tracks on the card too: this one's,
// then the next one's. A track's own cover (coverPng), or the one of the
// track before with the same cover URL (coverFrom), is never fetched. One
// fetch at a time, on a thread of its own, through Sony's libraries as
// SceShell has them, with the queue's header lines.
enum { kFetchIdle, kFetchRunning, kFetchDone, kFetchFailed };
enum { kJobSong, kJobCover };  // a song's file and its cover, or its cover alone
const int kFetchStack = 0x8000;
const int kFetchStopped = 1;  // fetchBody's: stopped by a cancel

// Under g_lock. t is the fetch thread's as it runs, but for its place and key.
struct Fetch {
    int state;    // kFetch*
    int kind;     // kJob*
    int cancel;   // set to stop it
    int coverDone;  // a song's job fetched its cover, or found it would not come
    int ends;     // fetches ended so far
    int forGood;  // failed in a way another try would not mend
    int unreached;  // failed with no answer from the server (g_noAnswer), or none asked for
    int stopped;  // failed as it was stopped
    int count;    // the queue's songs, for the log
    char songs[112];  // the folder of songs it fetches into
    Track t;      // the song; its file in t.path once fetched
};
Fetch g_fetch;
Track g_ahead;       // the music thread's: a song looked at to fetch
// The fetch thread's: the libraries' functions found; what it passes on to
// the card; a request's URL, the file beside the one it is for, and the
// queue's header lines.
int g_netFound;
uint8_t g_fetchBuf[16384];
char g_fetchUrl[1024], g_fetchPart[220];
char g_fetchHeaders[2048];
// Whether t can be fetched at all: it has a URL for the shell.
bool fetchable(const Track* t) {
    return t->url[0] || t->altUrl[0];
}

// The fetch thread's: a request for its song got no answer from the server,
// failing before any came, as all did with Wi-Fi off (0x80412113 in log
// 0790f7dc).
int g_noAnswer;

// The string value of "key" in VitaPlex's settings, a flat object with \ and
// " escaped (player.c's jsonString). Its length, or -1 with out "".
// The string value of "key" in a JSON answer, with \ and " escaped
// (player.c's jsonString). Its length, or -1 with out "".
int jsonString(const char* json, const char* key, char* out, int cap) {
    char pat[40];
    const int plen = sceClibSnprintf(pat, sizeof(pat), "\"%s\"", key);
    out[0] = 0;
    const char* v = json;
    while (*v && !startsWith(v, pat)) v++;
    if (!*v) return -1;
    v += plen;
    while (*v && *v != ':') v++;
    while (*v && *v != '"') v++;
    if (!*v) return -1;
    v++;
    int n = 0;
    while (*v && *v != '"' && n < cap - 1) {
        if (*v == '\\' && v[1]) v++;
        out[n++] = *v++;
    }
    out[n] = 0;
    if (*v == '"') return n;
    out[0] = 0;
    return -1;
}

// Where `what` first is in s, or NULL.
const char* findText(const char* s, const char* what) {
    for (; *s; s++)
        if (startsWith(s, what)) return s;
    return NULL;
}

// Bytes as megabytes, "x.yy", and milliseconds as seconds, "x.y", for the
// log: in 32 bits, with no division but by constants.
struct Num {
    char s[16];
};
Num mbOf(uint32_t bytes) {
    Num m;
    sceClibSnprintf(m.s, sizeof(m.s), "%u.%02u", (unsigned)(bytes >> 20),
                    (unsigned)(((bytes & 0xFFFFF) * 100u) >> 20));
    return m;
}
Num secsOf(uint32_t ms) {
    Num m;
    sceClibSnprintf(m.s, sizeof(m.s), "%u.%u", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    return m;
}

// The fetch thread's framer for the server's MP3 of a song (fetchMp3), and
// where what it passes on goes: the file.
Mp3Frames g_mp3;
int mp3Sink(void* ctx, const uint8_t* data, uint32_t n) {
    const SceUID fd = *(const SceUID*)ctx;
    return sceIoWrite(fd, data, (SceSize)n) == (int)n ? 0 : -1;
}

// n bytes over the start of the file at path, opened again for it: the
// plugin has no seek of its own, sceIoLseek not being among its imports.
int writeAtStart(const char* path, const uint8_t* data, uint32_t n) {
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY, 0);
    if (fd < 0) return fd;
    const int w = sceIoWrite(fd, data, (SceSize)n);
    const int rc = sceIoClose(fd);
    return w != (int)n ? (w < 0 ? w : -1) : rc;
}

// The body of req: into fd a chunk at a time through g_fetchBuf, framed as
// an MP3 on the way when mp3 is given (mp3_frames.h), or with fd < 0 into
// g_fetchBuf, as much as fits, ended with 0. Its first 8 bytes into first,
// if given; a file's progress logged every 10 s under label, if given. 0 at
// the body's end (or with g_fetchBuf full), kFetchStopped at a cancel, or the
// code of the read or write that failed; the bytes in *got.
int fetchBody(int req, SceUID fd, const char* label, uint8_t* first, uint32_t* got,
              Mp3Frames* mp3 = NULL) {
    *got = 0;
    SceUInt64 logged = sceKernelGetSystemTimeWide();
    int rc = 0;
    for (;;) {
        if (__atomic_load_n(&g_fetch.cancel, __ATOMIC_SEQ_CST)) {
            rc = kFetchStopped;
            break;
        }
        uint8_t* const to = fd >= 0 ? g_fetchBuf : g_fetchBuf + *got;
        const uint32_t want = fd >= 0 ? sizeof(g_fetchBuf) : sizeof(g_fetchBuf) - 1 - *got;
        if (want == 0) break;
        const int n = httpFn<FnRead>(kHttpRead)(req, to, want);
        if (n <= 0) {
            rc = n;
            break;
        }
        for (uint32_t i = 0; first && i < (uint32_t)n && *got + i < 8; i++) first[*got + i] = to[i];
        if (fd >= 0 && mp3) {
            if (mp3fFeed(mp3, to, (uint32_t)n, mp3Sink, &fd) < 0) {
                rc = -1;
                break;
            }
        } else if (fd >= 0) {
            const int w = sceIoWrite(fd, to, (SceSize)n);
            if (w != n) {
                rc = w < 0 ? w : -1;
                break;
            }
        }
        *got += (uint32_t)n;
        const SceUInt64 now = sceKernelGetSystemTimeWide();
        if (label && (uint32_t)(now - logged) >= 10000000u) {
            logged = now;
            logLine("carry on: %s: %s MB so far", label, mbOf(*got).s);
        }
    }
    if (fd < 0) g_fetchBuf[*got] = 0;
    return rc;
}

// {session} in `tmpl` replaced by session, into g_fetchUrl. false when it
// does not fit, as a URL is never cut.
bool sessionUrl(const char* tmpl, uint32_t session) {
    const int cap = (int)sizeof(g_fetchUrl);
    int n = 0;
    for (const char* p = tmpl; *p;) {
        if (startsWith(p, "{session}")) {
            n += sceClibSnprintf(g_fetchUrl + (n < cap ? n : cap - 1), n < cap ? cap - n : 1, "%u",
                                 (unsigned)session);
            p += 9;
        } else {
            if (n < cap - 1) g_fetchUrl[n] = *p;
            n++;
            p++;
        }
    }
    if (n >= cap) {
        g_fetchUrl[0] = 0;
        return false;
    }
    g_fetchUrl[n] = 0;
    return true;
}

// The track's cover for the row, from its coverUrl, which must answer with
// a PNG: nothing here could make one of another format. Left out when
// anything is amiss, the row showing its own icon then. 1 when the cover is
// on the card, 0 when it is not, -1 when stopped.
int fetchCover(int tmpl, const char* label) {
    const Track* const t = &g_fetch.t;
    static char path[200];
    char* const part = g_fetchPart;
    if (coverPathIn(g_fetch.songs, t, path, sizeof(path))) return 1;
    if (!t->coverUrl[0]) return 0;
    sceClibSnprintf(path, sizeof(path), "%s/%s.png", g_fetch.songs, t->key);
    sceClibSnprintf(g_fetchUrl, sizeof(g_fetchUrl), "%s", t->coverUrl);
    sceClibSnprintf(part, sizeof(g_fetchPart), "%s.part", path);
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        logLine("carry on: %s: no cover: %s not written (0x%08X)", label, part, (unsigned)fd);
        return 0;
    }
    int conn, req, status = 0;
    uint8_t first[8];
    sceClibMemset(first, 0, sizeof(first));
    uint32_t got = 0;
    int rc = httpOpen(tmpl, "carry on", "its cover", g_fetchUrl, getWith(g_fetchHeaders), &conn,
                      &req);
    if (rc >= 0) {
        httpFn<FnStatus>(kHttpStatus)(req, &status);
        if (status == 200) rc = fetchBody(req, fd, NULL, first, &got);
        httpClose(conn, req);
    }
    const int rcClose = sceIoClose(fd);
    static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    bool png = got >= 8;
    for (int i = 0; i < 8; i++) png = png && first[i] == kPng[i];
    if (rc == 0 && status == 200 && png && rcClose >= 0) {
        sceIoRemove(path);
        rc = sceIoRename(part, path);
        if (rc >= 0) {
            logLine("carry on: %s: its cover fetched, %u bytes", label, (unsigned)got);
            return 1;
        }
    }
    sceIoRemove(part);
    if (rc == kFetchStopped) {
        logLine("carry on: %s: no cover: stopped", label);
        return -1;
    } else if (rc < 0 || rcClose < 0) {
        logLine("carry on: %s: no cover: 0x%08X", label, (unsigned)(rc < 0 ? rc : rcClose));
    } else if (status != 200) {
        logLine("carry on: %s: no cover: HTTP %d", label, status);
    } else {
        logLine("carry on: %s: no cover: not a PNG, its first bytes %02X %02X %02X %02X", label,
                first[0], first[1], first[2], first[3]);
    }
    return 0;
}

// What g_fetchUrl answers, whole, into the file `to`, through a file beside
// it, moved in once all of it has come; framed on the way when mp3 is given,
// the frame made at the end over the room left for it. The bytes that came
// in *size. kFetchDone, or kFetchFailed with why, and *forGood when another
// try would fail the same way.
int fetchFile(int tmpl, const char* label, const char* to, uint32_t* size, char* why, int cap,
              int* forGood, Mp3Frames* mp3 = NULL) {
    char* const part = g_fetchPart;
    sceClibSnprintf(part, sizeof(g_fetchPart), "%s.part", to);
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        sceClibSnprintf(why, cap, "%s not written (0x%08X)", part, (unsigned)fd);
        return kFetchFailed;
    }
    const SceUInt64 t0 = sceKernelGetSystemTimeWide();
    int conn, req, status = 0, rcLength = -1;
    unsigned long long length = 0;
    uint32_t got = 0;
    int rc = httpOpen(tmpl, "carry on", label, g_fetchUrl, getWith(g_fetchHeaders), &conn, &req);
    if (rc >= 0) {
        httpFn<FnStatus>(kHttpStatus)(req, &status);
        rcLength = httpFn<FnLength>(kHttpLength)(req, &length);
        if (status == 200) rc = fetchBody(req, fd, label, NULL, &got, mp3);
        httpClose(conn, req);
    } else {
        g_noAnswer = 1;
    }
    const int rcClose = sceIoClose(fd);
    // An MP3's frame, over the room at its start; none when the server's
    // first frame is one of its own.
    int made = 0;
    if (mp3 && rc == 0 && status == 200 && rcClose >= 0) {
        made = mp3fFinish(mp3);
        if (made > 0) {
            const int rcHead = writeAtStart(part, mp3->frame, mp3->room);
            if (rcHead < 0) {
                sceClibSnprintf(why, cap, "the frame giving its length not written (0x%08X)",
                                (unsigned)rcHead);
                sceIoRemove(part);
                return kFetchFailed;
            }
        } else if (made < 0) {
            sceClibSnprintf(why, cap, "no MP3 frame in what altUrl gave (%s MB)", mbOf(got).s);
            *forGood = 1;
            sceIoRemove(part);
            return kFetchFailed;
        }
    }
    // In 32 bits: a song comes in well under 71 minutes.
    const uint32_t ms = (uint32_t)(sceKernelGetSystemTimeWide() - t0) / 1000u;
    // The length the server gave, unless it gave none or 4 GB or more.
    const bool sized = rcLength >= 0 && (length >> 32) == 0;
    if (rc == 0 && status == 200 && got > 0 && rcClose >= 0 && (!sized || (uint32_t)length == got)) {
        logLine("carry on: %s: fetched, %s MB in %s s (%u KB/s)", label, mbOf(got).s, secsOf(ms).s,
                (unsigned)((got >> 10) * 1000u / (ms ? ms : 1)));
        *size = got;
        sceIoRemove(to);
        rc = sceIoRename(part, to);
        if (rc >= 0) return kFetchDone;
        sceClibSnprintf(why, cap, "%s not moved in (0x%08X)", part, (unsigned)rc);
    } else if (rc == kFetchStopped) {
        sceClibSnprintf(why, cap, "stopped after %s MB", mbOf(got).s);
    } else if (rc < 0) {
        sceClibSnprintf(why, cap, "its file: 0x%08X after %s MB", (unsigned)rc, mbOf(got).s);
    } else if (status != 200) {
        sceClibSnprintf(why, cap, "its file: HTTP %d", status);
        *forGood = status == 404 || status == 410;
    } else if (rcClose < 0) {
        sceClibSnprintf(why, cap, "%s not written (0x%08X)", part, (unsigned)rcClose);
    } else {
        sceClibSnprintf(why, cap, "%u bytes of its %u came", (unsigned)got,
                        (unsigned)(sized ? length : 0));
    }
    sceIoRemove(part);
    return kFetchFailed;
}

// ── A track as MP3, for a format the shell does not play ──────────────────
// As VitaPlex's background player had it (player.c's download and
// giveHeader, in VitaPlex's history): altPrep first, when the queue gives
// one (Plex's transcode decision), then altUrl, with {session} in both the
// same number. A streamed transcode has no length header, so the frames go
// to the shell behind a Xing frame giving their count, and without the ID3
// tag (mp3_frames.h), framed as they come: two passes over the whole file
// after it had come took 9.6 s of a 23.3 s wait (VitaPlex's log 8e7a4b23).
// The framer is the fetch thread's (g_mp3).

// The track as MP3 into t's path. As fetchFile returns.
int fetchMp3(int tmpl, const char* label, char* why, int cap, int* forGood) {
    Track* const t = &g_fetch.t;
    const uint32_t session = (uint32_t)g_fetch.ends + 1;
    if (t->altPrep[0]) {
        if (!sessionUrl(t->altPrep, session)) {
            sceClibSnprintf(why, cap, "altPrep is longer than the %u bytes here",
                            (unsigned)sizeof(g_fetchUrl));
            *forGood = 1;
            return kFetchFailed;
        }
        int conn, req, status = 0;
        uint32_t got = 0;
        int rc = httpOpen(tmpl, "carry on", label, g_fetchUrl, getWith(g_fetchHeaders), &conn,
                          &req);
        if (rc < 0) {
            sceClibSnprintf(why, cap, "the server did not answer altPrep (0x%08X)", (unsigned)rc);
            g_noAnswer = 1;
            return kFetchFailed;
        }
        httpFn<FnStatus>(kHttpStatus)(req, &status);
        rc = fetchBody(req, -1, NULL, NULL, &got);
        httpClose(conn, req);
        if (rc == kFetchStopped) {
            sceClibSnprintf(why, cap, "stopped");
            return kFetchFailed;
        }
        // As the player did, altUrl whatever this said: its answer says.
        logLine("carry on: %s: altPrep HTTP %d, %u bytes", label, status, (unsigned)got);
    }
    if (!sessionUrl(t->altUrl, session)) {
        sceClibSnprintf(why, cap, "altUrl is longer than the %u bytes here",
                        (unsigned)sizeof(g_fetchUrl));
        *forGood = 1;
        return kFetchFailed;
    }
    mp3fInit(&g_mp3);
    uint32_t size = 0;
    if (fetchFile(tmpl, label, t->path, &size, why, cap, forGood, &g_mp3) != kFetchDone)
        return kFetchFailed;
    const uint32_t ms = mp3fMs(&g_mp3);
    if (g_mp3.tag[0]) {
        logLine("carry on: %s: the MP3 has a %s header already: %s", label, g_mp3.tag, mmss(ms).s);
    } else {
        // The length from the first frame's bitrate alone, as the shell would
        // have it: every byte from that frame on, at that bitrate.
        const uint32_t guess = (size - g_mp3.firstAt) * 8u / g_mp3.firstKbps;
        logLine("carry on: %s: %u frames make %s; the first frame's %u kbps alone would make it "
                "%s, so it goes to the shell behind a header, without the %u-byte ID3 tag",
                label, (unsigned)g_mp3.frames, mmss(ms).s, (unsigned)g_mp3.firstKbps,
                mmss(guess).s, (unsigned)g_mp3.id3Size);
    }
    return kFetchDone;
}

// The fetch thread's work on g_fetch.t, under label: its cover first, which
// the row shows while the track is waited for (with it after the file, the
// row had the heading's own icon all the while: VitaPlex's log 8e7a4b23);
// then its file, from url as it is if the shell plays its format, else from
// altUrl as MP3. kFetchDone with the file at t.path; else kFetchFailed, as
// fetchFile.
int fetchSongNow(int tmpl, const char* label, char* why, int cap, int* forGood) {
    Track* const t = &g_fetch.t;
    if (fetchCover(tmpl, label) < 0) {
        sceClibSnprintf(why, cap, "stopped");
        return kFetchFailed;
    }
    g_fetch.coverDone = 1;
    char ext[8];
    if (t->ext[0])
        sceClibSnprintf(ext, sizeof(ext), "%s", t->ext);
    else
        extOf(t->url, ext, sizeof(ext));
    for (char* c = ext; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c += 32;
    if (same(ext, "mp4")) sceClibSnprintf(ext, sizeof(ext), "m4a");
    const bool asItIs = t->url[0] && shellExt(ext);
    if (!asItIs && !t->altUrl[0]) {
        sceClibSnprintf(why, cap, "its format (%s) is not one the shell plays, and it has no altUrl",
                        ext[0] ? ext : "none given");
        *forGood = 1;
        return kFetchFailed;
    }
    sceClibSnprintf(t->path, sizeof(t->path), "%s/%s.%s", g_fetch.songs, t->key, asItIs ? ext : "mp3");
    if (exists(t->path)) {
        logLine("carry on: %s: already on the memory card, at %s", label, t->path);
    } else if (asItIs) {
        logLine("carry on: %s: a %s file; fetching it to %s", label, ext, t->path);
        if (!sessionUrl(t->url, (uint32_t)g_fetch.ends + 1)) {
            sceClibSnprintf(why, cap, "its URL is longer than the %u bytes here",
                            (unsigned)sizeof(g_fetchUrl));
            *forGood = 1;
            t->path[0] = 0;
            return kFetchFailed;
        }
        uint32_t size = 0;
        if (fetchFile(tmpl, label, t->path, &size, why, cap, forGood) != kFetchDone) {
            t->path[0] = 0;
            return kFetchFailed;
        }
    } else {
        logLine("carry on: %s: %s is not a format the shell plays; fetching it as MP3 to %s",
                label, ext[0] ? ext : "its format", t->path);
        if (fetchMp3(tmpl, label, why, cap, forGood) != kFetchDone) {
            t->path[0] = 0;
            return kFetchFailed;
        }
    }
    return kFetchDone;
}

// The fetch thread: g_fetch.t fetched, or its cover alone, the outcome left
// in g_fetch for the music thread (fetchPoll).
int fetchThread(SceSize, void*) {
    static char label[32], why[96];
    sceClibSnprintf(label, sizeof(label), "song %d of %d", g_fetch.t.index + 1, g_fetch.count);
    why[0] = 0;
    int forGood = 0, result = kFetchFailed;
    bool said = false;    // what came of it logged already
    bool canAsk = false;  // the libraries there to ask with
    g_noAnswer = 0;
    lock();
    sceClibMemcpy(g_fetchHeaders, g_musicHeaders, sizeof(g_fetchHeaders));
    unlock();
    if (!g_netFound) g_netFound = findLibraries("carry on");
    int rcSsl = -1, rcHttp = -1;
    if (!g_netFound) {
        sceClibSnprintf(why, sizeof(why),
                        "Sony's SSL and HTTP libraries are not all there in SceShell");
    } else if (!httpStart("carry on", &rcSsl, &rcHttp)) {
        sceClibSnprintf(why, sizeof(why), "the HTTP library would not start");
    } else {
        const int tmpl = httpTemplate("carry on");
        canAsk = tmpl >= 0;
        if (tmpl < 0) {
            sceClibSnprintf(why, sizeof(why), "no template (0x%08X)", (unsigned)tmpl);
        } else if (g_fetch.kind == kJobCover) {
            const int got = fetchCover(tmpl, label);
            result = got > 0 ? kFetchDone : kFetchFailed;
            if (got < 0) sceClibSnprintf(why, sizeof(why), "stopped");
            said = true;
            httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
        } else {
            result = fetchSongNow(tmpl, label, why, sizeof(why), &forGood);
            httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
        }
    }
    httpEnd(rcSsl, rcHttp);
    sceClibMemset(g_fetchHeaders, 0, sizeof(g_fetchHeaders));
    if (result != kFetchDone && !said)
        logLine("carry on: %s: %snot fetched: %s", label,
                g_fetch.kind == kJobCover ? "its cover " : "", why);
    const bool stopped = result != kFetchDone && startsWith(why, "stopped");
    lock();
    g_fetch.state = result;
    g_fetch.forGood = forGood;
    // No other song would come either (Carry::offlineSkip).
    g_fetch.unreached = result != kFetchDone && !forGood && !stopped && (!canAsk || g_noAnswer);
    g_fetch.stopped = stopped;
    g_fetch.ends++;
    unlock();
    return sceKernelExitDeleteThread(0);
}

// Whether g_fetch is a job of this kind for t. Under g_lock.
bool fetchIsFor(const Track* t, int kind) {
    return g_fetch.state != kFetchIdle && g_fetch.kind == kind && g_fetch.t.index == t->index &&
           same(g_fetch.t.key, t->key);
}

// The song fetched here and not played, left on the card for the app as it
// takes the music back, and forgotten: next waited 22.6 s for a song fetched
// here and removed at the take-back (log 8e7a4b23).
void forgetFetched() {
    if (g_c.fetchedIndex >= 0)
        logLine("carry on: song %d, fetched and not played, left for the app", g_c.fetchedIndex + 1);
    g_c.fetchedIndex = -1;
    g_c.fetchedPath[0] = 0;
}

// The song fetched here and not played, removed with its cover, unless the
// shell has it, whose file goes once another has it; forgotten either way.
void dropFetched() {
    if (g_c.fetchedIndex < 0) return;
    if (!same(g_c.fetchedPath, g_c.cur.path)) {
        const int rc = sceIoRemove(g_c.fetchedPath);
        char cover[200];
        sceClibSnprintf(cover, sizeof(cover), "%s/%s.png", g_c.songs, g_c.fetchedKey);
        sceIoRemove(cover);
        logLine("carry on: song %d, fetched and not played, removed: 0x%08X",
                g_c.fetchedIndex + 1, (unsigned)rc);
    }
    g_c.fetchedIndex = -1;
    g_c.fetchedPath[0] = 0;
}

// A song's file that g_fetch has fetched, taken into g_c.fetched*, and g_fetch
// left free; one fetched before and not played goes, if it is another file.
void takeFetched() {
    Track t;
    lock();
    const bool took = g_fetch.state == kFetchDone && g_fetch.kind == kJobSong;
    if (took) {
        t = g_fetch.t;
        g_fetch.state = kFetchIdle;
    }
    unlock();
    if (!took) return;
    if (g_c.fetchedIndex >= 0 && !same(g_c.fetchedPath, t.path)) dropFetched();
    g_c.fetchedIndex = t.index;
    sceClibSnprintf(g_c.fetchedKey, sizeof(g_c.fetchedKey), "%s", t.key);
    sceClibSnprintf(g_c.fetchedPath, sizeof(g_c.fetchedPath), "%s", t.path);
}

// t's file as fetched here, into t->path: true when it is on the card. One
// gone from the card since is forgotten.
bool fetchedFor(Track* t) {
    takeFetched();
    if (g_c.fetchedIndex != t->index || !same(g_c.fetchedKey, t->key)) return false;
    if (!exists(g_c.fetchedPath)) {
        g_c.fetchedIndex = -1;
        g_c.fetchedPath[0] = 0;
        return false;
    }
    sceClibSnprintf(t->path, sizeof(t->path), "%s", g_c.fetchedPath);
    t->own = 1;
    return true;
}

// A job for t (kJob*): now, or once the one running has stopped, which is
// stopped if it is another's. A song's file is waited for that way
// (fetchWant); a cover only begins with nothing running (fetchAhead). A song
// fetched before and not played is removed.
void fetchStart(const Track* t, int kind) {
    lock();
    const bool running = g_fetch.state == kFetchRunning;
    const bool forIt = fetchIsFor(t, kind);
    int stopping = -1;
    if (running && !forIt && !g_fetch.cancel) {
        __atomic_store_n(&g_fetch.cancel, 1, __ATOMIC_SEQ_CST);
        stopping = g_fetch.t.index;
    }
    const int stoppingKind = g_fetch.kind;
    const bool later = running;
    if (later && kind == kJobSong && !(running && forIt && !g_fetch.cancel))
        g_c.fetchWant = t->index;
    unlock();
    if (stopping >= 0)
        logLine("carry on: song %d's %s stopped: song %d of %d is wanted first", stopping + 1,
                stoppingKind == kJobCover ? "cover's fetch" : "fetch", t->index + 1, g_c.count);
    if (later) return;
    takeFetched();
    if (kind == kJobSong && g_c.fetchedIndex >= 0 && g_c.fetchedIndex != t->index) dropFetched();
    lock();
    g_fetch.state = kFetchRunning;
    g_fetch.kind = kind;
    g_fetch.cancel = 0;
    g_fetch.coverDone = 0;
    g_fetch.forGood = 0;
    g_fetch.unreached = 0;
    g_fetch.stopped = 0;
    g_fetch.count = g_c.count;
    sceClibSnprintf(g_fetch.songs, sizeof(g_fetch.songs), "%s", g_c.songs);
    g_fetch.t = *t;
    g_fetch.t.path[0] = 0;
    unlock();
    if (kind == kJobSong) {
        g_c.fetchWant = -1;
        logLine("carry on: song %d of %d, \"%s\", is not on the memory card: fetching it",
                t->index + 1, g_c.count, t->title);
    } else {
        logLine("carry on: song %d of %d: fetching its cover for the row", t->index + 1,
                g_c.count);
    }
    const SceUID thread = sceKernelCreateThread("ShellCompanion::Fetch", fetchThread, kThreadPriority,
                                                kFetchStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        logLine("carry on: song %d of %d: no thread to fetch it (0x%08X)", t->index + 1, g_c.count,
                (unsigned)rc);
        lock();
        g_fetch.state = kFetchFailed;
        g_fetch.ends++;
        unlock();
    }
}

// The song the queue goes on to after the one with the shell, into out: the
// nearest within kLookAhead on the card, or that the server may have a file
// of for the shell. Its place, with *card saying which; -1 with none.
int nextSong(Track* out, bool* card) {
    for (int d = 1; d <= kLookAhead; d++) {
        const int i = stepFrom(g_c.cur.index, d);
        if (i < 0 || i == g_c.cur.index || !trackAt(i, out)) return -1;
        *card = anyOnCard(out);
        if (*card || (fetchable(out) && !ringHas(&g_c.lost, i))) return i;
    }
    return -1;
}

// Whether t's cover for the row is yet to be fetched: on the server, not on
// the card, and not asked for before.
bool wantsCover(const Track* t) {
    if (!t->coverUrl[0] || !t->key[0] || ringHas(&g_c.coverTried, t->index)) return false;
    return !coverFrom(t, &g_c.cur);
}

// What to fetch while the song with the shell plays, if anything: the next
// song's file when it is not on the card, unless it failed ahead already
// (it is fetched again, waited for, when it is next); else, with nothing
// running, the covers the row is to show, this song's and then the next
// one's. Nothing while waiting, nor a song ahead for repeat one.
void fetchAhead() {
    g_c.aheadDue = 0;
    if (g_c.waitIndex >= 0) return;
    bool card = false;
    const int next = g_c.repeat == SC_REPEAT_ONE ? -1 : nextSong(&g_ahead, &card);
    takeFetched();
    lock();
    const bool running = g_fetch.state == kFetchRunning;
    const bool inHand =
        next >= 0 && ((g_c.fetchedIndex == next && same(g_c.fetchedKey, g_ahead.key)) ||
                      (running && fetchIsFor(&g_ahead, kJobSong)));
    unlock();
    if (next >= 0 && !card && !inHand && fetchable(&g_ahead) && !ringHas(&g_c.failedAhead, next)) {
        coverFrom(&g_ahead, &g_c.cur);
        fetchStart(&g_ahead, kJobSong);
        return;
    }
    if (running) return;
    if (wantsCover(&g_c.cur))
        fetchStart(&g_c.cur, kJobCover);
    else if (next >= 0 && card && wantsCover(&g_ahead))
        fetchStart(&g_ahead, kJobCover);
}

// Each look: a fetch that has ended seen to, the fetch wanted next begun, and
// the next song fetched ahead when it is due.
void fetchPoll() {
    lock();
    const int state = g_fetch.state, ends = g_fetch.ends, index = g_fetch.t.index;
    const int kind = g_fetch.kind, forGood = g_fetch.forGood, stopped = g_fetch.stopped;
    const int coverDone = g_fetch.coverDone;
    unlock();
    if (ends != g_c.fetchEnds) {
        g_c.fetchEnds = ends;
        if (kind == kJobSong && state == kFetchFailed) {
            if (forGood)
                ringAdd(&g_c.lost, index);
            else if (!stopped)
                ringAdd(&g_c.failedAhead, index);
        }
        // A song's file fetched comes with its cover, or with its cover's
        // failure, unless it was waited for: not asked for again.
        if (kind == kJobSong && state == kFetchDone) {
            if (coverDone) ringAdd(&g_c.coverTried, index);
            takeFetched();
        }
        if (kind == kJobCover && !stopped) {
            ringAdd(&g_c.coverTried, index);
            if (state == kFetchDone && index == g_c.cur.index && g_c.waitIndex < 0)
                showCover(&g_c.cur);
        }
        g_c.aheadDue = 1;  // on to what is to be fetched next
    }
    if (state != kFetchRunning && g_c.fetchWant >= 0) {
        const int want = g_c.fetchWant;
        g_c.fetchWant = -1;
        if (trackAt(want, &g_ahead)) fetchStart(&g_ahead, kJobSong);
    }
    if (g_c.aheadDue) fetchAhead();
}

// t, not on the card, fetched and then played (carryWait), the shell stopped
// meanwhile; why is why it is next. The row shows it, and the status names it
// with "fetching=1", so that the app takes the music back from its start.
void waitFor(const Track* t, const char* why) {
    const int rcStop = musicEvent(kEventStop);
    g_c.wait = *t;
    g_c.waitIndex = t->index;
    g_c.waitUs = sceKernelGetSystemTimeWide();
    g_c.waitTries = 0;
    g_c.waitWhy = why;
    g_c.waitPlayUs = 0;
    g_c.held = 0;
    g_c.menuPaused = 0;
    g_c.resuming = 0;
    // No song plays meanwhile: the music thread back to its usual looks.
    g_c.fastUntilUs = 0;
    g_c.backUs = 0;
    logLine("carry on: song %d of %d (%s), \"%s\", is not on the memory card: the shell stopped "
            "(0x%08X) until it is fetched",
            t->index + 1, g_c.count, why, t->title, (unsigned)rcStop);
    coverFrom(t, &g_c.cur);
    showTrack(t);
    char cover[160];
    g_c.waitArt = coverPath(t, cover, sizeof(cover));
    carryStatusOf(t, SC_STATE_PLAYING, startAt(t), true);
    report(t, kBuffering, startAt(t));
    lock();
    const bool under = g_fetch.state == kFetchRunning && fetchIsFor(t, kJobSong) && !g_fetch.cancel;
    unlock();
    if (!under) {
        g_c.waitTries = 1;
        fetchStart(t, kJobSong);
    }
}

// ── Telling the app's server ────────────────────────────────────────────────
// Two things go to the app's server from here, as the queue's report
// templates say (docs/PROTOCOL.md §4.3), on a thread of their own that sends
// what is waiting and ends, so that a slow or missing server holds nothing
// up: not the music, nor the next track's fetch. With the queue's header
// lines, copied for each run and wiped after it.
//
// - A track that played to its end: report.played.
// - Where the music is (report.state): the track, playing, paused,
//   buffering while it is fetched, or stopped, and where in it, at each
//   change and every report.state.every seconds. Only the latest is kept: a
//   server that does not answer is never more than one behind, and its
//   failure is logged once until one is taken.
const int kScrobbles = 4;
// A request made from a template as it fell due, to be sent.
struct Sent {
    char url[1024];
    char body[512];
    char method[8];
    char type[64];
    int index;          // the track's place, for the log
    const char* state;  // for report.state's log
};
// Under g_lock: the tracks waiting to be reported played, from
// g_scrobbleFirst on; the state waiting; and whether a thread is sending
// them.
Sent g_scrobbles[kScrobbles];
int g_scrobbleFirst, g_scrobbleCount, g_serving;
Sent g_report;
int g_reportDue;
// The server thread's: the one it sends, the header lines, and whether the
// last report was not taken (logged once).
Sent g_sending;
char g_serverHeaders[2048];
int g_reportFailed;

// What a template may name beyond the track (the control listener's): the
// request's query, and the subscriber a timeline is pushed to.
struct Extra {
    const char* query;      // the request's query, after '?', or NULL
    const char* commandID;  // {commandID}
    const char* subIp;      // {subscriberIp}
    int subPort;            // {subscriberPort}
};

bool queryParam(const char* query, const char* name, char* out, int cap);

// A template with its placeholders filled in for t, into out: {id},
// {state}, {positionMs}, {positionS}, {durationMs}, {durationS}, {index},
// and with x, {param:NAME}, {commandID}, {subscriberIp} and
// {subscriberPort}. Anything else in braces is left as it is. false when it
// does not fit, as a URL is never cut.
bool expand(const char* tmpl, const Track* t, const char* state, uint32_t posMs, uint32_t durMs,
            char* out, int cap, const Extra* x = NULL) {
    int n = 0;
    char v[96], name[40];
    for (const char* p = tmpl; *p;) {
        const char* put = NULL;
        const char* close = NULL;
        if (*p == '{') {
            int k = 1;
            while (k < 40 && p[k] && p[k] != '}' && p[k] != '{') k++;
            if (p[k] == '}') {
                close = p + k;
                sceClibSnprintf(name, sizeof(name), "%.*s", k - 1, p + 1);
            }
        }
        if (close) {
            if (same(name, "id")) {
                put = t->id;
            } else if (same(name, "state")) {
                put = state;
            } else if (same(name, "positionMs")) {
                sceClibSnprintf(v, sizeof(v), "%u", (unsigned)posMs);
                put = v;
            } else if (same(name, "positionS")) {
                sceClibSnprintf(v, sizeof(v), "%u.%03u", (unsigned)(posMs / 1000),
                                (unsigned)(posMs % 1000));
                put = v;
            } else if (same(name, "durationMs")) {
                sceClibSnprintf(v, sizeof(v), "%u", (unsigned)durMs);
                put = v;
            } else if (same(name, "durationS")) {
                sceClibSnprintf(v, sizeof(v), "%u.%03u", (unsigned)(durMs / 1000),
                                (unsigned)(durMs % 1000));
                put = v;
            } else if (same(name, "index")) {
                sceClibSnprintf(v, sizeof(v), "%d", t->index);
                put = v;
            } else if (x && startsWith(name, "param:")) {
                if (!x->query || !queryParam(x->query, name + 6, v, sizeof(v))) v[0] = 0;
                put = v;
            } else if (x && same(name, "commandID")) {
                put = x->commandID ? x->commandID : "";
            } else if (x && same(name, "subscriberIp")) {
                put = x->subIp ? x->subIp : "";
            } else if (x && same(name, "subscriberPort")) {
                sceClibSnprintf(v, sizeof(v), "%d", x->subPort);
                put = v;
            }
        }
        if (put) {
            for (; *put; put++, n++)
                if (n < cap - 1) out[n] = *put;
            p = close + 1;
        } else {
            if (n < cap - 1) out[n] = *p;
            n++;
            p++;
        }
    }
    if (n >= cap) {
        out[0] = 0;
        return false;
    }
    out[n] = 0;
    return true;
}

// tpl made into a request for t, into out. false when it would not fit.
bool makeSent(const ReportTpl* tpl, const Track* t, const char* state, uint32_t posMs,
              uint32_t durMs, Sent* out) {
    if (!expand(tpl->url, t, state, posMs, durMs, out->url, sizeof(out->url)) ||
        !expand(tpl->body, t, state, posMs, durMs, out->body, sizeof(out->body)))
        return false;
    sceClibSnprintf(out->method, sizeof(out->method), "%s", tpl->method[0] ? tpl->method : "GET");
    sceClibSnprintf(out->type, sizeof(out->type), "%s",
                    tpl->type[0] ? tpl->type : out->body[0] ? "application/json" : "");
    out->index = t->index;
    out->state = state;
    return true;
}

// g_sending sent; the server's status, or 0 when it never answered, the
// call's code then in *rc. Its failure to be sent is logged unless quiet.
int serverSend(int tmpl, const char* what, int* rc, bool quiet = false) {
    int conn, req, status = 0;
    HttpAsk ask = getWith(g_serverHeaders);
    ask.method = g_sending.method;
    if (g_sending.body[0]) {
        ask.body = g_sending.body;
        ask.bodyLen = (uint32_t)sceClibStrnlen(g_sending.body, sizeof(g_sending.body));
    }
    if (g_sending.type[0]) ask.type = g_sending.type;
    *rc = httpOpen(tmpl, quiet ? NULL : "carry on", what, g_sending.url, ask, &conn, &req);
    if (*rc >= 0) {
        httpFn<FnStatus>(kHttpStatus)(req, &status);
        httpClose(conn, req);
    }
    return status;
}

bool ok2xx(int status) {
    return status >= 200 && status < 300;
}

// The server thread's: each track waiting reported played, then the state
// waiting sent, until none is; each said not to be when the libraries are
// not there.
void serverWaiting() {
    int rcSsl = -1, rcHttp = -1, tmpl = -1;
    lock();
    sceClibMemcpy(g_serverHeaders, g_musicHeaders, sizeof(g_serverHeaders));
    unlock();
    if (!g_netFound) g_netFound = findLibraries("carry on");
    if (g_netFound && httpStart("carry on", &rcSsl, &rcHttp)) tmpl = httpTemplate("carry on");
    for (;;) {
        lock();
        const bool marking = g_scrobbleCount > 0;
        if (marking) {
            g_sending = g_scrobbles[g_scrobbleFirst];
            g_scrobbleFirst = (g_scrobbleFirst + 1) % kScrobbles;
            g_scrobbleCount--;
        }
        const bool reporting = !marking && g_reportDue;
        if (reporting) {
            g_sending = g_report;
            g_reportDue = 0;
        }
        unlock();
        int rc = 0;
        if (marking) {
            if (tmpl < 0) {
                logLine("carry on: song %d not reported played: no HTTP library",
                        g_sending.index + 1);
                continue;
            }
            const int status = serverSend(tmpl, "reporting a song played", &rc);
            if (ok2xx(status))
                logLine("carry on: song %d reported played", g_sending.index + 1);
            else
                logLine("carry on: song %d not reported played: HTTP %d (0x%08X)",
                        g_sending.index + 1, status, (unsigned)rc);
        } else if (reporting) {
            int status = 0;
            // Quiet once failed: with Wi-Fi off each report failed, one every
            // 10 s as the songs on the card played (VitaPlex's log 0790f7dc),
            // and the log stops at kLogMax.
            if (tmpl >= 0)
                status = serverSend(tmpl, "telling the server where the music is", &rc,
                                    g_reportFailed != 0);
            if (ok2xx(status) && g_reportFailed) {
                g_reportFailed = 0;
                logLine("carry on: the server told where the music is again: song %d %s",
                        g_sending.index + 1, g_sending.state);
            } else if (!ok2xx(status) && !g_reportFailed) {
                g_reportFailed = 1;
                if (tmpl < 0)
                    logLine("carry on: the server not told where the music is: no HTTP library");
                else
                    logLine("carry on: the server not told where the music is: HTTP %d (0x%08X)",
                            status, (unsigned)rc);
            }
        } else {
            break;
        }
    }
    if (tmpl >= 0) httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
    httpEnd(rcSsl, rcHttp);
    sceClibMemset(g_serverHeaders, 0, sizeof(g_serverHeaders));
    sceClibMemset(&g_sending, 0, sizeof(g_sending));
}

int serverThread(SceSize, void*) {
    for (;;) {
        serverWaiting();
        // Ended only with nothing left waiting, under the lock that anything
        // added since would have been put in under.
        lock();
        const bool more = g_scrobbleCount > 0 || g_reportDue;
        if (!more) g_serving = 0;
        unlock();
        if (!more) break;
    }
    return sceKernelExitDeleteThread(0);
}

// The server thread started, when the caller found none running (g_serving
// set under g_lock). The music thread's.
void startServing() {
    const SceUID thread = sceKernelCreateThread("ShellCompanion::Server", serverThread,
                                                kThreadPriority, kFetchStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        lock();
        const int dropped = g_scrobbleCount;
        g_scrobbleCount = 0;
        g_reportDue = 0;
        g_serving = 0;
        unlock();
        logLine("carry on: nothing told the server: no thread for it (0x%08X); %d song(s) not "
                "reported played",
                (unsigned)rc, dropped);
    }
}

// The length the reports give for t.
uint32_t reportLength(const Track* t) {
    return t->durationMs > 0 ? t->durationMs : g_c.lenMs;
}

// t reported played (report.played), now or after those waiting. The music
// thread's.
void scrobbleSong(const Track* t) {
    if (!g_c.playedOn.url[0] || !t->key[0]) return;
    static Sent made;
    const uint32_t len = reportLength(t);
    if (!makeSent(&g_c.playedOn, t, kStopped, len, len, &made)) {
        logLine("carry on: song %d not reported played: report.played is too long here",
                t->index + 1);
        return;
    }
    lock();
    const bool full = g_scrobbleCount == kScrobbles;
    bool start = false;
    if (!full) {
        g_scrobbles[(g_scrobbleFirst + g_scrobbleCount) % kScrobbles] = made;
        g_scrobbleCount++;
        start = !g_serving;
        if (start) g_serving = 1;
    }
    unlock();
    if (full) {
        logLine("carry on: song %d not reported played: %d songs are waiting to be already",
                t->index + 1, kScrobbles);
        return;
    }
    if (start) startServing();
}

// The server told that t is in `state` (kPlaying...), at timeMs, in place of
// any report not yet sent (report.state). The music thread's.
void report(const Track* t, const char* state, uint32_t timeMs) {
    g_c.reportState = state;
    g_c.reportUs = sceKernelGetSystemTimeWide();
    if (!g_c.reportOn.url[0] || !t->key[0]) return;
    const uint32_t len = reportLength(t);
    // A time past the track's length is refused by some servers (Plex).
    if (len > 0 && timeMs > len) timeMs = len;
    static Sent made;
    if (!makeSent(&g_c.reportOn, t, state, timeMs, len, &made)) {
        if (!g_reportFailed) logLine("carry on: report.state is too long here");
        g_reportFailed = 1;
        return;
    }
    lock();
    g_report = made;
    g_reportDue = 1;
    const bool start = !g_serving;
    if (start) g_serving = 1;
    unlock();
    if (start) startServing();
}

void startControl();
void stopControl();

// Stops carrying the queue on (kEnd*). The row goes back to SceShell unless a
// process has taken it. A fetch under way is stopped, and a song fetched and
// not played removed.
void carryEnd(const char* why, int how) {
    // The remotes go first: nothing they ask now is to be carried out.
    stopControl();
    int state = 0;
    uint32_t time = 0;
    musicStatus(&state, &time);
    // The app takes the music back with its player ready, and plays on from
    // the place written below: the shell is stopped first, so that the status
    // it reads is never one with the shell still playing. Up to build 2048
    // the song was left for the app to stop, which it never did when it went
    // before it got that far, and the song played on in the shell with
    // nothing looking after it (log 7c38c1fa).
    int rcStop = 0, rcEnd = 0;
    if (how == kEndTakeBack) {
        rcStop = musicEvent(kEventStop);
        rcEnd = musicTracked(0x30001);
    }
    // The queue has gone on to a song being fetched: the music plays, as far
    // as the app is to know, from that song's start.
    const bool waiting = g_c.waitIndex >= 0;
    const char* written = "nothing";
    if (how == kEndTakeBack || how == kEndRowBack) {
        written = state == 1 || waiting ? SC_STATE_PLAYING : SC_STATE_STOPPED;
    } else if (how == kEndOther || how == kEndStopped) {
        written = SC_STATE_STOPPED;
    } else if (how == kEndOver) {
        written = SC_STATE_OVER;
    }
    if (how != kEndYield) {
        if (waiting)
            carryStatusOf(&g_c.wait, written, startAt(&g_c.wait), true);
        else
            carryStatus(written, time);
    }
    // The server told the music has stopped, unless the app goes on with it
    // and says so itself.
    if (how == kEndOver || how == kEndStopped || how == kEndOther) {
        if (waiting)
            report(&g_c.wait, kStopped, startAt(&g_c.wait));
        else
            report(&g_c.cur, kStopped, time);
    }
    lock();
    const bool fetching = g_fetch.state == kFetchRunning;
    if (fetching) __atomic_store_n(&g_fetch.cancel, 1, __ATOMIC_SEQ_CST);
    unlock();
    // Taken back, the songs' files stay: the app plays on from them.
    if (how == kEndTakeBack || how == kEndRowBack) {
        takeFetched();
        forgetFetched();
    } else {
        dropFetched();
    }
    if (how == kEndOver || how == kEndStopped) {
        rcStop = musicEvent(kEventStop);
        rcEnd = musicTracked(0x30001);
        removeOwn(&g_c.cur);
        if (!same(g_c.prev.path, g_c.cur.path)) removeOwn(&g_c.prev);
    }
    char also[64] = "";
    if (waiting)
        sceClibSnprintf(also, sizeof(also), ", song %d to come once fetched", g_c.waitIndex + 1);
    logLine("carry on: stopped at song %d of %d, %s, state %d%s (%s): status %s; stop 0x%08X, "
            "end 0x%08X%s",
            g_c.cur.index + 1, g_c.count, mmss(time).s, state, also, why, written,
            (unsigned)rcStop, (unsigned)rcEnd, fetching ? "; its fetch stopped" : "");
    g_c.waitIndex = -1;
    g_c.fastUntilUs = 0;  // the music thread back to its usual looks
    lock();
    g_carry = 0;
    g_carryStop = NULL;
    g_carryApp = -1;
    g_remoteCount = 0;
    sceClibMemset(g_musicHeaders, 0, sizeof(g_musicHeaders));
    if (g_owner == kOwnerShell) {
        g_active = 0;
        g_releaseDue = 1;
        g_artSerial = 0;
        g_playing = -1;
        logLine("carry on: row given back");
        notesWanted();
    }
    unlock();
}

// Nothing carried on after all: the row goes back, as it would have without
// SceShell carrying anything on.
void carryNone() {
    stopControl();
    lock();
    g_carry = 0;
    g_carryStop = NULL;
    g_carryApp = -1;
    g_remoteCount = 0;
    sceClibMemset(g_musicHeaders, 0, sizeof(g_musicHeaders));
    if (g_owner == kOwnerShell) {
        g_active = 0;
        g_releaseDue = 1;
        g_artSerial = 0;
        g_playing = -1;
        logLine("carry on: nothing carried on; row given back");
    }
    unlock();
}

// The carry-on's state as one begins: nothing waited for, wanted or
// remembered, and a fetch from a queue carried on before forgotten, or left
// to stop.
void carryReset() {
    sceClibMemset(&g_c, 0, sizeof(g_c));
    g_c.waitIndex = -1;
    g_c.fetchedIndex = -1;
    g_c.fetchWant = -1;
    g_c.startIndex = -1;
    lock();
    if (g_fetch.state != kFetchRunning) g_fetch.state = kFetchIdle;
    g_c.fetchEnds = g_fetch.ends;
    unlock();
}

void seekTo(uint32_t ms, const char* why = "where the app was");

// Hands t to the shell: stop, open and play, as the player did. true when it
// took the file.
bool carryPlay(const Track* t, const char* why) {
    // The app's place is for its own track alone.
    if (t->index != g_c.startIndex) g_c.startAtMs = 0;
    const uint32_t at = startAt(t);
    const int rcStop = musicEvent(kEventStop), rcOpen = musicOpen(t->path);
    // The place set before play too: should the shell take it then, the
    // song begins there. A seek event right after play went unheeded, and a
    // second of the song's start played before the one after it took (log
    // d6e76980, build 2048).
    const int rcFirst = at >= 2000 ? musicSeekTime(at) : 0;
    const int rcPlay = musicEvent(kEventPlay);
    char first[48] = "";
    if (at >= 2000) sceClibSnprintf(first, sizeof(first), ", seek time first 0x%08X", (unsigned)rcFirst);
    logLine("carry on: song %d of %d (%s), \"%s\" by %s, %s, %s: stop 0x%08X, open 0x%08X%s, "
            "play 0x%08X",
            t->index + 1, g_c.count, why, t->title, t->artist, mmss(t->durationMs).s, t->path,
            (unsigned)rcStop, (unsigned)rcOpen, first, (unsigned)rcPlay);
    if (rcOpen < 0) return false;
    coverFrom(t, &g_c.cur);
    // The song before keeps its file, for previous; the one before that
    // goes, unless the shell has it again or it is the one fetched next:
    // previous waited 37.5 s for a song whose file had gone (log 8e7a4b23).
    if (!same(g_c.cur.path, t->path)) {
        if (!same(g_c.prev.path, t->path) && !same(g_c.prev.path, g_c.cur.path) &&
            !same(g_c.prev.path, g_c.fetchedPath))
            removeOwn(&g_c.prev);
        g_c.prev = g_c.cur;
    }
    g_c.lastLenMs = g_c.lenMs;
    g_c.cur = *t;
    g_c.started = 0;
    g_c.menuPaused = 0;
    g_c.resuming = 0;
    g_c.havePrev = 0;
    g_c.maxTime = 0;
    g_c.playMs = 0;
    g_c.lenMs = 0;
    g_c.replays = 0;
    g_c.otherLogged = 0;
    g_c.waitTicks = 0;
    g_c.stoppedUs = 0;
    g_c.songUs = sceKernelGetSystemTimeWide();
    g_c.waitIndex = -1;
    g_c.held = 0;
    g_c.aheadDue = 1;  // the song after it fetched, if it is not on the card
    g_c.seekWantMs = 0;
    g_c.seekTries = 0;
    g_c.reportState = NULL;  // told once it plays
    g_c.fastUntilUs = 0;
    g_c.backUs = 0;
    // The app's place, asked for at once as well, so that the song's start
    // is not heard first (1.3 s of it in log 8e7a4b23). The shell did not
    // take this seek in log d6e76980; so the music thread looks every 25 ms
    // until it plays, and carryTick asks again the moment it does.
    if (at >= 2000) {
        seekTo(at);
        g_c.fastUntilUs = g_c.songUs + kFastLookUs;
    }
    // A fetched song, played: its file is the shell's now, removed once the
    // shell is two songs on.
    takeFetched();
    if (g_c.fetchedIndex >= 0 && same(g_c.fetchedPath, t->path)) {
        g_c.fetchedIndex = -1;
        g_c.fetchedPath[0] = 0;
    }
    showTrack(t);
    carryStatus(SC_STATE_PLAYING, startAt(t));
    return true;
}

// What carryNext goes on to (nextFrom).
enum { kGoNone, kGoPlay, kGoFetch };

// The song the queue goes on to after `from`, into g_found, within
// kLookAhead songs: the nearest on the card or fetched here (kGoPlay), else
// the nearest to fetch and wait for (kGoFetch). Passed over: songs not on the
// card with no server to fetch them from, and songs the server has no file
// of for the shell.
int nextFrom(int from) {
    for (int d = 1; d <= kLookAhead; d++) {
        const int i = stepFrom(from, d);
        if (i < 0 || i == from || !trackAt(i, &g_found)) return kGoNone;
        if (fetchedFor(&g_found) || anyOnCard(&g_found)) return kGoPlay;
        if (fetchable(&g_found) && !ringHas(&g_c.lost, i)) return kGoFetch;
        logLine("carry on: song %d of %d, \"%s\", is not on the memory card%s: passed over", i + 1,
                g_c.count, g_found.title,
                fetchable(&g_found) ? ", and its URLs gave no file the shell plays"
                                    : ", and the queue has no URL for it");
    }
    return kGoNone;
}

// The song after `from` as the queue goes on: the nearest on the card or
// fetched, or one to fetch, which is waited for; the same one again for
// repeat one as a song ends. Up to three that will not open are passed over.
void carryNext(int from, bool ended, const char* why) {
    g_c.waitIndex = -1;
    for (int tries = 0; tries < 3; tries++) {
        int go;
        if (ended && g_c.repeat == SC_REPEAT_ONE && g_c.cur.path[0] && tries == 0) {
            g_found = g_c.cur;
            go = kGoPlay;
        } else {
            go = nextFrom(from);
        }
        if (go == kGoNone) {
            const bool over = stepFrom(from, 1) < 0;
            carryEnd(over ? "the queue is over"
                          : "no further song is on the memory card or can be fetched",
                     over ? kEndOver : kEndStopped);
            return;
        }
        if (go == kGoFetch) {
            waitFor(&g_found, why);
            return;
        }
        if (carryPlay(&g_found, why)) return;
        from = g_found.index;
    }
    carryEnd("three songs in a row would not open", kEndStopped);
}

// Whether t is on the memory card, with its file in t->path: its download,
// its file fetched here, or its file in the folder of songs. The folder keeps
// the files of the songs near the one playing alone (handover.h), and is
// looked in for those, `near`, and for the songs the shell has and had,
// wherever they are in the queue.
bool cardHas(Track* t, bool near) {
    if (onCard(t) || fetchedFor(t)) return true;
    if (near) return inFolder(t);
    const Track* const had[2] = {&g_c.cur, &g_c.prev};
    for (int k = 0; k < 2; k++) {
        const Track* const h = had[k];
        if (h->index == t->index && same(h->key, t->key) && h->path[0] && exists(h->path)) {
            sceClibSnprintf(t->path, sizeof(t->path), "%s", h->path);
            t->own = h->own;
            return true;
        }
    }
    return false;
}

// The nearest song after `from` in the queue's order that is on the memory
// card (cardHas), repeat all going round, into *out: from one read of the
// queue, as a thousand songs can be between. Its place, or -1 with none.
int nextOnCard(int from, Track* out) {
    QueueScan s;
    if (!scanOpen(&s)) return -1;
    const bool round = g_c.repeat == SC_REPEAT_ALL;
    int found = -1;
    bool after = false;  // found after `from`, not going round
    const char* fields;
    while (!after && scanNext(&s, &fields)) {
        const int i = s.index;
        // Before `from` only going round, and the first found there alone.
        if (i == from || (i < from && (!round || found >= 0))) continue;
        const int ahead = i > from ? i - from : i - from + g_c.count;
        parseTrack(fields, i, &g_scan);
        if (!cardHas(&g_scan, ahead <= kLookAhead || g_c.count - ahead <= kLookAhead)) continue;
        *out = g_scan;
        found = i;
        after = i > from;
    }
    readerClose();
    return found;
}

// The song waited for got no answer from the server, as each did with Wi-Fi
// off, and the queue stopped after three (log 0790f7dc, build 2052). With
// the app's setting (offlineSkip) the queue goes on from the nearest song
// after it on the memory card, from its start; with none, the song is held,
// paused: the row's play fetches it again, and the app takes the music back
// from it.
void carryOnCard() {
    const int from = g_c.waitIndex;
    const int i = nextOnCard(from, &g_found);
    if (i >= 0) {
        logLine("carry on: song %d of %d, \"%s\", could not be fetched, the server not answering: "
                "on to song %d, on the memory card",
                from + 1, g_c.count, g_c.wait.title, i + 1);
        if (carryPlay(&g_found, "on the memory card, the server not answering")) return;
    }
    g_c.held = 1;
    g_c.menuPaused = 1;
    lock();
    g_playing = 0;
    unlock();
    logLine("carry on: song %d of %d, \"%s\", could not be fetched, the server not answering, and "
            "%s: held, paused, for the row's play or the app",
            from + 1, g_c.count, g_c.wait.title,
            i >= 0 ? "the song on the memory card would not open"
                   : "no song after it is on the memory card");
    carryStatusOf(&g_c.wait, SC_STATE_PLAYING, startAt(&g_c.wait), true);
    report(&g_c.wait, kPaused, startAt(&g_c.wait));
}

// The queue app handed to SceShell as it went (SC_CMD_MUSIC_PLAY): played
// from its track and place there, and the row SceShell's from now. That
// track plays from its file on the card, else from its URLs, waited for;
// and if it can be neither, the next that can is played from its start.
// true when it carries on (g_carry is 2 then, or 0 again if the queue had
// nothing to play); else nothing has changed but the row's owner.
bool carryStartQueue(int app) {
    carryReset();
    lock();
    const App* a = appAt(app);
    if (a) {
        g_c.app = app;
        sceClibSnprintf(g_c.folder, sizeof(g_c.folder), "%s", a->folder);
        sceClibSnprintf(g_c.name, sizeof(g_c.name), "%s", a->name);
        sceClibSnprintf(g_c.songs, sizeof(g_c.songs), "%s/%s", a->folder, SC_SONGS);
    }
    unlock();
    if (!a) {
        logLine("play queue: from an app not registered");
        return false;
    }
    QueueScan s;
    if (!scanOpen(&s, &g_queueHead)) {
        logLine("play queue: no queue at %s/%s", g_c.folder, SC_QUEUE);
        return false;
    }
    // The remote control's set-up, unless the last queue's listener is
    // somehow still reading the one there.
    lock();
    const bool ctlFree = !g_ctlRunning;
    unlock();
    if (ctlFree) {
        sceClibMemset(&g_ctl, 0, sizeof(g_ctl));
        s.ctl = &g_ctl;
    }
    const char* fields;
    bool have = false;
    while (scanNext(&s, &fields)) {
        if (s.index == s.start) {
            parseTrack(fields, s.index, &g_c.cur);
            have = true;
        }
    }
    readerClose();
    g_c.count = s.index + 1;
    g_c.repeat = s.repeat;
    g_c.offlineSkip = s.offlineSkip;
    g_c.seekButtons = s.seekButtons;
    g_c.seekForwardMs = s.seekForwardMs ? s.seekForwardMs : 30000;
    g_c.seekBackMs = s.seekBackMs ? s.seekBackMs : 10000;
    sceClibSnprintf(g_c.queue, sizeof(g_c.queue), "%s", s.written);
    g_c.reportOn = g_queueHead.state;
    g_c.playedOn = g_queueHead.played;
    g_c.reportEveryUs = (g_queueHead.everyS ? g_queueHead.everyS : 10) * 1000000u;
    if (g_c.reportEveryUs < 2000000u) g_c.reportEveryUs = 2000000u;
    if (!s.whole || !have) {
        logLine("play queue: %s's queue (%s) is %s", g_c.name, s.written,
                !s.whole ? "not whole" : "without the track it names to begin with");
        sceClibMemset(&g_queueHead, 0, sizeof(g_queueHead));
        return false;
    }
    if (!findSvcObj("play queue") || !g_getSvcObj()) {
        logLine("play queue: no client for SceShell's music service");
        sceClibMemset(&g_queueHead, 0, sizeof(g_queueHead));
        return false;
    }
    const int rcStart = musicTracked(0x30000);
    logLine("play queue: %s's song %d of %d, \"%s\" by %s, %s; repeat %d; offline skip %s; "
            "buttons %s; reports: state %s, played %s; music service started: 0x%08X",
            g_c.name, s.start + 1, g_c.count, g_c.cur.title, g_c.cur.artist, mmss(s.startMs).s,
            g_c.repeat, g_c.offlineSkip ? "on" : "off", g_c.seekButtons ? "seek" : "tracks",
            g_c.reportOn.url[0] ? "yes" : "no", g_c.playedOn.url[0] ? "yes" : "no",
            (unsigned)rcStart);
    if (rcStart < 0) {
        sceClibMemset(&g_queueHead, 0, sizeof(g_queueHead));
        return false;
    }
    sceIoMkdir(g_c.songs, 0777);
    g_c.startIndex = s.start;
    g_c.startAtMs = s.startMs;
    lock();
    sceClibMemcpy(g_musicHeaders, g_queueHead.headers, sizeof(g_musicHeaders));
    g_owner = kOwnerShell;
    g_ownerApp = app;
    g_carryApp = app;
    g_pressTitle[0] = 0;
    g_active = 1;
    g_carry = 2;
    notesWanted();
    unlock();
    sceClibMemset(&g_queueHead, 0, sizeof(g_queueHead));
    if (g_ctl.port)
        logLine("play queue: remote control on port %d, %d route(s), discovery %s, pushes %s",
                g_ctl.port, g_ctl.routeCount, g_ctl.discoveryPort ? "on" : "off",
                g_ctl.pushUrl[0] ? "on" : "off");
    startControl();
    // Nothing is with the shell yet: the track is played from here.
    g_found = g_c.cur;
    g_c.cur.path[0] = 0;
    g_c.cur.own = 0;
    if (!anyOnCard(&g_found)) g_found.path[0] = 0;
    if (g_found.path[0]) {
        if (!carryPlay(&g_found, "the app's track"))
            carryNext(s.start, false, "the app's track would not open");
    } else if (fetchable(&g_found)) {
        waitFor(&g_found, "the app's track");
    } else {
        logLine("play queue: song %d of %d, \"%s\", is not on the memory card, and the queue has "
                "no URL for it",
                s.start + 1, g_c.count, g_found.title);
        carryNext(s.start, false, "the app's track is not on the memory card");
    }
    g_c.aheadDue = 1;
    return true;
}

// Takes the song to ms (the app's place in it, unless why says otherwise):
// the seek time, then the seek event, as player.c's seekFrom; carryTick
// watches where it lands.
void seekTo(uint32_t ms, const char* why) {
    const int rcTime = musicSeekTime(ms), rcSeek = musicEvent(kEventSeek);
    logLine("carry on: song %d of %d to %s, %s: seek time 0x%08X, seek 0x%08X",
            g_c.cur.index + 1, g_c.count, mmss(ms).s, why, (unsigned)rcTime, (unsigned)rcSeek);
    g_c.seekWantMs = ms;
    g_c.seekUs = sceKernelGetSystemTimeWide();
    g_c.seekTries++;
    g_c.maxTime = 0;
    g_c.playMs = ms;
    g_c.havePrev = 0;
    g_c.backUs = 0;
}

// Takes the song back to its start: the seek time, then the seek event, as
// player.c's seekFrom does first.
void seekStart(const char* why) {
    const int rcTime = musicSeekTime(0), rcSeek = musicEvent(kEventSeek);
    logLine("carry on: song %d back to its start (%s): seek time 0x%08X, seek 0x%08X",
            g_c.cur.index + 1, why, (unsigned)rcTime, (unsigned)rcSeek);
    g_c.maxTime = 0;
    g_c.playMs = 0;
    g_c.havePrev = 0;
    g_c.backUs = 0;
}

// The track taken to ms, from the row or a remote: on to the next track when
// that is at or past its end. Not before the shell plays it, as where it is
// is not known yet.
void seekAt(uint32_t ms, const char* why) {
    if (!g_c.started) {
        logLine("carry on: %s: song %d has not begun to play yet; no seek", why, g_c.cur.index + 1);
        return;
    }
    const uint32_t len = g_c.cur.durationMs > 0 ? g_c.cur.durationMs : g_c.lenMs;
    if (len > 0 && ms + 1000 >= len) {
        logLine("carry on: %s: %s is past song %d's end; on to the next", why, mmss(ms).s,
                g_c.cur.index + 1);
        carryNext(g_c.cur.index, false, why);
        return;
    }
    g_c.seekTries = 0;
    seekTo(ms, why);
}

// The track moved by `by` ms from `time`, where it is: back to its start at
// the most.
void seekBy(int by, uint32_t time, const char* why) {
    const int to = (int)time + by;
    seekAt(to > 0 ? (uint32_t)to : 0, why);
}

// A press on the row, as player.c's watchSong takes them: play/pause stops
// the shell and plays it again, which keeps its place; previous goes back to
// the start past a song's first 3 s, and to the song before within them,
// fetched first if it is not on the card; next goes on past repeat one.
void carryPress(int press, int state, uint32_t time, bool fromRow = true) {
    g_c.backUs = 0;  // where the song goes now is the row's doing
    // A remote's next and previous are tracks whatever the buttons do: it has
    // forward and back of its own.
    if (fromRow && g_c.seekButtons && (press == SC_PRESS_NEXT || press == SC_PRESS_PREVIOUS)) {
        const int by = press == SC_PRESS_NEXT ? (int)g_c.seekForwardMs : -(int)g_c.seekBackMs;
        seekBy(by, time, press == SC_PRESS_NEXT ? "forward, from the row" : "back, from the row");
        return;
    }
    if (press == SC_PRESS_NEXT) {
        logLine("carry on: next, from the row, at %s", mmss(time).s);
        carryNext(g_c.cur.index, false, "next, from the row");
        return;
    }
    if (press == SC_PRESS_PREVIOUS) {
        if (g_c.started && time >= 3000) {
            g_c.menuPaused = 0;
            seekStart("previous, from the row");
        } else {
            logLine("carry on: previous, from the row, at %s", mmss(time).s);
            const int before = stepFrom(g_c.cur.index, -1);
            if (before >= 0 && trackAt(before, &g_found)) {
                if (fetchedFor(&g_found) || anyOnCard(&g_found)) {
                    if (carryPlay(&g_found, "previous, from the row")) return;
                } else if (fetchable(&g_found) && !ringHas(&g_c.lost, before)) {
                    waitFor(&g_found, "previous, from the row");
                    return;
                }
            }
            seekStart("previous, from the row, with the song before not on the card");
        }
        return;
    }
    int rc;
    if (g_c.menuPaused || state != 1) {
        g_c.menuPaused = 0;
        g_c.resuming = 1;
        rc = musicEvent(kEventPlay);
        lock();
        g_playing = 1;
        unlock();
    } else {
        g_c.menuPaused = 1;
        rc = musicEvent(kEventStop);
        lock();
        g_playing = 0;
        unlock();
    }
    logLine("carry on: %s at %s, from the row: 0x%08X", g_c.menuPaused ? "paused" : "play",
            mmss(time).s, (unsigned)rc);
}

// ── Remote control: the commands ────────────────────────────────────────────
// What a remote asks of the queue SceShell carries on (the control listener,
// below), queued under g_lock for the music thread, which carries each out
// at its next look as it does the row's presses.
// (the commands themselves: RemoteCmd, with the carry-on's state)

// What the queue is doing, for the control listener's replies: written by
// the music thread at each look, read under g_lock. serial changes with the
// track or its state, so that a reply waiting for a change sees one.
struct CtlState {
    char id[48];
    int index;
    uint32_t durationMs, positionMs;
    const char* state;  // kPlaying, kPaused, kBuffering or kStopped
    uint32_t serial;
};
CtlState g_ctlState;

// A command queued, and the music thread woken for it. Under g_lock. false
// when too many wait.
bool remotePush(const RemoteCmd& c) {
    if (g_remoteCount == kRemoteMax || !g_carry) return false;
    g_remote[(g_remoteFirst + g_remoteCount) % kRemoteMax] = c;
    g_remoteCount++;
    if (g_musicSema >= 0) sceKernelSignalSema(g_musicSema, 1);
    return true;
}

// The music thread's: the next command waiting, into *c.
bool remoteTake(RemoteCmd* c) {
    lock();
    const bool any = g_remoteCount > 0;
    if (any) {
        *c = g_remote[g_remoteFirst];
        g_remoteFirst = (g_remoteFirst + 1) % kRemoteMax;
        g_remoteCount--;
    }
    unlock();
    return any;
}

// The music thread's: where the queue is, for the control listener.
void publishState(const Track* t, const char* state, uint32_t positionMs) {
    lock();
    if (state != g_ctlState.state || !same(g_ctlState.id, t->id) || g_ctlState.index != t->index)
        g_ctlState.serial++;
    sceClibSnprintf(g_ctlState.id, sizeof(g_ctlState.id), "%s", t->id);
    g_ctlState.index = t->index;
    g_ctlState.durationMs = t->durationMs > 0 ? t->durationMs : g_c.lenMs;
    g_ctlState.positionMs = positionMs;
    g_ctlState.state = state;
    unlock();
}

// The queue's track whose id is id, or whose id id ends with after a '/'
// (as "/library/metadata/123" names 123): its place, or -1.
int indexOfId(const char* id) {
    QueueScan s;
    if (!id[0] || !scanOpen(&s)) return -1;
    const char* fields;
    int found = -1;
    char one[48];
    const int n = (int)sceClibStrnlen(id, 64);
    while (found < 0 && scanNext(&s, &fields)) {
        const char* p = fields;
        field(&p, one, sizeof(one), true);
        const int m = (int)sceClibStrnlen(one, sizeof(one));
        if (same(one, id) || (n > m && id[n - m - 1] == '/' && same(id + n - m, one)))
            found = s.index;
    }
    readerClose();
    return found;
}

// The track at index played from its start: from the card, or fetched and
// waited for. why says who asked.
void goTo(int index, const char* why) {
    if (index < 0 || !trackAt(index, &g_found)) {
        logLine("carry on: %s: no such track in the queue", why);
        return;
    }
    if (fetchedFor(&g_found) || anyOnCard(&g_found)) {
        if (!carryPlay(&g_found, why)) carryNext(index, false, "the one asked for would not open");
    } else if (fetchable(&g_found) && !ringHas(&g_c.lost, index)) {
        waitFor(&g_found, why);
    } else {
        logLine("carry on: %s: song %d is not on the card and cannot be fetched", why, index + 1);
    }
}

// A remote's command carried out. While a track is fetched (waiting), play,
// pause and the tracks go the way the row's presses do there, and seeks wait
// for a track to seek in. true when it took the queue elsewhere, and the
// look is over.
bool carryRemote(const RemoteCmd& c, bool waiting, int state, uint32_t time) {
    static const char* const kNames[11] = {"?",    "play",   "pause", "play/pause", "next", "previous",
                                           "stop", "seek",   "forward", "back",     "skip to"};
    logLine("carry on: %s, from a remote", kNames[c.cmd >= 1 && c.cmd <= 10 ? c.cmd : 0]);
    if (c.cmd == kRcStop) {
        carryEnd("stopped from a remote", kEndStopped);
        return true;
    }
    if (c.cmd == kRcSkipTo) {
        const int i = indexOfId(c.id);
        if (i < 0) {
            logLine("carry on: skip to \"%s\": not in the queue", c.id);
            return false;
        }
        // Begun at its place, as the app's track is as a queue begins.
        g_c.startIndex = c.ms >= 2000 ? i : -1;
        g_c.startAtMs = c.ms >= 2000 ? c.ms : 0;
        goTo(i, "skip to, from a remote");
        return true;
    }
    const bool paused = g_c.menuPaused || (!waiting && state != 1);
    int press = 0;
    if (c.cmd == kRcToggle || (c.cmd == kRcPlay && paused) || (c.cmd == kRcPause && !paused))
        press = SC_PRESS_PLAY_PAUSE;
    else if (c.cmd == kRcNext)
        press = SC_PRESS_NEXT;
    else if (c.cmd == kRcPrevious)
        press = SC_PRESS_PREVIOUS;
    if (press) {
        if (waiting) {
            // carryWait takes it as it does the row's.
            __atomic_store_n(&g_carryPress, press, __ATOMIC_SEQ_CST);
            return false;
        }
        carryPress(press, state, time, false);
        return true;
    }
    if (c.cmd == kRcPlay || c.cmd == kRcPause) return false;  // as it is already
    if (waiting) {
        logLine("carry on: no seek while song %d is fetched", g_c.waitIndex + 1);
        return false;
    }
    if (c.cmd == kRcSeekTo)
        seekAt(c.ms, "seek, from a remote");
    else if (c.cmd == kRcForward)
        seekBy((int)(c.ms ? c.ms : g_c.seekForwardMs), time, "forward, from a remote");
    else if (c.cmd == kRcBack)
        seekBy(-(int)(c.ms ? c.ms : g_c.seekBackMs), time, "back, from a remote");
    return true;
}

// Each look while a song is fetched to play next: it is played once fetched,
// tried again once if its fetch fails, and else passed over, or, with no
// answer from the server, the queue goes on from the songs on the card or
// holds it (carryOnCard). The row's buttons act on it: play/pause holds it
// back once fetched, and fetches a song held again; previous goes back to the
// song before from its start, next passes it over.
void carryWait() {
    // Written every 2 s while it waits, as while it plays: an app takes a
    // status not written for a while for a queue SceShell no longer carries
    // on.
    if ((uint32_t)(sceKernelGetSystemTimeWide() - g_c.statusUs) >= 2000000u)
        carryStatusOf(&g_c.wait, SC_STATE_PLAYING, startAt(&g_c.wait), true);
    publishState(&g_c.wait, g_c.menuPaused ? kPaused : kBuffering, startAt(&g_c.wait));
    RemoteCmd rc;
    if (remoteTake(&rc) && carryRemote(rc, true, 0, 0)) return;
    const int press = __atomic_exchange_n(&g_carryPress, 0, __ATOMIC_SEQ_CST);
    if (press == SC_PRESS_NEXT) {
        logLine("carry on: next, from the row, as song %d is fetched", g_c.waitIndex + 1);
        lock();
        if (g_fetch.state == kFetchRunning && fetchIsFor(&g_c.wait, kJobSong))
            __atomic_store_n(&g_fetch.cancel, 1, __ATOMIC_SEQ_CST);
        unlock();
        carryNext(g_c.waitIndex, false, "next, from the row");
        return;
    }
    if (press == SC_PRESS_PREVIOUS) {
        logLine("carry on: previous, from the row, as song %d is fetched", g_c.waitIndex + 1);
        if (g_c.cur.path[0] && exists(g_c.cur.path)) {
            g_found = g_c.cur;
            if (carryPlay(&g_found, "previous, from the row")) return;
        }
    } else if (press == SC_PRESS_PLAY_PAUSE) {
        g_c.menuPaused = !g_c.menuPaused;
        lock();
        g_playing = g_c.menuPaused ? 0 : 1;
        unlock();
        logLine("carry on: %s as song %d is fetched, from the row",
                g_c.menuPaused ? "paused" : "play", g_c.waitIndex + 1);
        // Held as the server did not answer: fetched again, twice at most.
        if (!g_c.menuPaused && g_c.held) {
            g_c.held = 0;
            g_c.waitTries = 0;
        }
    }
    // Something else played in the shell meanwhile, as the system's Music
    // app would play it, seen for half a second: not for this to stop. Timed,
    // as looks can come 25 ms apart.
    int state = 0;
    uint32_t time = 0;
    if (musicStatus(&state, &time) >= 0 && state == 1) {
        const SceUInt64 now = sceKernelGetSystemTimeWide();
        if (!g_c.waitPlayUs) {
            g_c.waitPlayUs = now;
        } else if ((uint32_t)(now - g_c.waitPlayUs) >= 500000u) {
            carryEnd("something else plays in the shell", kEndOther);
            return;
        }
    } else {
        g_c.waitPlayUs = 0;
    }
    // Its cover, fetched before its file, on the row as soon as it comes.
    if (!g_c.waitArt) {
        char cover[160];
        if (coverPath(&g_c.wait, cover, sizeof(cover))) {
            showCover(&g_c.wait);
            g_c.waitArt = 1;
        }
    }
    g_found = g_c.wait;
    if (fetchedFor(&g_found)) {
        if (g_c.menuPaused) return;
        logLine("carry on: song %d of %d fetched, after %s s of waiting", g_found.index + 1,
                g_c.count, secsOf((uint32_t)(sceKernelGetSystemTimeWide() - g_c.waitUs) / 1000u).s);
        if (!carryPlay(&g_found, g_c.waitWhy))
            carryNext(g_found.index, false, "the one before would not open");
        return;
    }
    // Held: only the row's play fetches it again.
    if (g_c.held) return;
    lock();
    const int fetchState = g_fetch.state;
    const bool forIt = fetchIsFor(&g_c.wait, kJobSong);
    const bool forGood = forIt && fetchState == kFetchFailed && g_fetch.forGood;
    const bool unreached = forIt && fetchState == kFetchFailed && g_fetch.unreached;
    unlock();
    // Its fetch under way, or to begin once another has stopped.
    if ((forIt && fetchState == kFetchRunning) || g_c.fetchWant == g_c.waitIndex) return;
    // Failed, or its file gone since it came: fetched again once, then
    // passed over, or with no answer from the server, on to the songs on the
    // card (carryOnCard).
    if (!forGood && g_c.waitTries < 2) {
        g_c.waitTries++;
        logLine("carry on: song %d of %d: fetched again", g_c.waitIndex + 1, g_c.count);
        fetchStart(&g_c.wait, kJobSong);
        return;
    }
    if (unreached && g_c.offlineSkip) {
        carryOnCard();
        return;
    }
    logLine("carry on: song %d of %d, \"%s\", could not be fetched: passed over",
            g_c.waitIndex + 1, g_c.count, g_c.wait.title);
    if (!forGood && ++g_c.failures >= 3) {
        carryEnd("three songs in a row could not be fetched", kEndStopped);
        return;
    }
    carryNext(g_c.waitIndex, false, g_c.waitWhy);
}

// At each of the music thread's looks while it carries on (musicLookUs), as
// player.c's watchSong did each half second.
void carryTick() {
    lock();
    const char* stop = g_carryStop;
    const int how = g_carryStopHow;
    unlock();
    if (stop) {
        carryEnd(stop, how);
        return;
    }
    fetchPoll();
    if (g_c.waitIndex >= 0) {
        carryWait();
        return;
    }
    const SceUInt64 now = sceKernelGetSystemTimeWide();
    int state = 0;
    uint32_t time = 0;
    const int rc = musicStatus(&state, &time);
    if (rc < 0) {
        if (g_statusReadErrors < 6) {
            g_statusReadErrors++;
            logLine("carry on: the music service's status: 0x%08X", (unsigned)rc);
        }
        return;
    }
    if (state != g_c.lastState) {
        logLine("carry on: song %d of %d: state %d at %s", g_c.cur.index + 1, g_c.count, state,
                mmss(time).s);
        g_c.lastState = state;
    }
    const int press = __atomic_exchange_n(&g_carryPress, 0, __ATOMIC_SEQ_CST);
    if (press) {
        carryPress(press, state, time);
        return;
    }
    RemoteCmd remote;
    if (remoteTake(&remote) && carryRemote(remote, false, state, time)) return;
    publishState(&g_c.cur, g_c.menuPaused ? kPaused : state == 1 ? kPlaying : kBuffering, time);
    if (!g_c.started) {
        // The app's track playing, short of its place: taken there at once,
        // not once its clock is seen to move, which took another 0.66 s in log
        // d6e76980 (state 1 at 0:00 0.58 s after play). Twice more at most,
        // half a second apart, should the shell not take it yet.
        const uint32_t place = startAt(&g_c.cur);
        if (state == 1 && now < g_c.fastUntilUs && place >= 2000 && time + 5000 < place &&
            g_c.seekTries < 3 && (g_c.seekTries == 1 || (uint32_t)(now - g_c.seekUs) >= 500000u)) {
            seekTo(place);
            return;
        }
        if (state == 1 && g_c.havePrev && time > g_c.prevTime) {
            uint32_t len = 0;
            musicLength(&len);
            if (hasSong(len, &g_c.cur, g_c.lastLenMs)) {
                g_c.started = 1;
                g_c.failures = 0;
                g_c.maxTime = time;
                g_c.lenMs = len;
                logLine("carry on: song %d of %d plays: the shell has it as %s long, the server "
                        "%s, and is at %s",
                        g_c.cur.index + 1, g_c.count, mmss(len).s, mmss(g_c.cur.durationMs).s,
                        mmss(time).s);
                // To the app's place in the track it handed over; else
                // from the start, as the shell can start a file it has had
                // before where it left it (player.c's fromTheStart). Then
                // nothing more this look: what was read is from before the
                // seek. Taken for where the song had got to, 3:11 of a song
                // the shell started again near its end for repeat one made
                // the next look's 0:00 its end, and it began again, and so on.
                const uint32_t at = startAt(&g_c.cur);
                g_c.startAtMs = 0;
                if (at >= 2000) {
                    // Begun there, the seek asked for with the song taken:
                    // else asked for again now that it plays.
                    if (time + 5000 >= at && time <= at + 6000) {
                        logLine("carry on: song %d of %d %s %s, where the app was, %s s after "
                                "play%s",
                                g_c.cur.index + 1, g_c.count,
                                g_c.seekTries > 1 ? "at" : "began at", mmss(time).s,
                                secsOf((uint32_t)(now - g_c.songUs) / 1000u).s,
                                g_c.seekTries > 1 ? ", taken there as it began" : "");
                        g_c.seekWantMs = 0;
                        g_c.fastUntilUs = 0;
                    } else {
                        g_c.seekTries = 0;
                        seekTo(at);
                        return;
                    }
                } else if (time >= 5000) {
                    seekStart("the shell started it part-way");
                    return;
                }
            } else if (!g_c.otherLogged) {
                g_c.otherLogged = 1;
                logLine("carry on: song %d of %d: the shell plays something %s long, not this "
                        "song of %s",
                        g_c.cur.index + 1, g_c.count, mmss(len).s, mmss(g_c.cur.durationMs).s);
            }
        }
        // Counted in the usual looks, not the 25 ms ones after the app's
        // song is played.
        if (!g_c.started && now >= g_c.fastUntilUs && ++g_c.waitTicks > kNeverStartedLooks) {
            logLine("carry on: song %d of %d: the shell never started it", g_c.cur.index + 1,
                    g_c.count);
            if (++g_c.failures >= 3)
                carryEnd("three songs in a row would not play", kEndStopped);
            else
                carryNext(g_c.cur.index, false, "the one before would not play");
            return;
        }
    }
    g_c.prevTime = time;
    g_c.havePrev = 1;
    if (g_c.started) {
        // Playing again after a stop or the row's play: not its end, wherever
        // it went on from (below), nor is the stop before the shell plays
        // again after the row's play.
        const bool back = state == 1 && (g_c.stoppedUs || g_c.resuming);
        // Only from a status read after the seek: in the tick that asked for
        // it, the shell was read at 0:00 before it, and the time since the
        // seek came out below 0, as 4294.9 s, which asked for it again at
        // once (log 5ae2ab41, build 2045).
        if (g_c.seekWantMs && now > g_c.seekUs) {
            // Near enough: an M4A download's seek landed 2 to 4 s short (log
            // f3042ca3, build 2035).
            const uint32_t want = g_c.seekWantMs;
            const uint32_t since = (uint32_t)(now - g_c.seekUs) / 1000u;
            if (time + 5000 >= want && time <= want + 6000) {
                logLine("carry on: song %d of %d at %s, %s s after the seek to %s, %s s after "
                        "play",
                        g_c.cur.index + 1, g_c.count, mmss(time).s, secsOf(since).s,
                        mmss(want).s, secsOf((uint32_t)(now - g_c.songUs) / 1000u).s);
                g_c.seekWantMs = 0;
                g_c.fastUntilUs = 0;
                g_c.maxTime = time;
            } else if (since >= 4000) {
                if (g_c.seekTries < 2) {
                    logLine("carry on: song %d of %d at %s, not %s, 4 s after the seek: again",
                            g_c.cur.index + 1, g_c.count, mmss(time).s, mmss(want).s);
                    seekTo(want, "again");
                    return;
                } else {
                    logLine("carry on: the seeks did not take song %d of %d to %s; it goes on "
                            "from %s",
                            g_c.cur.index + 1, g_c.count, mmss(want).s, mmss(time).s);
                    g_c.seekWantMs = 0;
                }
            }
        }
        // Not while a seek is under way: the place read before it lands is
        // where the track was, and one further on than where it lands would
        // read as the track's end.
        if (time > g_c.maxTime && !g_c.seekWantMs) g_c.maxTime = time;
        const uint32_t lenMs = g_c.cur.durationMs > 0 ? g_c.cur.durationMs : g_c.lenMs;
        if (!back && !g_c.resuming && !g_c.seekWantMs && g_c.maxTime + 10000 >= lenMs &&
            (time + 5000 < g_c.maxTime || state != 1) && !g_c.menuPaused) {
            logLine("carry on: song %d of %d ended (it got to %s of %s)", g_c.cur.index + 1,
                    g_c.count, mmss(g_c.maxTime).s, mmss(lenMs).s);
            scrobbleSong(&g_c.cur);
            carryNext(g_c.cur.index, true, "the one before ended");
            return;
        }
        // Stopped before its end, not from the row: as the power button
        // stops the shell's music, 0.1 to 0.4 s after four of seven presses
        // in log e98e760e (build 2002). Played again at once, and every 0.6 s
        // while it stays so (kReplayEveryUs): with 5 s between tries, the
        // music once stayed stopped 2.6 s (log e18b5831). Logged the first
        // times and then once a minute.
        if (state != 1 && !g_c.menuPaused && !g_c.resuming) {
            if (!g_c.stoppedUs) g_c.stoppedUs = now;
            if ((uint32_t)(now - g_c.lastPlayUs) >= kReplayEveryUs) {
                g_c.lastPlayUs = now;
                const int rcPlay = musicEvent(kEventPlay);
                if (g_c.replays < 3 || g_c.replays % 100 == 0)
                    logLine("carry on: song %d of %d stopped at %s, before its end (state %d); "
                            "play again: 0x%08X",
                            g_c.cur.index + 1, g_c.count, mmss(time).s, state, (unsigned)rcPlay);
                g_c.replays++;
            }
        } else if (back) {
            if (g_c.stoppedUs)
                logLine("carry on: song %d of %d plays again at %s, %s s after it was seen stopped",
                        g_c.cur.index + 1, g_c.count, mmss(time).s,
                        secsOf((uint32_t)(now - g_c.stoppedUs) / 1000u).s);
            g_c.stoppedUs = 0;
            g_c.replays = 0;
            g_c.resuming = 0;
            // As player.c's watchSong: an M4A download stopped at 1:58 went
            // on at 1:23 (log f3042ca3), and is taken back to where it
            // stopped.
            if (time + 3000 < g_c.playMs || time > g_c.playMs + 3000) {
                const int rcTime = musicSeekTime(g_c.playMs), rcSeek = musicEvent(kEventSeek);
                logLine("carry on: song %d of %d went on at %s, not %s where it stopped; seek "
                        "time 0x%08X, seek 0x%08X",
                        g_c.cur.index + 1, g_c.count, mmss(time).s, mmss(g_c.playMs).s,
                        (unsigned)rcTime, (unsigned)rcSeek);
                g_c.maxTime = time;
                return;
            }
            // It can go on where it stopped and jump back only after: an M4A
            // did, 20 to 37 s, after the look 0.1 s on had seen it where it
            // stopped (log 0790f7dc; looks came every half second up to build
            // 2050). Watched for kBackWatchUs.
            g_c.backFromMs = g_c.playMs;
            g_c.backUs = now;
        } else if (state == 1 && g_c.backUs) {
            const uint32_t since = (uint32_t)(now - g_c.backUs);
            const uint32_t want = g_c.backFromMs + since / 1000u;
            if (since >= kBackWatchUs) {
                g_c.backUs = 0;
            } else if (time + 3000 < want) {
                g_c.backUs = 0;
                const int rcTime = musicSeekTime(want), rcSeek = musicEvent(kEventSeek);
                logLine("carry on: song %d of %d went on at %s, %s s after it played again at %s; "
                        "seek time 0x%08X, seek 0x%08X",
                        g_c.cur.index + 1, g_c.count, mmss(time).s, secsOf(since / 1000u).s,
                        mmss(g_c.backFromMs).s, (unsigned)rcTime, (unsigned)rcSeek);
                g_c.maxTime = time;
                g_c.playMs = want;
                return;
            }
        }
        if (state == 1) g_c.playMs = time;
        // Something else in the shell, as the system's Music app would put
        // there: not this song's length.
        if ((uint32_t)(now - g_c.checkUs) >= 2000000u) {
            g_c.checkUs = now;
            uint32_t len = 0;
            if (musicLength(&len) >= 0 && len > 0 && state == 1 && !hasSong(len, &g_c.cur, 0) &&
                (g_c.lenMs == 0 || len + 3000 < g_c.lenMs || len > g_c.lenMs + 3000)) {
                logLine("carry on: the shell plays a song %s long, not song %d", mmss(len).s,
                        g_c.cur.index + 1);
                carryEnd("something else plays in the shell", kEndOther);
                return;
            }
        }
        // Where the music is, to the server: at each change and every 10 s.
        const char* const told = g_c.menuPaused ? kPaused : kPlaying;
        if (told != g_c.reportState || (uint32_t)(now - g_c.reportUs) >= g_c.reportEveryUs)
            report(&g_c.cur, told, time);
    }
    if ((uint32_t)(now - g_c.statusUs) >= 2000000u) carryStatus(SC_STATE_PLAYING, time);
}

bool probeHolds();
bool ssHolds();

// One look of the music thread's: a queue an app handed over begun, once a
// queue carried on before has stopped, and the probe (probeStep); the queue
// carried on, looked at.
void musicStep() {
    lock();
    const int carry = g_carry;
    // Once a queue carried on before has stopped (SC_CMD_MUSIC_PLAY stops it).
    const bool handed = g_playQueueDue && carry != 2 && !probeHolds() && !ssHolds();
    const int app = g_playQueueApp;
    if (handed) {
        g_playQueueDue = 0;
        g_carryStop = NULL;
    }
    unlock();
    if (handed) {
        if (carryStartQueue(app))
            logLine("play queue: SceShell plays the queue");
        else
            carryNone();
    } else if (carry == 2) {
        carryTick();
    }
}

// How long the music thread waits for its next look: 25 ms while the app's
// song is taken to its place (kFastLookUs), 100 ms while the shell plays a
// song of the queue, half a second else (kLookEveryUs).
SceUInt musicLookUs() {
    if (g_c.fastUntilUs > sceKernelGetSystemTimeWide()) return kFastLookEveryUs;
    lock();
    const int carry = g_carry;
    unlock();
    return carry == 2 && g_c.waitIndex < 0 ? kLookEveryUs : kSlowLookEveryUs;
}

// The music thread: musicStep at each look (musicLookUs), and as soon as
// asked.
class MusicThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        while (!IsCanceled()) {
            SceUInt timeout = musicLookUs();
            sceKernelWaitSema(g_musicSema, 1, &timeout);
            musicStep();
        }
    }
};

// The music thread, made the first time it is wanted, so that a boot with no
// queue carried on has no stack for it in SceShell. true when there is one.
bool startMusicThread() {
    if (g_musicThread) return true;
    if (g_musicSema < 0) {
        g_musicSema = sceKernelCreateSema("ShellCompanion::Music", 0, 0, 1, NULL);
        if (g_musicSema < 0) {
            logLine("music: its semaphore: 0x%08X", (unsigned)g_musicSema);
            return false;
        }
    }
    const int rc = (new MusicThread(kThreadPriority, 0x8000, "ShellCompanion::Music"))->Start();
    if (rc < 0) logLine("music: its thread not started: 0x%08X", (unsigned)rc);
    g_musicThread = rc >= 0;
    return g_musicThread;
}

// Has SceShell stop carrying the queue on, if it does: at its next look, on
// the music thread. Under g_lock.
void stopCarry(const char* why, int how) {
    if (!g_carry || g_carryStop) return;
    g_carryStop = why;
    g_carryStopHow = how;
    logLine("carry on: to stop: %s", why);
    if (g_musicSema >= 0) sceKernelSignalSema(g_musicSema, 1);
}

// ── Probe: does the shell play a WAV that is still being written? ───────────
// (docs/PROTOCOL.md §10) A debug run, for Music Assistant in the background:
// a Sendspin player gets its audio as a stream, and the shell's music
// service plays only files. So the probe writes 44.1 kHz 16-bit stereo PCM
// into a WAV at the clock's pace, kProbeLeadMs ahead of the start, hands the
// file to the service as the queue's songs are handed (carryPlay), and logs
// each second what it has written beside what the service says: its state,
// its clock and the length it gives the song. Then it pauses, plays, seeks
// back, seeks to near the end of what is written (past what the file held
// as it was opened), and skips to a second file, timing each. Its lines in
// the log begin "probe:". It runs only while no queue is carried on, and a
// queue handed over waits for it to stop.
//
// The WAV's header gives either a huge size (SC_PROBE_HUGE), or the size
// written so far, rewritten as it grows (SC_PROBE_GROWING, SC_PROBE_REOPEN);
// SC_PROBE_REOPEN has the shell open the file again where it was each time
// it stops, and every kProbeReopenS, to time the gap. Each write appends with
// the file opened and closed again, so the card's entry has its size.
const uint32_t kProbeLeadMs = 3000;      // written before the shell is given the file
const uint32_t kProbeHugeBytes = 0x7FFFF000u;  // the huge header's data size, 3 h 22 min
const uint32_t kProbeDelayMs = 10000;    // from the command to the first file, to close the app
const uint32_t kProbeEndS = 80;          // a run's length
const uint32_t kProbeReopenS = 10;
const uint32_t kProbeTookMaxMs = 10000;  // a step not seen taking effect by then is logged so
const uint32_t kProbeWavHead = 44;
const uint32_t kProbeBytesPer10Ms = 1764;  // 44100 * 4 / 100

// Under g_lock: the run asked for (SC_PROBE_*, -1 for none), and for whom;
// running; and to stop, which the probe thread reads without it.
int g_probeWant = -1;
int g_probeWantApp;
char g_probeWantSource[160];
int g_probeRunning;
int g_probeStop;  // atomic
SceUID g_probeSema = -1;
int g_probeThread;

// The probe thread's alone.
struct Probe {
    int mode;              // the run's SC_PROBE_*
    char name[48];
    char path[2][160];     // its two files, in the app's folder; SC_PROBE_FILE's file
    int file;              // the one written now and with the shell
    int huge;              // its header gives kProbeHugeBytes, not what is written
    uint32_t bytes;        // PCM written to it
    uint32_t openMs;       // audio in it as the shell was given it
    SceUInt64 beganUs;     // its writing began: kProbeLeadMs is written then, and the clock's
                           // pace after
    int writeErrors;
    // Where the PCM comes from: a WAV on the card (source), looped, or a tone.
    char source[160];
    SceUID src;
    uint32_t srcLeft;      // of its data chunk
    uint32_t pendOff, pendN;  // its PCM read with its header, in g_probeHead, not yet used
    int srcLoops;
    uint32_t samples;      // the tone's so far
    uint16_t phase;
    // What the shell said last, and since when its clock has not moved.
    int state;
    uint32_t timeMs, lenMs;
    SceUInt64 stillUs;
    int stalled;           // logged as stalled, until it moves again
    int paused;            // paused by the probe
    // A step asked of the shell, timed until it plays at its place: what,
    // where it is to play from (within 1 s back, 3 s on), and since when.
    const char* mark;
    uint32_t markMs;
    SceUInt64 markUs;
};
Probe g_p;
uint8_t g_probeBuf[4096];
uint8_t g_probeHead[512];

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint32_t le16(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}
void putLe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
void putLe16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// A WAV's 44-byte header for `data` bytes of 44.1 kHz 16-bit stereo PCM.
void probeHeader(uint8_t* h, uint32_t data) {
    sceClibMemcpy(h, "RIFF", 4);
    putLe32(h + 4, data + 36);
    sceClibMemcpy(h + 8, "WAVEfmt ", 8);
    putLe32(h + 16, 16);
    putLe16(h + 20, 1);  // PCM
    putLe16(h + 22, 2);
    putLe32(h + 24, 44100);
    putLe32(h + 28, 44100 * 4);
    putLe16(h + 32, 4);
    putLe16(h + 34, 16);
    sceClibMemcpy(h + 36, "data", 4);
    putLe32(h + 40, data);
}

// Audio as ms, and ms as audio's bytes, a whole frame each.
uint32_t probeMsOf(uint32_t bytes) {
    return bytes / kProbeBytesPer10Ms * 10u;
}
uint32_t probeBytesOf(uint32_t ms) {
    return ms / 10u * kProbeBytesPer10Ms;
}

// The source WAV opened at its data's start: true when it is 44.1 kHz
// 16-bit stereo PCM, its PCM read with the header kept in g_probeHead.
bool probeSourceOpen() {
    if (g_p.src >= 0) sceIoClose(g_p.src);
    g_p.pendN = 0;
    g_p.src = sceIoOpen(g_p.source, SCE_O_RDONLY, 0);
    if (g_p.src < 0) {
        logLine("probe: %s: open 0x%08X", g_p.source, (unsigned)g_p.src);
        return false;
    }
    const int n = sceIoRead(g_p.src, g_probeHead, sizeof(g_probeHead));
    const uint8_t* h = g_probeHead;
    if (n < 12 || sceClibMemcmp(h, "RIFF", 4) != 0 || sceClibMemcmp(h + 8, "WAVE", 4) != 0) {
        logLine("probe: %s is not a WAV (read %d)", g_p.source, n);
        return false;
    }
    bool fmt = false;
    for (uint32_t off = 12; off + 8 <= (uint32_t)n;) {
        const uint32_t size = le32(h + off + 4);
        if (sceClibMemcmp(h + off, "fmt ", 4) == 0 && off + 24 <= (uint32_t)n) {
            const uint32_t tag = le16(h + off + 8), ch = le16(h + off + 10);
            const uint32_t rate = le32(h + off + 12), bits = le16(h + off + 22);
            if (tag != 1 || ch != 2 || rate != 44100 || bits != 16) {
                logLine("probe: %s is format %u, %u channels, %u Hz, %u bits, not PCM 2, 44100, "
                        "16",
                        g_p.source, (unsigned)tag, (unsigned)ch, (unsigned)rate, (unsigned)bits);
                return false;
            }
            fmt = true;
        } else if (sceClibMemcmp(h + off, "data", 4) == 0) {
            if (!fmt) break;
            g_p.pendOff = off + 8;
            g_p.pendN = (uint32_t)n - g_p.pendOff;
            // An unknown size (0 or all ones) reads to the file's end.
            g_p.srcLeft = size == 0 || size == 0xFFFFFFFFu ? 0xFFFFFFFFu : size & ~3u;
            if (g_p.pendN > g_p.srcLeft) g_p.pendN = g_p.srcLeft;
            return true;
        }
        if (size > (uint32_t)n) break;
        off += 8 + size + (size & 1);
    }
    logLine("probe: %s: no %s in its first %d bytes", g_p.source, fmt ? "data chunk" : "fmt chunk",
            n);
    return false;
}

void probeSourceClose() {
    if (g_p.src >= 0) sceIoClose(g_p.src);
    g_p.src = -1;
}

// n bytes of the tone: 440 Hz, 880 Hz each tenth second, each second's last
// 100 ms silent, so that where it plays can be heard.
void probeTone(uint8_t* out, uint32_t n) {
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        const uint32_t inSecond = g_p.samples % 44100u, second = g_p.samples / 44100u;
        int v = 0;
        if (inSecond < 39690u) {
            g_p.phase = (uint16_t)(g_p.phase + (second % 10u == 0 ? 1307u : 653u));
            const int tri = g_p.phase < 32768u ? g_p.phase : 65535 - g_p.phase;
            v = (tri - 16384) / 4;
        }
        putLe16(out + i, (uint32_t)v);
        putLe16(out + i + 2, (uint32_t)v);
        g_p.samples++;
    }
}

// n bytes of PCM into out (a multiple of 4): from the source, round again
// at its end, else the tone.
void probeFill(uint8_t* out, uint32_t n) {
    uint32_t got = 0;
    int empty = 0;
    while (g_p.src >= 0 && got < n) {
        if (g_p.pendN) {
            uint32_t k = n - got < g_p.pendN ? n - got : g_p.pendN;
            sceClibMemcpy(out + got, g_probeHead + g_p.pendOff, k);
            g_p.pendOff += k;
            g_p.pendN -= k;
            g_p.srcLeft -= k;
            got += k;
            empty = 0;
            continue;
        }
        uint32_t want = n - got < g_p.srcLeft ? n - got : g_p.srcLeft;
        const int r = want ? sceIoRead(g_p.src, out + got, want) : 0;
        if (r > 0) {
            got += (uint32_t)r;
            g_p.srcLeft -= (uint32_t)r;
            empty = 0;
            continue;
        }
        // Its end: round again, unless it has no PCM to give.
        g_p.srcLoops++;
        if (++empty > 1 || !probeSourceOpen()) {
            logLine("probe: %s gives nothing more; the tone instead", g_p.source);
            probeSourceClose();
        }
    }
    got &= ~3u;
    if (got < n) probeTone(out + got, n - got);
}

// The file written up to its clock: kProbeLeadMs and the time since its
// writing began. Its header rewritten after with the size, unless huge.
void probeWrite(SceUInt64 now) {
    const uint32_t want = probeBytesOf(kProbeLeadMs + (uint32_t)(now - g_p.beganUs) / 1000u);
    if (want <= g_p.bytes) return;
    const char* path = g_p.path[g_p.file];
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_APPEND, 0);
    int rc = fd;
    while (fd >= 0 && g_p.bytes < want) {
        const uint32_t n = want - g_p.bytes < sizeof(g_probeBuf) ? want - g_p.bytes
                                                                 : (uint32_t)sizeof(g_probeBuf);
        probeFill(g_probeBuf, n);
        rc = sceIoWrite(fd, g_probeBuf, n);
        if (rc != (int)n) break;
        g_p.bytes += n;
        rc = 0;
    }
    if (fd >= 0) sceIoClose(fd);
    if (rc >= 0 && !g_p.huge) {
        uint8_t h[kProbeWavHead];
        probeHeader(h, g_p.bytes);
        rc = writeAtStart(path, h, sizeof(h));
    }
    if (rc < 0 && g_p.writeErrors++ < 5)
        logLine("probe: writing %s at %u bytes: 0x%08X", path, (unsigned)g_p.bytes, (unsigned)rc);
}

// A file begun: its header, then kProbeLeadMs of audio.
bool probeBegin(int file) {
    g_p.file = file;
    g_p.bytes = 0;
    const char* path = g_p.path[file];
    uint8_t h[kProbeWavHead];
    probeHeader(h, g_p.huge ? kProbeHugeBytes : 0);
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    const int w = fd >= 0 ? sceIoWrite(fd, h, sizeof(h)) : fd;
    if (fd >= 0) sceIoClose(fd);
    if (w != (int)sizeof(h)) {
        logLine("probe: %s not made: 0x%08X", path, (unsigned)w);
        return false;
    }
    g_p.beganUs = sceKernelGetSystemTimeWide();
    probeWrite(g_p.beganUs);
    return true;
}

// A step timed from now until the shell plays from atMs.
void probeMark(const char* what, uint32_t atMs) {
    g_p.mark = what;
    g_p.markMs = atMs;
    g_p.markUs = sceKernelGetSystemTimeWide();
    g_p.stalled = 0;
    g_p.stillUs = 0;
}

// The file written now, to the shell: stop, open and play, as carryPlay
// hands a song over, and from atMs when it is not 0.
void probePlay(const char* why, uint32_t atMs) {
    const char* path = g_p.path[g_p.file];
    const int rcStop = musicEvent(kEventStop), rcOpen = musicOpen(path);
    const int rcTime = atMs ? musicSeekTime(atMs) : 0;
    const int rcPlay = musicEvent(kEventPlay);
    int rcSeek = 0;
    if (atMs) rcSeek = musicEvent(kEventSeek);
    g_p.openMs = probeMsOf(g_p.bytes);
    g_p.paused = 0;
    logLine("probe: %s: %s, %u ms written, from %u ms: stop 0x%08X, open 0x%08X, seek time "
            "0x%08X, play 0x%08X, seek 0x%08X",
            why, path, (unsigned)g_p.openMs, (unsigned)atMs, (unsigned)rcStop, (unsigned)rcOpen,
            (unsigned)rcTime, (unsigned)rcPlay, (unsigned)rcSeek);
    probeMark(why, atMs);
}

void probeSeek(const char* why, uint32_t ms) {
    const int rcTime = musicSeekTime(ms), rcSeek = musicEvent(kEventSeek);
    logLine("probe: %s: to %u ms, %u ms written: seek time 0x%08X, seek 0x%08X", why,
            (unsigned)ms, (unsigned)probeMsOf(g_p.bytes), (unsigned)rcTime, (unsigned)rcSeek);
    probeMark(why, ms);
}

// The shell looked at: a change of state logged, a step timed, and a clock
// that stops while it plays logged as a stall. true when it stalled now.
bool probeLook(SceUInt64 now) {
    int state = 0;
    uint32_t time = 0;
    const int rc = musicStatus(&state, &time);
    if (rc < 0) {
        if (g_p.writeErrors++ < 5) logLine("probe: status 0x%08X", (unsigned)rc);
        return false;
    }
    const int was = g_p.state;
    const uint32_t before = g_p.timeMs;
    g_p.state = state;
    g_p.timeMs = time;
    if (state != was)
        logLine("probe: state %d to %d at %u ms%s", was, state, (unsigned)time,
                g_p.mark || g_p.paused ? "" : ", not asked for");
    if (g_p.mark) {
        const uint32_t since = (uint32_t)(now - g_p.markUs) / 1000u;
        if (state == 1 && time > before && time + 1000 >= g_p.markMs && time <= g_p.markMs + 3000) {
            logLine("probe: %s: playing at %u ms, %u ms after it was asked", g_p.mark,
                    (unsigned)time, (unsigned)since);
            g_p.mark = NULL;
        } else if (since >= kProbeTookMaxMs) {
            logLine("probe: %s: not playing from %u ms %u ms after it was asked: state %d at %u ms",
                    g_p.mark, (unsigned)g_p.markMs, (unsigned)since, state, (unsigned)time);
            g_p.mark = NULL;
        }
        return false;
    }
    if (g_p.paused) return false;
    if (state == 1 && time != before) {
        if (g_p.stalled)
            logLine("probe: moving again at %u ms, %u ms written", (unsigned)time,
                    (unsigned)probeMsOf(g_p.bytes));
        g_p.stalled = 0;
        g_p.stillUs = 0;
        return false;
    }
    if (!g_p.stillUs) g_p.stillUs = now;
    if (g_p.stalled || (uint32_t)(now - g_p.stillUs) < 1500000u) return false;
    g_p.stalled = 1;
    logLine("probe: STALL: state %d at %u ms, its clock still for 1.5 s; %u ms written, %u ms as "
            "the shell opened it",
            state, (unsigned)time, (unsigned)probeMsOf(g_p.bytes), (unsigned)g_p.openMs);
    return true;
}

void probeSecond(uint32_t second) {
    uint32_t len = 0;
    const int rcLen = musicLength(&len);
    g_p.lenMs = len;
    logLine("probe: %u s: %u ms written (%u KB), shell state %d at %u ms, length %u ms (0x%08X)%s",
            (unsigned)second, (unsigned)probeMsOf(g_p.bytes),
            (unsigned)((g_p.bytes + kProbeWavHead) / 1024u), g_p.state, (unsigned)g_p.timeMs,
            (unsigned)len, (unsigned)rcLen, g_p.paused ? ", paused" : "");
}

bool probeStopAsked() {
    return __atomic_load_n(&g_probeStop, __ATOMIC_SEQ_CST) != 0;
}

const char* probeModeName(int mode) {
    return mode == SC_PROBE_HUGE      ? "huge header"
         : mode == SC_PROBE_GROWING   ? "header of the size written"
         : mode == SC_PROBE_REOPEN    ? "reopened"
         : mode == SC_PROBE_FILE      ? "a file as it is"
         : mode == SC_PROBE_HTTP      ? "a request"
                                      : "?";
}

// SC_PROBE_FILE: the file in g_p.source played as it is, for 30 s, as the
// queue plays a local track: does the shell take it (an .m4b, say), how long
// does it say it is, and does it seek? A seek to 60 s at 10 s, a pause at
// 20 s, play at 23 s. The file is left as it was.
void probeFileRun() {
    g_p.huge = 1;  // its header is never touched
    g_p.state = -1;
    g_p.timeMs = 0;
    g_p.mark = NULL;
    g_p.stalled = 0;
    g_p.stillUs = 0;
    g_p.paused = 0;
    g_p.bytes = 0;
    g_p.file = 0;
    copyText(g_p.path[0], g_p.source, sizeof(g_p.path[0]));
    SceIoStat st;
    sceClibMemset(&st, 0, sizeof(st));
    const int rcStat = sceIoGetstat(g_p.path[0], &st);
    const int rcStart = musicTracked(0x30000);
    logLine("probe: run \"%s\": %s, %u KB (stat 0x%08X); music service started: 0x%08X",
            probeModeName(SC_PROBE_FILE), g_p.path[0], (unsigned)((uint64_t)st.st_size >> 10),
            (unsigned)rcStat, (unsigned)rcStart);
    probePlay("first play", 0);
    const SceUInt64 playUs = sceKernelGetSystemTimeWide();
    uint32_t nextSecond = 1;
    int step = 0;
    while (!probeStopAsked()) {
        SceUInt timeout = 100 * 1000;
        sceKernelWaitSema(g_probeSema, 1, &timeout);
        const SceUInt64 now = sceKernelGetSystemTimeWide();
        probeLook(now);
        const uint32_t ms = (uint32_t)(now - playUs) / 1000u, s = ms / 1000u;
        if (ms >= nextSecond * 1000u) probeSecond(nextSecond++);
        if (s >= 30) break;
        if (step == 0 && s >= 10) {
            step++;
            probeSeek("seek to 60 s", 60000);
        } else if (step == 1 && s >= 20) {
            step++;
            const int rc = musicEvent(kEventStop);
            g_p.paused = 1;
            g_p.mark = NULL;
            logLine("probe: pause at %u ms: stop 0x%08X", (unsigned)g_p.timeMs, (unsigned)rc);
        } else if (step == 2 && s >= 23) {
            step++;
            const uint32_t at = g_p.timeMs;
            const int rc = musicEvent(kEventPlay);
            g_p.paused = 0;
            logLine("probe: play again from %u ms: play 0x%08X", (unsigned)at, (unsigned)rc);
            probeMark("play again", at);
        }
    }
    const int rcStop = musicEvent(kEventStop), rcEnd = musicTracked(0x30001);
    logLine("probe: run \"%s\" %s at %u ms, length %u ms: stop 0x%08X, end 0x%08X",
            probeModeName(SC_PROBE_FILE), probeStopAsked() ? "stopped" : "over",
            (unsigned)g_p.timeMs, (unsigned)g_p.lenMs, (unsigned)rcStop, (unsigned)rcEnd);
}

// SC_PROBE_HTTP: the request <folder>/sc-probe-http.txt describes sent once,
// as a report is (serverSend), and its answer's status logged: does the
// firmware send PATCH (sceHttpCreateRequestWithURL2), and does the server
// take it? The file is the queue's kind, its values encoded:
// url=, method=, headers=, body=, type=, end. Read and decoded in place in
// g_probeBuf; the URL, headers and body are never logged.
void probeHttpRun(const char* file) {
    const int n = readFile(file, (char*)g_probeBuf, sizeof(g_probeBuf));
    if (n < 0) {
        logLine("probe: %s: open 0x%08X", file, (unsigned)n);
        return;
    }
    const char *url = "", *method = "", *headers = "", *body = "", *type = "";
    bool whole = false;
    char* text = (char*)g_probeBuf;
    for (char* line = text; *line;) {
        char* end = line;
        while (*end && *end != '\n') end++;
        const bool last = !*end;
        *end = 0;
        int len = (int)(end - line);
        if (len > 0 && line[len - 1] == '\r') line[--len] = 0;
        char* eq = line;
        while (*eq && *eq != '=') eq++;
        if (*eq) {
            *eq = 0;
            char* v = eq + 1;
            urlDecode(v, (int)(line + len - v), v, (int)(line + len - v) + 1, true);
            if (same(line, "url")) url = v;
            else if (same(line, "method")) method = v;
            else if (same(line, "headers")) headers = v;
            else if (same(line, "body")) body = v;
            else if (same(line, "type")) type = v;
        } else if (same(line, "end")) {
            whole = true;
        }
        if (last) break;
        line = end + 1;
    }
    if (!whole || !url[0]) {
        logLine("probe: %s is %s", file, !whole ? "not whole (no end line)" : "without a url");
        return;
    }
    if (!g_netFound) g_netFound = findLibraries("probe");
    logLine("probe: run \"%s\": %s, %u bytes of body; sceHttpCreateRequestWithURL2 %s",
            probeModeName(SC_PROBE_HTTP), method[0] ? method : "GET",
            (unsigned)sceClibStrnlen(body, sizeof(g_probeBuf)),
            g_httpRequest2.fn ? "found" : "missing");
    int rcSsl = -1, rcHttp = -1;
    if (!g_netFound || !httpStart("probe", &rcSsl, &rcHttp)) {
        logLine("probe: no HTTP library");
        return;
    }
    const int tmpl = httpTemplate("probe");
    if (tmpl >= 0) {
        HttpAsk ask = getWith(headers);
        ask.method = method;
        if (body[0]) {
            ask.body = body;
            ask.bodyLen = (uint32_t)sceClibStrnlen(body, sizeof(g_probeBuf));
            ask.type = type[0] ? type : "application/json";
        }
        int conn, req, status = 0;
        unsigned long long length = 0;
        const int rc = httpOpen(tmpl, "probe", "the request", url, ask, &conn, &req);
        if (rc >= 0) {
            httpFn<FnStatus>(kHttpStatus)(req, &status);
            httpFn<FnLength>(kHttpLength)(req, &length);
            httpClose(conn, req);
        }
        logLine("probe: the request: 0x%08X, HTTP %d, %u bytes of answer", (unsigned)rc, status,
                (unsigned)length);
        httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
    }
    httpEnd(rcSsl, rcHttp);
    sceClibMemset(g_probeBuf, 0, sizeof(g_probeBuf));
}

// One run of a mode, for kProbeEndS from the first play. Its script, by the
// second: SC_PROBE_HUGE and SC_PROBE_GROWING pause at 15, play at 20, seek
// back to 5 s at 30, seek to 2 s short of what is written at 40, and skip to
// the second file at 55; SC_PROBE_REOPEN opens the file again where it was
// each time it stalls and every kProbeReopenS.
void probeRun(int mode) {
    g_p.mode = mode;
    g_p.huge = mode == SC_PROBE_HUGE;
    g_p.state = -1;
    g_p.timeMs = 0;
    g_p.mark = NULL;
    g_p.stalled = 0;
    g_p.stillUs = 0;
    g_p.paused = 0;
    g_p.samples = 0;
    g_p.phase = 0;
    if (g_p.source[0] && !probeSourceOpen()) probeSourceClose();
    const int rcStart = musicTracked(0x30000);
    logLine("probe: run \"%s\" (mode %d), from %s, %u ms ahead; music service started: 0x%08X",
            probeModeName(mode), mode, g_p.src >= 0 ? g_p.source : "the tone",
            (unsigned)kProbeLeadMs, (unsigned)rcStart);
    if (!probeBegin(0)) {
        musicTracked(0x30001);
        probeSourceClose();
        return;
    }
    probePlay("first play", 0);
    const SceUInt64 playUs = sceKernelGetSystemTimeWide();
    uint32_t nextSecond = 1, lastReopenS = 0;
    int step = 0;
    while (!probeStopAsked()) {
        SceUInt timeout = 100 * 1000;
        sceKernelWaitSema(g_probeSema, 1, &timeout);
        const SceUInt64 now = sceKernelGetSystemTimeWide();
        probeWrite(now);
        const bool stalled = probeLook(now);
        const uint32_t ms = (uint32_t)(now - playUs) / 1000u, s = ms / 1000u;
        if (ms >= nextSecond * 1000u) probeSecond(nextSecond++);
        if (s >= kProbeEndS) break;
        if (mode == SC_PROBE_REOPEN) {
            if ((stalled || s >= lastReopenS + kProbeReopenS) && !g_p.mark) {
                lastReopenS = s;
                probePlay(stalled ? "reopened, stalled" : "reopened, on time", g_p.timeMs);
            }
            continue;
        }
        static const uint32_t kAt[] = {15, 20, 30, 40, 55};
        if (step >= 5 || s < kAt[step]) continue;
        switch (step++) {
        case 0: {
            const int rc = musicEvent(kEventStop);
            g_p.paused = 1;
            g_p.mark = NULL;
            logLine("probe: pause at %u ms: stop 0x%08X", (unsigned)g_p.timeMs, (unsigned)rc);
            break;
        }
        case 1: {
            const uint32_t at = g_p.timeMs;
            const int rc = musicEvent(kEventPlay);
            g_p.paused = 0;
            logLine("probe: play again from %u ms: play 0x%08X", (unsigned)at, (unsigned)rc);
            probeMark("play again", at);
            break;
        }
        case 2:
            probeSeek("seek back", 5000);
            break;
        case 3:
            probeSeek("seek to near what is written", probeMsOf(g_p.bytes) - 2000);
            break;
        case 4: {
            const int old = g_p.file;
            if (probeBegin(1 - old)) {
                probePlay("skip to the second file", 0);
                sceIoRemove(g_p.path[old]);
            }
            break;
        }
        }
    }
    const int rcStop = musicEvent(kEventStop), rcEnd = musicTracked(0x30001);
    probeSourceClose();
    sceIoRemove(g_p.path[0]);
    sceIoRemove(g_p.path[1]);
    logLine("probe: run \"%s\" %s at %u ms, %u ms written: stop 0x%08X, end 0x%08X",
            probeModeName(mode), probeStopAsked() ? "stopped" : "over", (unsigned)g_p.timeMs,
            (unsigned)probeMsOf(g_p.bytes), (unsigned)rcStop, (unsigned)rcEnd);
}

// The run asked for: SC_PROBE_ALL runs the other three in turn.
void probeStep() {
    lock();
    const int mode = g_probeWant;
    const int app = g_probeWantApp;
    const App* a = appAt(app);
    const bool carry = g_carry != 0;
    const bool go = mode > 0 && a && !carry;
    g_probeWant = -1;
    if (go) {
        g_probeRunning = 1;
        sceClibSnprintf(g_p.path[0], sizeof(g_p.path[0]), "%s/sc-probe-a.wav", a->folder);
        sceClibSnprintf(g_p.path[1], sizeof(g_p.path[1]), "%s/sc-probe-b.wav", a->folder);
        copyText(g_p.name, a->name, sizeof(g_p.name));
        copyText(g_p.source, g_probeWantSource, sizeof(g_p.source));
    }
    unlock();
    if (mode < 0) return;
    if (!go) {
        logLine("probe: mode %d not run%s", mode, carry ? ": a queue is carried on" : "");
        return;
    }
    g_p.src = -1;
    g_p.writeErrors = 0;
    if (mode == SC_PROBE_HTTP) {
        char file[160];
        lock();
        const App* who = appAt(app);
        if (who) appPath(who, "sc-probe-http.txt", file, sizeof(file));
        unlock();
        logLine("probe: for %s, mode %d", g_p.name, mode);
        if (who) probeHttpRun(file);
    } else if (mode == SC_PROBE_FILE && !g_p.source[0]) {
        logLine("probe: mode %d wants a file", mode);
    } else if (findSvcObj("probe") && g_getSvcObj()) {
        logLine("probe: for %s, mode %d, in %u s", g_p.name, mode, (unsigned)(kProbeDelayMs / 1000));
        SceUInt timeout = kProbeDelayMs * 1000u;
        sceKernelWaitSema(g_probeSema, 1, &timeout);
        const int first = mode == SC_PROBE_ALL ? SC_PROBE_HUGE : mode;
        const int last = mode == SC_PROBE_ALL ? SC_PROBE_REOPEN : mode;
        if (mode == SC_PROBE_FILE && !probeStopAsked()) probeFileRun();
        for (int m = first; m <= last && mode != SC_PROBE_FILE && !probeStopAsked(); m++) {
            probeRun(m);
            SceUInt pause = 3000 * 1000;
            if (m < last) sceKernelWaitSema(g_probeSema, 1, &pause);
        }
    } else {
        logLine("probe: no client for SceShell's music service");
    }
    lock();
    g_probeRunning = 0;
    __atomic_store_n(&g_probeStop, 0, __ATOMIC_SEQ_CST);
    // A queue handed over meanwhile goes now.
    if (g_playQueueDue && g_musicSema >= 0) sceKernelSignalSema(g_musicSema, 1);
    unlock();
    logLine("probe: done");
}

class ProbeThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        while (!IsCanceled()) {
            sceKernelWaitSema(g_probeSema, 1, NULL);
            probeStep();
        }
    }
};

// The probe runs, or is about to: a queue handed over waits. Under g_lock.
bool probeHolds() {
    return g_probeRunning || g_probeWant >= 0;
}

// Has the probe stop, if it runs, or not begin. Under g_lock.
void probeStop(const char* why) {
    if (g_probeWant >= 0) {
        g_probeWant = -1;
        logLine("probe: not to run: %s", why);
    }
    if (!g_probeRunning || __atomic_load_n(&g_probeStop, __ATOMIC_SEQ_CST)) return;
    __atomic_store_n(&g_probeStop, 1, __ATOMIC_SEQ_CST);
    logLine("probe: to stop: %s", why);
    sceKernelSignalSema(g_probeSema, 1);
}

// SC_CMD_PROBE_STREAM from an app. Under g_lock.
void probeAsk(int app, uint32_t mode, const char* source) {
    if (mode == SC_PROBE_STOP) {
        probeStop("asked");
        return;
    }
    if (mode > SC_PROBE_HTTP) {
        logLine("probe: no mode %u", (unsigned)mode);
        return;
    }
    if (g_probeRunning || g_probeWant >= 0) {
        logLine("probe: one runs already");
        return;
    }
    if (g_carry || g_playQueueDue) {
        logLine("probe: not while a queue is carried on");
        return;
    }
    if (ssHolds()) {
        logLine("probe: not while the plugin is the Sendspin player");
        return;
    }
    if (g_probeSema < 0) g_probeSema = sceKernelCreateSema("ShellCompanion::Probe", 0, 0, 1, NULL);
    if (g_probeSema < 0) {
        logLine("probe: its semaphore: 0x%08X", (unsigned)g_probeSema);
        return;
    }
    if (!g_probeThread) {
        const int rc = (new ProbeThread(kThreadPriority, kFetchStack, "ShellCompanion::Probe"))->Start();
        if (rc < 0) {
            logLine("probe: its thread not started: 0x%08X", (unsigned)rc);
            return;
        }
        g_probeThread = 1;
    }
    g_probeWant = (int)mode;
    g_probeWantApp = app;
    copyText(g_probeWantSource, source, sizeof(g_probeWantSource));
    sceKernelSignalSema(g_probeSema, 1);
}

// ── Remote control: the listener ────────────────────────────────────────────
// A thread of its own while SceShell carries a queue on that asked for one
// (g_ctl): it answers HTTP on g_ctl.port and discovery searches on UDP, and
// pushes the queue's state to the remotes that subscribed. One request at a
// time, each read and answered whole: remotes ask little and seldom. Through
// SceShell's own SceNet, found with taiHEN as the HTTP library is, so that a
// firmware without some of it keeps the rest of the plugin. Its lines in the
// log begin "control:"; no URL or header line is logged.
const uint32_t kNetLibrary = 0x6BF8B2A2;  // SceNet, in module SceNet
enum { kNetSocket, kNetBind, kNetListen, kNetAccept, kNetRecv, kNetSend, kNetSendto, kNetRecvfrom,
       kNetSetsockopt, kNetClose, kNetFns };
LibFn g_net[kNetFns] = {{"sceNetSocket", 0xF084FCE3, 0},     {"sceNetBind", 0x1296A94B, 0},
                        {"sceNetListen", 0x7A8DA094, 0},     {"sceNetAccept", 0x1ADF9BB1, 0},
                        {"sceNetRecv", 0x023643B7, 0},       {"sceNetSend", 0xE3DD8CD9, 0},
                        {"sceNetSendto", 0x52DB31D5, 0},     {"sceNetRecvfrom", 0xB226138B, 0},
                        {"sceNetSetsockopt", 0x065505CA, 0}, {"sceNetSocketClose", 0x29822B4D, 0}};
// As psp2common/net.h has them.
struct NetAddrIn {
    uint8_t len, family;
    uint16_t port;     // network order
    uint32_t addr;     // network order
    uint16_t vport;
    char zero[6];
};
static_assert(sizeof(NetAddrIn) == 16, "SceNetSockaddrIn");
const int kAfInet = 2, kSockStream = 1, kSockDgram = 2;
const int kSolSocket = 0xFFFF, kSoReuseAddr = 4, kSoSndTimeo = 0x1005, kSoRcvTimeo = 0x1006,
          kSoNbio = 0x1100, kIpProtoIp = 0, kIpAddMembership = 12;
typedef int (*FnSocket)(const char*, int, int, int);
typedef int (*FnBind)(int, const void*, unsigned);
typedef int (*FnListen)(int, int);
typedef int (*FnAccept)(int, void*, unsigned*);
typedef int (*FnRecv)(int, void*, unsigned, int);
typedef int (*FnNetSend)(int, const void*, unsigned, int);
typedef int (*FnSendto)(int, const void*, unsigned, int, const void*, unsigned);
typedef int (*FnRecvfrom)(int, void*, unsigned, int, void*, unsigned*);
typedef int (*FnSetsockopt)(int, int, int, const void*, unsigned);
typedef int (*FnClose)(int);
template <typename F>
F netFn(int i) {
    return (F)g_net[i].fn;
}

bool netFind() {
    for (int i = 0; i < kNetFns; i++) {
        uintptr_t fn = 0;
        const int rc = scGetModuleExportFunc("SceNet", kNetLibrary, g_net[i].nid, &fn);
        g_net[i].fn = rc >= 0 ? fn : 0;
        if (rc < 0) {
            logLine("control: %s not in SceNet: 0x%08X", g_net[i].name, (unsigned)rc);
            return false;
        }
    }
    return true;
}

uint16_t netOrder16(int v) {
    return (uint16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF));
}

// "a.b.c.d" as an address in network order into *out. true when it is one.
bool parseIp(const char* s, uint32_t* out) {
    uint32_t parts[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return false;
        parts[i] = toUint(s);
        if (parts[i] > 255) return false;
        while (*s >= '0' && *s <= '9') s++;
        if (i < 3 && *s++ != '.') return false;
    }
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return true;
}

void ipText(uint32_t addr, char* out, int cap) {
    sceClibSnprintf(out, cap, "%u.%u.%u.%u", (unsigned)(addr & 0xFF), (unsigned)((addr >> 8) & 0xFF),
                    (unsigned)((addr >> 16) & 0xFF), (unsigned)(addr >> 24));
}

int setOpt(int sock, int level, int name, int value) {
    return netFn<FnSetsockopt>(kNetSetsockopt)(sock, level, name, &value, sizeof(value));
}

// The value of query parameter name, %XX and '+' decoded, into out. true
// when it is there.
bool queryParam(const char* query, const char* name, char* out, int cap) {
    const int nlen = (int)sceClibStrnlen(name, 40);
    for (const char* p = query; p && *p;) {
        if (startsWith(p, name) && p[nlen] == '=') {
            const char* v = p + nlen + 1;
            int n = 0;
            while (v[n] && v[n] != '&') n++;
            urlDecode(v, n, out, cap, false);
            for (char* c = out; *c; c++)
                if (*c == '+') *c = ' ';
            return true;
        }
        while (*p && *p != '&') p++;
        if (*p) p++;
    }
    if (cap) out[0] = 0;
    return false;
}

// Remotes that asked for the queue's state to be pushed to them.
const int kSubscribers = 4;
const uint32_t kSubscriberIdleMs = 90000;
struct Subscriber {
    uint32_t addr;        // network order; 0 for a free slot
    int port;
    char commandID[16];
    SceUInt64 seenUs;     // its last request
    int failures;         // pushes in a row it did not take
};
// The listener thread's alone, all of them.
Subscriber g_subs[kSubscribers];
char g_ctlReq[4096];
char g_ctlBody[2048];
char g_ctlOut[3072];
char g_ctlUrl[512];
Track g_ctlTrack;
const char* g_ctlTrackState;
uint32_t g_ctlPos, g_ctlDur, g_ctlSerial;

// The queue's state as the music thread last wrote it, into g_ctlTrack.
void ctlSnapshot() {
    lock();
    sceClibSnprintf(g_ctlTrack.id, sizeof(g_ctlTrack.id), "%s", g_ctlState.id);
    g_ctlTrack.index = g_ctlState.index;
    g_ctlTrack.durationMs = g_ctlState.durationMs;
    g_ctlTrackState = g_ctlState.state ? g_ctlState.state : kStopped;
    g_ctlPos = g_ctlState.positionMs;
    g_ctlDur = g_ctlState.durationMs;
    g_ctlSerial = g_ctlState.serial;
    unlock();
}

const char* replyNamed(const char* name) {
    for (int i = 0; i < g_ctl.replyCount; i++)
        if (same(g_ctl.replies[i].name, name)) return g_ctl.replies[i].body;
    return NULL;
}

// All of data sent on sock. false when it would not go.
bool sendAll(int sock, const char* data, int n) {
    while (n > 0) {
        const int sent = netFn<FnNetSend>(kNetSend)(sock, data, (unsigned)n, 0);
        if (sent <= 0) return false;
        data += sent;
        n -= sent;
    }
    return true;
}

// g_ctl.headers as \r\n-ended lines appended to out at n: its new length.
int putHeaderLines(char* out, int n, int cap) {
    for (const char* p = g_ctl.headers; *p && n < cap - 3;) {
        while (*p && *p != '\r' && *p != '\n' && n < cap - 3) out[n++] = *p++;
        out[n++] = '\r';
        out[n++] = '\n';
        while (*p == '\r' || *p == '\n') p++;
    }
    return n;
}

// A whole answer on sock: status, the app's header lines, extra lines, and
// body.
void respond(int sock, int status, const char* extra, const char* body) {
    const int blen = (int)sceClibStrnlen(body, sizeof(g_ctlBody));
    int n = sceClibSnprintf(g_ctlOut, sizeof(g_ctlOut),
                            "HTTP/1.1 %d %s\r\nContent-Length: %d\r\nConnection: close\r\n", status,
                            status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request", blen);
    n = putHeaderLines(g_ctlOut, n, (int)sizeof(g_ctlOut) - 2);
    if (extra) n += sceClibSnprintf(g_ctlOut + n, sizeof(g_ctlOut) - n, "%s", extra);
    if (n > (int)sizeof(g_ctlOut) - 3) n = (int)sizeof(g_ctlOut) - 3;
    g_ctlOut[n++] = '\r';
    g_ctlOut[n++] = '\n';
    if (sendAll(sock, g_ctlOut, n)) sendAll(sock, body, blen);
}

// A subscriber by its address, made when make; NULL when none (or full).
Subscriber* subscriberOf(uint32_t addr, bool make) {
    Subscriber* free = NULL;
    for (int i = 0; i < kSubscribers; i++) {
        if (g_subs[i].addr == addr) return &g_subs[i];
        if (!g_subs[i].addr && !free) free = &g_subs[i];
    }
    if (!make || !free) return NULL;
    sceClibMemset(free, 0, sizeof(*free));
    free->addr = addr;
    return free;
}

// A route's commands carried out for a request with this query from addr.
// *wait set when it says to wait for a change before replying.
void runCommands(const char* cmds, const char* query, uint32_t addr, bool* wait) {
    char one[48], arg[64];
    // A skip and a seek together (Plex's playMedia) are one command: the
    // track begun at the place, as the seek alone would come before it plays.
    RemoteCmd skip;
    sceClibMemset(&skip, 0, sizeof(skip));
    uint32_t seekMs = 0;
    bool seek = false;
    for (const char* p = cmds; *p;) {
        int n = 0;
        while (*p && *p != ',') {
            if (n < (int)sizeof(one) - 1) one[n++] = *p;
            p++;
        }
        if (*p) p++;
        one[n] = 0;
        char* colon = NULL;
        for (char* c = one; *c && !colon; c++)
            if (*c == ':') colon = c;
        if (colon) *colon = 0;
        const char* name = one;
        const bool has = colon && queryParam(query, colon + 1, arg, sizeof(arg)) && arg[0];
        RemoteCmd rc;
        sceClibMemset(&rc, 0, sizeof(rc));
        if (same(name, "play")) rc.cmd = kRcPlay;
        else if (same(name, "pause")) rc.cmd = kRcPause;
        else if (same(name, "toggle")) rc.cmd = kRcToggle;
        else if (same(name, "next")) rc.cmd = kRcNext;
        else if (same(name, "previous")) rc.cmd = kRcPrevious;
        else if (same(name, "stop")) rc.cmd = kRcStop;
        else if (same(name, "seekTo") && has) {
            seek = true;
            seekMs = toUint(arg);
        } else if (same(name, "forward") || same(name, "back")) {
            rc.cmd = same(name, "forward") ? kRcForward : kRcBack;
            rc.ms = has ? toUint(arg) : 0;
        } else if (same(name, "skipTo") && has) {
            skip.cmd = kRcSkipTo;
            copyText(skip.id, arg, sizeof(skip.id));
        } else if (same(name, "subscribe")) {
            Subscriber* sub = subscriberOf(addr, true);
            if (sub) {
                sub->port = has ? (int)toUint(arg) : 0;
                sub->seenUs = sceKernelGetSystemTimeWide();
                sub->failures = 0;
                queryParam(query, "commandID", sub->commandID, sizeof(sub->commandID));
                char ip[16];
                ipText(addr, ip, sizeof(ip));
                logLine("control: %s subscribes, port %d", ip, sub->port);
            }
        } else if (same(name, "unsubscribe")) {
            Subscriber* sub = subscriberOf(addr, false);
            if (sub) sub->addr = 0;
        } else if (same(name, "wait")) {
            *wait = true;
        }
        if (rc.cmd) {
            lock();
            const bool queued = remotePush(rc);
            unlock();
            if (!queued) logLine("control: command %s not taken: too many waiting", name);
        }
    }
    if (skip.cmd) {
        skip.ms = seek ? seekMs : 0;
    } else if (seek) {
        skip.cmd = kRcSeekTo;
        skip.ms = seekMs;
    }
    if (skip.cmd) {
        lock();
        const bool queued = remotePush(skip);
        unlock();
        if (!queued) logLine("control: a skip or seek not taken: too many waiting");
    }
}

// One connection: its request read, its route's commands carried out, and
// its reply sent.
void handleConnection(int sock, uint32_t addr) {
    setOpt(sock, kSolSocket, kSoNbio, 0);
    setOpt(sock, kSolSocket, kSoRcvTimeo, 2000000);
    setOpt(sock, kSolSocket, kSoSndTimeo, 2000000);
    int n = 0;
    for (;;) {
        const int got = netFn<FnRecv>(kNetRecv)(sock, g_ctlReq + n, sizeof(g_ctlReq) - 1 - n, 0);
        if (got <= 0) break;
        n += got;
        g_ctlReq[n] = 0;
        if (findText(g_ctlReq, "\r\n\r\n") || n >= (int)sizeof(g_ctlReq) - 1) break;
    }
    g_ctlReq[n] = 0;
    // "METHOD /path?query HTTP/1.1"
    char method[12], path[128];
    static char query[1024];
    int i = 0, k = 0;
    while (g_ctlReq[i] && g_ctlReq[i] != ' ' && k < (int)sizeof(method) - 1) method[k++] = g_ctlReq[i++];
    method[k] = 0;
    while (g_ctlReq[i] == ' ') i++;
    k = 0;
    while (g_ctlReq[i] && g_ctlReq[i] != ' ' && g_ctlReq[i] != '?' && k < (int)sizeof(path) - 1)
        path[k++] = g_ctlReq[i++];
    path[k] = 0;
    k = 0;
    if (g_ctlReq[i] == '?') {
        i++;
        while (g_ctlReq[i] && g_ctlReq[i] != ' ' && k < (int)sizeof(query) - 1) query[k++] = g_ctlReq[i++];
    }
    query[k] = 0;
    if (!method[0] || path[0] != '/') {
        respond(sock, 400, NULL, "");
        return;
    }
    // A subscriber's every request may bring it a new command id.
    Subscriber* sub = subscriberOf(addr, false);
    if (sub) {
        sub->seenUs = sceKernelGetSystemTimeWide();
        char id[16];
        if (queryParam(query, "commandID", id, sizeof(id))) copyText(sub->commandID, id, sizeof(sub->commandID));
    }
    if (same(method, "OPTIONS")) {
        // A browser's preflight: what it asks for, allowed.
        static char extra[512];
        char asked[256];
        asked[0] = 0;
        const char* h = findText(g_ctlReq, "Access-Control-Request-Headers:");
        if (h) {
            h += 31;
            while (*h == ' ') h++;
            int a = 0;
            while (h[a] && h[a] != '\r' && h[a] != '\n' && a < (int)sizeof(asked) - 1) {
                asked[a] = h[a];
                a++;
            }
            asked[a] = 0;
        }
        sceClibSnprintf(extra, sizeof(extra),
                        "Access-Control-Allow-Methods: POST, GET, OPTIONS, DELETE, PUT, HEAD\r\n"
                        "Access-Control-Max-Age: 1209600\r\n%s%s%s",
                        asked[0] ? "Access-Control-Allow-Headers: " : "", asked,
                        asked[0] ? "\r\n" : "");
        respond(sock, 200, extra, "");
        return;
    }
    const Route* route = NULL;
    for (int r = 0; r < g_ctl.routeCount && !route; r++)
        if (same(g_ctl.routes[r].path, path)) route = &g_ctl.routes[r];
    bool wait = false;
    if (route) runCommands(route->cmds, query, addr, &wait);
    ctlSnapshot();
    // Wait for the state to change (a remote's long poll), a second at most,
    // so that one remote never holds the listener up for long.
    if (wait) {
        const uint32_t was = g_ctlSerial;
        for (int t = 0; t < 20 && g_ctlSerial == was; t++) {
            sceKernelDelayThread(50 * 1000);
            ctlSnapshot();
        }
    }
    const char* tmpl = route ? replyNamed(route->reply) : replyNamed(g_ctl.notFound);
    char ip[16], cmdId[16];
    ipText(addr, ip, sizeof(ip));
    queryParam(query, "commandID", cmdId, sizeof(cmdId));
    Extra x = {query, cmdId, ip, 0};
    if (!tmpl || !expand(tmpl, &g_ctlTrack, g_ctlTrackState, g_ctlPos, g_ctlDur, g_ctlBody,
                         sizeof(g_ctlBody), &x))
        g_ctlBody[0] = 0;
    respond(sock, route || tmpl ? 200 : 404, NULL, g_ctlBody);
}

// A UDP datagram of text to "a.b.c.d:port".
void sendDatagram(int udp, const char* to, const char* text) {
    char host[24];
    int k = 0;
    while (to[k] && to[k] != ':' && k < (int)sizeof(host) - 1) {
        host[k] = to[k];
        k++;
    }
    host[k] = 0;
    NetAddrIn a;
    sceClibMemset(&a, 0, sizeof(a));
    if (to[k] != ':' || !parseIp(host, &a.addr)) return;
    a.len = sizeof(a);
    a.family = kAfInet;
    a.port = netOrder16((int)toUint(to + k + 1));
    netFn<FnSendto>(kNetSendto)(udp, text, (unsigned)sceClibStrnlen(text, 1024), 0, &a, sizeof(a));
}

// The queue's state to each subscriber, as control.push says. Those idle
// too long, or that took none of three pushes in a row, are dropped.
void pushAll(int tmpl) {
    const char* body = replyNamed(g_ctl.pushReply);
    const SceUInt64 now = sceKernelGetSystemTimeWide();
    ctlSnapshot();
    for (int i = 0; i < kSubscribers; i++) {
        Subscriber* sub = &g_subs[i];
        if (!sub->addr) continue;
        char ip[16];
        ipText(sub->addr, ip, sizeof(ip));
        if ((uint32_t)(now - sub->seenUs) / 1000u > kSubscriberIdleMs || sub->failures >= 3) {
            logLine("control: %s dropped (%s)", ip, sub->failures >= 3 ? "it took no pushes" : "idle");
            sub->addr = 0;
            continue;
        }
        Extra x = {NULL, sub->commandID, ip, sub->port};
        if (!expand(g_ctl.pushUrl, &g_ctlTrack, g_ctlTrackState, g_ctlPos, g_ctlDur, g_ctlUrl,
                    sizeof(g_ctlUrl), &x) ||
            !expand(body ? body : "", &g_ctlTrack, g_ctlTrackState, g_ctlPos, g_ctlDur, g_ctlBody,
                    sizeof(g_ctlBody), &x))
            continue;
        HttpAsk ask = getWith(g_ctl.headers);
        ask.method = g_ctl.pushMethod;
        ask.body = g_ctlBody;
        ask.bodyLen = (uint32_t)sceClibStrnlen(g_ctlBody, sizeof(g_ctlBody));
        if (g_ctl.pushType[0]) ask.type = g_ctl.pushType;
        int conn, req, status = 0;
        const int rc = httpOpen(tmpl, NULL, NULL, g_ctlUrl, ask, &conn, &req);
        if (rc >= 0) {
            httpFn<FnStatus>(kHttpStatus)(req, &status);
            httpClose(conn, req);
        }
        sub->failures = status >= 200 && status < 300 ? 0 : sub->failures + 1;
    }
}

bool ctlStopping() {
    lock();
    const bool stop = g_ctlStop != 0;
    unlock();
    return stop;
}

int ctlThread(SceSize, void*) {
    int tcp = -1, udp = -1, tmpl = -1, rcSsl = -1, rcHttp = -1;
    sceClibMemset(g_subs, 0, sizeof(g_subs));
    bool ok = netFind();
    if (ok) {
        tcp = netFn<FnSocket>(kNetSocket)("ShellCompanion::Control", kAfInet, kSockStream, 0);
        NetAddrIn a;
        sceClibMemset(&a, 0, sizeof(a));
        a.len = sizeof(a);
        a.family = kAfInet;
        a.port = netOrder16(g_ctl.port);
        int rc = tcp;
        if (tcp >= 0) {
            setOpt(tcp, kSolSocket, kSoReuseAddr, 1);
            rc = netFn<FnBind>(kNetBind)(tcp, &a, sizeof(a));
            if (rc >= 0) rc = netFn<FnListen>(kNetListen)(tcp, 4);
            if (rc >= 0) rc = setOpt(tcp, kSolSocket, kSoNbio, 1);
        }
        ok = rc >= 0;
        logLine("control: listening on port %d: 0x%08X", g_ctl.port, (unsigned)rc);
    }
    if (ok && g_ctl.discoveryPort) {
        udp = netFn<FnSocket>(kNetSocket)("ShellCompanion::Discovery", kAfInet, kSockDgram, 0);
        NetAddrIn a;
        sceClibMemset(&a, 0, sizeof(a));
        a.len = sizeof(a);
        a.family = kAfInet;
        a.port = netOrder16(g_ctl.discoveryPort);
        int rc = udp, rcJoin = 0;
        if (udp >= 0) {
            setOpt(udp, kSolSocket, kSoReuseAddr, 1);
            rc = netFn<FnBind>(kNetBind)(udp, &a, sizeof(a));
            uint32_t group[2] = {0, 0};  // the group, then any interface
            if (rc >= 0 && g_ctl.discoveryGroup[0] && parseIp(g_ctl.discoveryGroup, &group[0]))
                rcJoin = netFn<FnSetsockopt>(kNetSetsockopt)(udp, kIpProtoIp, kIpAddMembership, group,
                                                              sizeof(group));
            if (rc >= 0) rc = setOpt(udp, kSolSocket, kSoNbio, 1);
        }
        logLine("control: discovery on UDP port %d: 0x%08X, group 0x%08X", g_ctl.discoveryPort,
                (unsigned)rc, (unsigned)rcJoin);
        if (rc < 0 && udp >= 0) {
            netFn<FnClose>(kNetClose)(udp);
            udp = -1;
        }
        if (udp >= 0 && g_ctl.helloTo[0] && g_ctl.hello[0]) sendDatagram(udp, g_ctl.helloTo, g_ctl.hello);
    }
    if (ok && g_ctl.pushUrl[0]) {
        if (!g_netFound) g_netFound = findLibraries("control");
        if (g_netFound && httpStart("control", &rcSsl, &rcHttp)) tmpl = httpTemplate("control", true);
    }
    const uint32_t every = g_ctl.pushEveryMs >= 500 ? g_ctl.pushEveryMs : 1000;
    SceUInt64 pushedUs = 0;
    uint32_t pushedSerial = 0;
    static char dgram[1024];
    while (ok && !ctlStopping()) {
        NetAddrIn from;
        unsigned fromLen = sizeof(from);
        const int conn = netFn<FnAccept>(kNetAccept)(tcp, &from, &fromLen);
        if (conn >= 0) {
            handleConnection(conn, from.addr);
            netFn<FnClose>(kNetClose)(conn);
            continue;
        }
        if (udp >= 0) {
            fromLen = sizeof(from);
            const int got =
                netFn<FnRecvfrom>(kNetRecvfrom)(udp, dgram, sizeof(dgram) - 1, 0, &from, &fromLen);
            if (got > 0) {
                dgram[got] = 0;
                if (g_ctl.discoveryReply[0] && startsWith(dgram, g_ctl.discoveryMatch)) {
                    ctlSnapshot();
                    if (expand(g_ctl.discoveryReply, &g_ctlTrack, g_ctlTrackState, g_ctlPos, g_ctlDur,
                               g_ctlBody, sizeof(g_ctlBody)))
                        netFn<FnSendto>(kNetSendto)(udp, g_ctlBody,
                                                    (unsigned)sceClibStrnlen(g_ctlBody, sizeof(g_ctlBody)),
                                                    0, &from, fromLen);
                }
                continue;
            }
        }
        const SceUInt64 now = sceKernelGetSystemTimeWide();
        if (tmpl >= 0) {
            lock();
            const uint32_t serial = g_ctlState.serial;
            unlock();
            if (serial != pushedSerial || (uint32_t)(now - pushedUs) / 1000u >= every) {
                pushedSerial = serial;
                pushedUs = now;
                pushAll(tmpl);
            }
        }
        sceKernelDelayThread(50 * 1000);
    }
    // The remotes told the queue has stopped, as a last push, and goodbye.
    if (tmpl >= 0) {
        lock();
        g_ctlState.state = kStopped;
        g_ctlState.serial++;
        unlock();
        pushAll(tmpl);
        httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
    }
    httpEnd(rcSsl, rcHttp);
    if (udp >= 0 && g_ctl.helloTo[0] && g_ctl.bye[0]) sendDatagram(udp, g_ctl.helloTo, g_ctl.bye);
    if (udp >= 0) netFn<FnClose>(kNetClose)(udp);
    if (tcp >= 0) netFn<FnClose>(kNetClose)(tcp);
    logLine("control: stopped");
    lock();
    g_ctlRunning = 0;
    unlock();
    return sceKernelExitDeleteThread(0);
}

// The listener begun for the queue just begun, if it asked for one.
void startControl() {
    if (!g_ctl.port) return;
    lock();
    const bool running = g_ctlRunning != 0;
    if (!running) {
        g_ctlRunning = 1;
        g_ctlStop = 0;
        sceClibMemset(&g_ctlState, 0, sizeof(g_ctlState));
    }
    unlock();
    if (running) {
        logLine("control: the last queue's listener has not stopped; none for this one");
        return;
    }
    const SceUID thread = sceKernelCreateThread("ShellCompanion::Control", ctlThread, kThreadPriority,
                                                kFetchStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        logLine("control: no thread for it: 0x%08X", (unsigned)rc);
        lock();
        g_ctlRunning = 0;
        unlock();
    }
}

// The listener stopped, waited for up to 3 s (a push under way may take
// two). The music thread's.
void stopControl() {
    lock();
    const bool running = g_ctlRunning != 0;
    g_ctlStop = 1;
    unlock();
    for (int i = 0; running && i < 60; i++) {
        lock();
        const bool still = g_ctlRunning != 0;
        unlock();
        if (!still) break;
        sceKernelDelayThread(50 * 1000);
    }
}

// ── Sendspin: Music Assistant's player, carried on ──────────────────────────
// (docs/PROTOCOL.md §8) An app that is a Sendspin player (Vita Music
// Assistant) hands it over as it goes: the plugin connects to the server
// with the app's client id, so that Music Assistant sees the same player,
// and plays what the server streams through SceShell's music service, which
// plays only files. So the PCM goes into a WAV as it comes, its header
// rewritten with the size every kSsFlushMs, and the file is given to the
// shell once kSsLeadMs of it is there. The shell is opened again where it
// was whenever it stops or its clock stands while more is written, as a
// file that grows may be read only as far as it was as it was opened
// (SC_PROBE_REOPEN measures how long that takes). Past kSsRotateMs the PCM
// goes to the other file, which the shell is given at the first's end.
//
// The Sendspin is its plain JSON, as the app speaks it: client/hello as a
// player of PCM 44.1 kHz 16-bit stereo, with metadata@v1 and controller@v1
// if the app asks, so that the Music row shows the song and its buttons go
// to the server; client/state; stream/start, stream/clear (a seek or a
// skip: the shell stopped, a new file begun), stream/end (pause or stop);
// server/command's volume and mute, applied to the PCM as it is written, so
// heard after what is buffered. Clock sync is not done: one player plays as
// the shell goes. Over plain ws:// on the LAN, to an IPv4 address: there is
// no TLS socket here. Its lines in the log begin "sendspin:"; no id is
// logged.
const uint32_t kSsLeadMs = 2000;           // written before the shell is given the file
const uint32_t kSsFlushMs = 250;           // the file closed, its header rewritten
const uint32_t kSsRotateMs = 20 * 60 * 1000;  // a file's longest, 211 MB
const uint32_t kSsTickUs = 100 * 1000;
const uint32_t kSsStillMs = 1000;          // the shell's clock still this long: opened again
const uint32_t kSsGoneWaitMs = 30000;      // for the app to have gone
const uint32_t kSsStatusUs = 2000000;
const int kNetEagain = (int)0x80410123, kNetEtimedout = (int)0x8041013C,
          kNetEinprogress = (int)0x80410124, kNetEalready = (int)0x80410125,
          kNetEisconn = (int)0x80410138;

LibFn g_netConnect = {"sceNetConnect", 0x11E5B6F6, 0};
typedef int (*FnConnect)(int, const void*, unsigned);

// Under g_lock: a start asked for, by whom; running; to stop, and why (the
// thread reads g_ssStop without it).
int g_ssWant = -1;
int g_ssRunning;
int g_ssStop;              // atomic: 1 to stop, 2 taken back by the app
const char* g_ssStopWhy;
SceUID g_ssSema = -1;
int g_ssThread;

// What the app handed over (SC_SENDSPIN). The thread's.
struct SsCfg {
    char titleId[12], folder[96], appName[48];
    uint32_t ip;          // network order
    int port;
    char path[64];
    char clientId[96];
    char name[64], product[48], manufacturer[48], software[64];
    int metadata, controller;  // the roles asked for beyond player@v1
    uint32_t bufferBytes;
    int staticDelayMs;
    int volume, muted;    // as the app last had them
};

// The session. The thread's alone, but for the row's lines, which go to the
// row under g_lock.
struct Ss {
    SsCfg cfg;
    int sock;
    WsIn in;
    uint32_t rng;
    int hello;            // server/hello seen
    char server[64];
    // A text message as it comes; too long for g_ssText, dropped.
    uint32_t textN;
    int textOver;
    // A binary message: its header's bytes so far, and its type.
    uint8_t binHead[9];
    uint32_t binN;
    // The stream: going (stream/start for the player in our format), and
    // its files: the one written (wr) and the one with the shell (sh), each
    // with its PCM so far, and the shell's file finished (it has all it
    // will have) once the stream went on to the other.
    int streaming;
    char path[2][120];
    SceUID fd;            // wr's, open between flushes
    uint32_t bytes[2];
    int wr, sh;           // -1 for none
    int shDone;
    SceUInt64 flushUs;
    int tracked;          // the music service started (musicTracked)
    // The shell, as last looked at.
    int state;
    uint32_t timeMs;
    SceUInt64 stillUs;
    int reopens, waitingLogged;
    uint32_t gainQ15;
    // A sample's first byte, held as its second comes in the next piece.
    uint8_t half;
    int haveHalf;
    // What the server said: the group playing; the song; the controller's
    // commands (kSsCmd* bits).
    int groupPlaying;
    uint32_t commands;
    char title[160], artist[96], album[96];
    int32_t progressMs, durationMs;
    SceUInt64 progressUs;
    // Counts, for the log.
    uint32_t chunks, unknown;
    SceUInt64 statusUs;
};
Ss g_ss;
// The socket's reads go through g_probeBuf: the probe and Sendspin never run
// at once (probeAsk, ssAsk). A text message is held in g_ssText.
char g_ssText[4096];
char g_ssOut[1536];
enum { kSsCmdPlay = 1, kSsCmdPause = 2, kSsCmdNext = 4, kSsCmdPrevious = 8 };

uint32_t ssRandom() {
    uint32_t x = g_ss.rng ? g_ss.rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g_ss.rng = x;
}

bool ssStopAsked() {
    return __atomic_load_n(&g_ssStop, __ATOMIC_SEQ_CST) != 0;
}

// The handover file (SC_SENDSPIN) into g_ss.cfg. true when whole and usable.
bool ssReadCfg(const char* file) {
    SsCfg* c = &g_ss.cfg;
    const int n = readFile(file, g_ssText, sizeof(g_ssText));
    if (n < 0 || !findText(g_ssText, "\nend")) {
        logLine("sendspin: %s %s", file, n < 0 ? "is not there" : "is not whole");
        return false;
    }
    char v[160];
    // Decoded into out, cut to fit.
    struct Get {
        static void at(const char* key, char* out, int cap) {
            char raw[192];
            noteValue(g_ssText, key, raw, sizeof(raw));
            urlDecode(raw, (int)sceClibStrnlen(raw, sizeof(raw)), out, cap, false);
        }
    };
    Get::at("host", v, sizeof(v));
    if (!parseIp(v, &c->ip)) {
        logLine("sendspin: host= is not an IPv4 address; the app resolves a name first");
        return false;
    }
    Get::at("port", v, sizeof(v));
    c->port = v[0] ? toInt(v) : 8927;
    Get::at("path", c->path, sizeof(c->path));
    if (c->path[0] != '/') copyText(c->path, "/sendspin", sizeof(c->path));
    Get::at("clientId", c->clientId, sizeof(c->clientId));
    Get::at("name", c->name, sizeof(c->name));
    Get::at("product", c->product, sizeof(c->product));
    Get::at("manufacturer", c->manufacturer, sizeof(c->manufacturer));
    Get::at("software", c->software, sizeof(c->software));
    Get::at("roles", v, sizeof(v));
    c->metadata = findText(v, "metadata") != NULL;
    c->controller = findText(v, "controller") != NULL;
    Get::at("bufferBytes", v, sizeof(v));
    c->bufferBytes = v[0] ? toUint(v) : 2u * 1024u * 1024u;
    Get::at("staticDelayMs", v, sizeof(v));
    c->staticDelayMs = toInt(v);
    Get::at("volume", v, sizeof(v));
    c->volume = v[0] ? toInt(v) : 100;
    if (c->volume < 0 || c->volume > 100) c->volume = 100;
    Get::at("muted", v, sizeof(v));
    c->muted = v[0] == '1';
    if (!c->clientId[0] || c->port <= 0 || c->port > 65535) {
        logLine("sendspin: %s has no clientId, or no port", file);
        return false;
    }
    if (!c->name[0]) copyText(c->name, c->appName, sizeof(c->name));
    return true;
}

// The status (SC_SENDSPIN_STATUS), for the app: state, the server, the song.
void ssStatus(const char* state) {
    static char text[640], path[160];
    if (!g_ss.cfg.folder[0]) return;
    uint32_t secs, micros;
    splitUs(sceKernelGetSystemTimeWide(), &secs, &micros);
    // The song's place, as metadata@v1 has it worked out.
    int32_t pos = g_ss.progressMs;
    if (g_ss.groupPlaying && g_ss.progressUs)
        pos += (int32_t)((uint32_t)(sceKernelGetSystemTimeWide() - g_ss.progressUs) / 1000u);
    if (g_ss.durationMs > 0 && pos > g_ss.durationMs) pos = g_ss.durationMs;
    const int n = sceClibSnprintf(
        text, sizeof(text),
        "written=%u.%06u\nstate=%s\nserver=%s\nstreaming=%d\nplaying=%d\nvolume=%d\nmuted=%d\n"
        "positionMs=%d\ndurationMs=%d\nend\n",
        (unsigned)secs, (unsigned)micros, state, g_ss.server, g_ss.streaming, g_ss.groupPlaying,
        g_ss.cfg.volume, g_ss.cfg.muted, (int)(pos > 0 ? pos : 0),
        (int)(g_ss.durationMs > 0 ? g_ss.durationMs : 0));
    sceClibSnprintf(path, sizeof(path), "%s/%s", g_ss.cfg.folder, SC_SENDSPIN_STATUS);
    writeWhole(path, text, n);
    g_ss.statusUs = sceKernelGetSystemTimeWide();
}

// A text message to the server. false when it could not go.
bool ssSendText(const char* text, uint32_t n) {
    uint8_t mask[4];
    const uint32_t r = ssRandom();
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(r >> (8 * i));
    static uint8_t frame[sizeof(g_ssOut) + 16];
    const uint32_t len = wsFrame(frame, sizeof(frame), WS_TEXT, (const uint8_t*)text, n, mask);
    return len && sendAll(g_ss.sock, (const char*)frame, (int)len);
}

bool ssSendControl(int op, const uint8_t* data, uint32_t n) {
    uint8_t mask[4] = {(uint8_t)ssRandom(), 0x5A, 0xA5, 0x3C};
    uint8_t frame[140];
    const uint32_t len = wsFrame(frame, sizeof(frame), op, data, n > 125 ? 125 : n, mask);
    return len && sendAll(g_ss.sock, (const char*)frame, (int)len);
}

// g_ssOut, made with ssjPut*, sent; false when it did not fit or go.
bool ssSendOut(uint32_t n, const char* what) {
    if (n >= sizeof(g_ssOut)) {
        logLine("sendspin: %s too long to send (%u bytes)", what, (unsigned)n);
        return false;
    }
    const bool ok = ssSendText(g_ssOut, n);
    if (!ok) logLine("sendspin: %s not sent", what);
    return ok;
}

void ssSendHello() {
    const SsCfg* c = &g_ss.cfg;
    char* o = g_ssOut;
    uint32_t n = 0;
    const uint32_t cap = sizeof(g_ssOut);
    ssjPut(o, &n, cap, "{\"type\":\"client/hello\",\"payload\":{\"client_id\":");
    ssjPutString(o, &n, cap, c->clientId);
    ssjPut(o, &n, cap, ",\"name\":");
    ssjPutString(o, &n, cap, c->name);
    ssjPut(o, &n, cap, ",\"version\":1,\"device_info\":{\"product_name\":");
    ssjPutString(o, &n, cap, c->product[0] ? c->product : "PS Vita");
    ssjPut(o, &n, cap, ",\"manufacturer\":");
    ssjPutString(o, &n, cap, c->manufacturer[0] ? c->manufacturer : "Sony");
    ssjPut(o, &n, cap, ",\"software_version\":");
    ssjPutString(o, &n, cap, c->software[0] ? c->software : "vita-shell-companion");
    ssjPut(o, &n, cap, "},\"supported_roles\":[\"player@v1\"");
    if (c->metadata) ssjPut(o, &n, cap, ",\"metadata@v1\"");
    if (c->controller) ssjPut(o, &n, cap, ",\"controller@v1\"");
    char num[64];
    sceClibSnprintf(num, sizeof(num), "%u", (unsigned)c->bufferBytes);
    ssjPut(o, &n, cap,
           "],\"player@v1_support\":{\"supported_formats\":[{\"codec\":\"pcm\",\"channels\":2,"
           "\"sample_rate\":44100,\"bit_depth\":16}],\"buffer_capacity\":");
    ssjPut(o, &n, cap, num);
    ssjPut(o, &n, cap, ",\"supported_commands\":[\"volume\",\"mute\"]}}}");
    ssSendOut(n, "client/hello");
}

// client/state: synchronized, with the player's volume, mute and delay.
void ssSendState() {
    uint32_t n = (uint32_t)sceClibSnprintf(
        g_ssOut, sizeof(g_ssOut),
        "{\"type\":\"client/state\",\"payload\":{\"state\":\"synchronized\",\"player\":"
        "{\"volume\":%d,\"muted\":%s,\"static_delay_ms\":%d}}}",
        g_ss.cfg.volume, g_ss.cfg.muted ? "true" : "false", g_ss.cfg.staticDelayMs);
    ssSendOut(n, "client/state");
}

void ssSendCommand(const char* command) {
    uint32_t n = (uint32_t)sceClibSnprintf(
        g_ssOut, sizeof(g_ssOut), "{\"type\":\"client/command\",\"payload\":{\"controller\":"
        "{\"command\":\"%s\"}}}", command);
    if (ssSendOut(n, "client/command")) logLine("sendspin: %s, from the row", command);
}

void ssSendGoodbye(const char* reason) {
    uint32_t n = (uint32_t)sceClibSnprintf(
        g_ssOut, sizeof(g_ssOut), "{\"type\":\"client/goodbye\",\"payload\":{\"reason\":\"%s\"}}",
        reason);
    ssSendOut(n, "client/goodbye");
}

// The gain the PCM is written with: volume as perceived loudness, about its
// square as amplitude; none when muted.
void ssGain() {
    const uint32_t v = (uint32_t)g_ss.cfg.volume;
    g_ss.gainQ15 = g_ss.cfg.muted ? 0 : v * v * 32768u / 10000u;
}

// ── The files and the shell ──
// The file written now flushed: closed, its header given its size, opened
// again to go on.
void ssFlush() {
    if (g_ss.wr < 0) return;
    if (g_ss.fd >= 0) sceIoClose(g_ss.fd);
    g_ss.fd = -1;
    uint8_t h[kProbeWavHead];
    probeHeader(h, g_ss.bytes[g_ss.wr]);
    const int rcHead = writeAtStart(g_ss.path[g_ss.wr], h, sizeof(h));
    g_ss.fd = sceIoOpen(g_ss.path[g_ss.wr], SCE_O_WRONLY | SCE_O_APPEND, 0);
    if ((rcHead < 0 || g_ss.fd < 0) && g_ss.chunks < 1000)
        logLine("sendspin: %s not flushed: header 0x%08X, open 0x%08X", g_ss.path[g_ss.wr],
                (unsigned)rcHead, (unsigned)g_ss.fd);
    g_ss.flushUs = sceKernelGetSystemTimeWide();
}

// A file begun for the stream: the other one than the shell has.
void ssBeginFile() {
    ssFlush();
    if (g_ss.fd >= 0) sceIoClose(g_ss.fd);
    g_ss.fd = -1;
    const int f = g_ss.sh == 0 ? 1 : 0;
    g_ss.wr = f;
    g_ss.bytes[f] = 0;
    g_ss.haveHalf = 0;
    uint8_t h[kProbeWavHead];
    probeHeader(h, 0);
    g_ss.fd = sceIoOpen(g_ss.path[f], SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (g_ss.fd < 0 || sceIoWrite(g_ss.fd, h, sizeof(h)) != (int)sizeof(h)) {
        logLine("sendspin: %s not made: 0x%08X", g_ss.path[f], (unsigned)g_ss.fd);
        if (g_ss.fd >= 0) sceIoClose(g_ss.fd);
        g_ss.fd = -1;
        g_ss.wr = -1;
        return;
    }
    g_ss.flushUs = sceKernelGetSystemTimeWide();
}

// The shell stopped, and its file forgotten (and removed unless written now).
void ssShellStop(const char* why) {
    if (g_ss.sh < 0) return;
    const int rc = musicEvent(kEventStop);
    logLine("sendspin: the shell stopped (%s) at %u ms: 0x%08X", why, (unsigned)g_ss.timeMs,
            (unsigned)rc);
    if (g_ss.sh != g_ss.wr) sceIoRemove(g_ss.path[g_ss.sh]);
    g_ss.sh = -1;
    g_ss.shDone = 0;
}

// A file to the shell (f, the one written now unless the shell's finished
// one is to play on), from atMs: stop, open, play, as the queue's songs are
// handed over.
void ssShellPlay(uint32_t atMs, const char* why, int f = -1) {
    if (f < 0) f = g_ss.wr;
    if (!g_ss.tracked) {
        const int rc = musicTracked(0x30000);
        g_ss.tracked = 1;
        logLine("sendspin: music service started: 0x%08X", (unsigned)rc);
    }
    ssFlush();
    const int old = g_ss.sh;
    const int done = f != g_ss.wr;  // the shell's finished file, played on
    const int rcStop = musicEvent(kEventStop), rcOpen = musicOpen(g_ss.path[f]);
    const int rcTime = atMs ? musicSeekTime(atMs) : 0;
    const int rcPlay = musicEvent(kEventPlay);
    const int rcSeek = atMs ? musicEvent(kEventSeek) : 0;
    if (g_ss.reopens < 20 || g_ss.reopens % 100 == 0)
        logLine("sendspin: to the shell (%s), %u ms written, from %u ms: stop 0x%08X, open 0x%08X, "
                "seek time 0x%08X, play 0x%08X, seek 0x%08X",
                why, (unsigned)probeMsOf(g_ss.bytes[f]), (unsigned)atMs, (unsigned)rcStop,
                (unsigned)rcOpen, (unsigned)rcTime, (unsigned)rcPlay, (unsigned)rcSeek);
    if (old >= 0 && old != f && old != g_ss.wr) sceIoRemove(g_ss.path[old]);
    g_ss.sh = f;
    g_ss.shDone = done;
    g_ss.stillUs = 0;
    g_ss.timeMs = atMs;
    g_ss.waitingLogged = 0;
}

// The stream's end (stream/end, or the session's): the shell stopped, the
// files gone.
void ssStreamEnd(const char* why) {
    ssShellStop(why);
    if (g_ss.fd >= 0) sceIoClose(g_ss.fd);
    g_ss.fd = -1;
    g_ss.wr = -1;
    g_ss.streaming = 0;
    sceIoRemove(g_ss.path[0]);
    sceIoRemove(g_ss.path[1]);
    if (g_ss.tracked) {
        musicTracked(0x30001);
        g_ss.tracked = 0;
    }
}

// A sample with the gain.
void ssScale(uint8_t* p) {
    const int s = (int16_t)(p[0] | p[1] << 8);
    const int v = (int)(((int32_t)s * (int32_t)g_ss.gainQ15) >> 15);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void ssWrite(const uint8_t* data, uint32_t n) {
    if (!n) return;
    if (g_ss.fd >= 0 && sceIoWrite(g_ss.fd, data, n) == (int)n) {
        g_ss.bytes[g_ss.wr] += n;
    } else if (g_ss.chunks < 5) {
        logLine("sendspin: PCM not written");
    }
}

// PCM from the server, written with the gain. The pieces the socket gives
// may split a sample: its first byte is held for the next.
void ssPcm(uint8_t* data, uint32_t n) {
    if (!g_ss.streaming || g_ss.wr < 0 || !n) return;
    if (g_ss.haveHalf) {
        uint8_t s[2] = {g_ss.half, data[0]};
        if (g_ss.gainQ15 < 32768u) ssScale(s);
        ssWrite(s, 2);
        g_ss.haveHalf = 0;
        data++;
        n--;
    }
    if (n & 1) {
        g_ss.half = data[n - 1];
        g_ss.haveHalf = 1;
        n--;
    }
    if (g_ss.gainQ15 < 32768u)
        for (uint32_t i = 0; i < n; i += 2) ssScale(data + i);
    ssWrite(data, n);
    // Long enough: the stream goes on in the other file, which the shell
    // is given at this one's end.
    if (g_ss.sh == g_ss.wr && g_ss.bytes[g_ss.wr] >= probeBytesOf(kSsRotateMs)) {
        logLine("sendspin: %s has %u ms; the stream goes on in the other", g_ss.path[g_ss.wr],
                (unsigned)probeMsOf(g_ss.bytes[g_ss.wr]));
        ssFlush();
        g_ss.shDone = 1;
        ssBeginFile();
    }
}

// Each tick: the file flushed, the shell given it, and kept playing.
void ssShellTick(SceUInt64 now) {
    if (!g_ss.streaming || g_ss.wr < 0) return;
    if ((uint32_t)(now - g_ss.flushUs) >= kSsFlushMs * 1000u) ssFlush();
    const uint32_t writtenMs = probeMsOf(g_ss.bytes[g_ss.wr]);
    if (g_ss.sh < 0) {
        if (writtenMs >= kSsLeadMs) ssShellPlay(0, "the stream's start");
        return;
    }
    int state = 0;
    uint32_t time = 0;
    if (musicStatus(&state, &time) < 0) return;
    const uint32_t before = g_ss.timeMs;
    if (state != g_ss.state)
        logLine("sendspin: the shell's state %d to %d at %u ms", g_ss.state, state, (unsigned)time);
    g_ss.state = state;
    g_ss.timeMs = time;
    const uint32_t shMs = probeMsOf(g_ss.bytes[g_ss.sh]);
    // The shell's file finished and played out: the other one from its start.
    if (g_ss.shDone && time + 150 >= shMs) {
        ssShellPlay(0, "the next file");
        return;
    }
    if (state == 1 && time != before) {
        g_ss.stillUs = 0;
        return;
    }
    if (!g_ss.stillUs) g_ss.stillUs = now;
    if (state == 1 && (uint32_t)(now - g_ss.stillUs) < kSsStillMs * 1000u) return;
    // Stopped, or its clock still: opened again where it was, if more is
    // written; else the stream is late, and it waits.
    if (shMs > time + 500) {
        g_ss.reopens++;
        ssShellPlay(time, state == 1 ? "its clock stood" : "it stopped", g_ss.sh);
    } else if (g_ss.shDone) {
        ssShellPlay(0, "the next file");
    } else if (!g_ss.waitingLogged) {
        g_ss.waitingLogged = 1;
        logLine("sendspin: the shell at %u ms has all that was written; waiting for the stream",
                (unsigned)time);
    }
}

// ── The row ──
void ssRowLines() {
    lock();
    if (g_owner == kOwnerShell && g_ssRunning) {
        if (g_ss.title[0]) {
            fromUtf8(g_ss.title, g_title, 256);
            fromUtf8(g_ss.artist, g_artist, 256);
            fromUtf8(g_ss.album, g_album, 256);
        } else {
            fromUtf8(g_ss.cfg.name, g_title, 256);
            fromUtf8(g_ss.server[0] ? g_ss.server : "Music Assistant", g_artist, 256);
            g_album[0] = 0;
        }
        g_playing = g_ss.groupPlaying || g_ss.streaming ? 1 : 0;
        g_artSerial = 0;
    }
    unlock();
}

// The row is the shell's while the plugin is the player, with an app's
// lines; given back as it stops.
void ssRowTake(int app) {
    lock();
    g_owner = kOwnerShell;
    g_ownerApp = app;
    g_pressTitle[0] = 0;
    g_active = 1;
    notesWanted();
    unlock();
    ssRowLines();
}

void ssRowGiveBack() {
    lock();
    if (g_owner == kOwnerShell && !g_carry) {
        g_active = 0;
        g_releaseDue = 1;
        g_artSerial = 0;
        g_playing = -1;
        notesWanted();
    }
    unlock();
}

// A press on the row, to the server as the controller's command.
void ssPress(int press) {
    const char* cmd = press == SC_PRESS_NEXT     ? "next"
                    : press == SC_PRESS_PREVIOUS ? "previous"
                    : g_ss.groupPlaying          ? "pause"
                                                 : "play";
    const uint32_t bit = press == SC_PRESS_NEXT     ? kSsCmdNext
                       : press == SC_PRESS_PREVIOUS ? kSsCmdPrevious
                       : g_ss.groupPlaying          ? kSsCmdPause
                                                    : kSsCmdPlay;
    if (!g_ss.cfg.controller) {
        logLine("sendspin: %s, from the row: not asked to be a controller; dropped", cmd);
        return;
    }
    if (g_ss.commands && !(g_ss.commands & bit)) {
        logLine("sendspin: %s, from the row: the server does not take it now", cmd);
        return;
    }
    ssSendCommand(cmd);
}

// ── The server's messages ──
// Whether roles (a stream message's) leaves the player out: a list without
// "player" in it.
bool ssNotForPlayer(const char* payload) {
    const char* roles = ssjMember(payload, "roles");
    return roles && !ssjArrayHas(roles, "player");
}

void ssStreamStart(const char* payload) {
    const char* p = ssjMember(payload, "player");
    if (!p) return;
    char codec[16];
    int32_t rate = 0, channels = 0, bits = 0;
    ssjString(ssjMember(p, "codec"), codec, sizeof(codec));
    ssjInt(ssjMember(p, "sample_rate"), &rate);
    ssjInt(ssjMember(p, "channels"), &channels);
    ssjInt(ssjMember(p, "bit_depth"), &bits);
    const bool ours = same(codec, "pcm") && rate == 44100 && channels == 2 && bits == 16;
    logLine("sendspin: stream/start: %s %d Hz, %d channels, %d bits%s%s", codec, (int)rate,
            (int)channels, (int)bits, ours ? "" : ": not what the shell is given; asked again",
            ours && g_ss.streaming ? ", the stream going on" : "");
    if (!ours) {
        const uint32_t n = (uint32_t)sceClibSnprintf(
            g_ssOut, sizeof(g_ssOut),
            "{\"type\":\"stream/request-format\",\"payload\":{\"player\":{\"codec\":\"pcm\","
            "\"sample_rate\":44100,\"channels\":2,\"bit_depth\":16}}}");
        ssSendOut(n, "stream/request-format");
        return;
    }
    // A stream going on is updated, not cleared (the spec's stream/start).
    if (g_ss.streaming) return;
    g_ss.streaming = 1;
    g_ss.reopens = 0;
    g_ss.state = -1;
    ssBeginFile();
}

void ssServerState(const char* payload) {
    const char* m = ssjMember(payload, "metadata");
    if (m) {
        // Only fields there change; null clears.
        const char* v;
        if ((v = ssjMember(m, "title"))) ssjString(v, g_ss.title, sizeof(g_ss.title));
        if ((v = ssjMember(m, "artist"))) ssjString(v, g_ss.artist, sizeof(g_ss.artist));
        if ((v = ssjMember(m, "album"))) ssjString(v, g_ss.album, sizeof(g_ss.album));
        const char* pr = ssjMember(m, "progress");
        if (pr) {
            int32_t at = 0, dur = 0, speed = 1000;
            ssjInt(ssjMember(pr, "track_progress"), &at);
            ssjInt(ssjMember(pr, "track_duration"), &dur);
            ssjInt(ssjMember(pr, "playback_speed"), &speed);
            g_ss.progressMs = at;
            g_ss.durationMs = dur;
            g_ss.progressUs = speed ? sceKernelGetSystemTimeWide() : 0;
        }
        ssRowLines();
    }
    const char* c = ssjMember(payload, "controller");
    const char* cmds = c ? ssjMember(c, "supported_commands") : NULL;
    if (cmds) {
        g_ss.commands = (ssjArrayHas(cmds, "play") ? kSsCmdPlay : 0) |
                        (ssjArrayHas(cmds, "pause") ? kSsCmdPause : 0) |
                        (ssjArrayHas(cmds, "next") ? kSsCmdNext : 0) |
                        (ssjArrayHas(cmds, "previous") ? kSsCmdPrevious : 0);
        // None of them: anything goes, rather than nothing.
    }
}

void ssText(const char* json) {
    char type[32];
    ssjString(ssjMember(json, "type"), type, sizeof(type));
    const char* payload = ssjMember(json, "payload");
    if (!payload) payload = json;
    if (same(type, "server/hello")) {
        ssjString(ssjMember(payload, "name"), g_ss.server, sizeof(g_ss.server));
        for (char* c = g_ss.server; *c; c++)
            if ((unsigned char)*c < 0x20) *c = ' ';  // one line in the status
        const char* roles = ssjMember(payload, "active_roles");
        logLine("sendspin: server/hello from \"%s\": player %s, metadata %s, controller %s",
                g_ss.server, ssjArrayHas(roles, "player@v1") ? "on" : "off",
                ssjArrayHas(roles, "metadata@v1") ? "on" : "off",
                ssjArrayHas(roles, "controller@v1") ? "on" : "off");
        g_ss.hello = 1;
        ssSendState();
        ssRowLines();
        ssStatus("connected");
    } else if (same(type, "stream/start")) {
        ssStreamStart(payload);
    } else if (same(type, "stream/clear")) {
        if (ssNotForPlayer(payload) || !g_ss.streaming) return;
        logLine("sendspin: stream/clear at %u ms, %u ms written", (unsigned)g_ss.timeMs,
                (unsigned)(g_ss.wr >= 0 ? probeMsOf(g_ss.bytes[g_ss.wr]) : 0));
        ssShellStop("stream/clear");
        ssBeginFile();
    } else if (same(type, "stream/end")) {
        if (ssNotForPlayer(payload)) return;
        logLine("sendspin: stream/end");
        ssStreamEnd("stream/end");
        ssRowLines();
    } else if (same(type, "server/command")) {
        const char* p = ssjMember(payload, "player");
        char cmd[24];
        ssjString(ssjMember(p, "command"), cmd, sizeof(cmd));
        int32_t v = 0;
        if (same(cmd, "volume") && ssjInt(ssjMember(p, "volume"), &v)) {
            g_ss.cfg.volume = v < 0 ? 0 : v > 100 ? 100 : (int)v;
        } else if (same(cmd, "mute")) {
            const int b = ssjBool(ssjMember(p, "mute"));
            if (b >= 0) g_ss.cfg.muted = b;
        } else {
            logLine("sendspin: server/command \"%s\" passed over", cmd);
            return;
        }
        ssGain();
        logLine("sendspin: volume %d%s, from the server", g_ss.cfg.volume,
                g_ss.cfg.muted ? ", muted" : "");
        ssSendState();
    } else if (same(type, "server/state")) {
        ssServerState(payload);
    } else if (same(type, "group/update")) {
        char state[16];
        if (ssjString(ssjMember(payload, "playback_state"), state, sizeof(state))) {
            g_ss.groupPlaying = same(state, "playing");
            ssRowLines();
        }
    } else if (same(type, "server/time")) {
    } else if (g_ss.unknown++ < 10) {
        logLine("sendspin: %s passed over", type[0] ? type : "a message with no type");
    }
}

// A piece of a message from the socket (wsFeed's callback).
int ssPiece(void*, int op, const uint8_t* data, uint32_t n, int first, int last) {
    if (op == WS_PING) return ssSendControl(WS_PONG, data, n) ? 0 : -10;
    if (op == WS_PONG) return 0;
    if (op == WS_CLOSE) {
        const uint32_t code = n >= 2 ? (uint32_t)data[0] << 8 | data[1] : 0;
        logLine("sendspin: the server closed the connection (%u)", (unsigned)code);
        ssSendControl(WS_CLOSE, data, n >= 2 ? 2 : 0);
        return -11;
    }
    if (op == WS_TEXT) {
        if (first) {
            g_ss.textN = 0;
            g_ss.textOver = 0;
        }
        if (g_ss.textN + n < sizeof(g_ssText)) {
            sceClibMemcpy(g_ssText + g_ss.textN, data, n);
            g_ss.textN += n;
        } else {
            g_ss.textOver = 1;
        }
        if (last) {
            g_ssText[g_ss.textN] = 0;
            if (g_ss.textOver)
                logLine("sendspin: a message of over %u bytes passed over",
                        (unsigned)sizeof(g_ssText));
            else
                ssText(g_ssText);
        }
        return 0;
    }
    // Binary: its 9-byte header, then what it carries.
    if (first) g_ss.binN = 0;
    uint32_t i = 0;
    while (g_ss.binN < 9 && i < n) g_ss.binHead[g_ss.binN++] = data[i++];
    if (g_ss.binN < 9 || g_ss.binHead[0] != 4) return 0;
    if (first || i > 0) g_ss.chunks++;
    if (i < n) ssPcm((uint8_t*)data + i, n - i);
    return 0;
}

// ── The connection ──
bool ssConnect() {
    const SsCfg* c = &g_ss.cfg;
    g_ss.sock = netFn<FnSocket>(kNetSocket)("ShellCompanion::Sendspin", kAfInet, kSockStream, 0);
    if (g_ss.sock < 0) {
        logLine("sendspin: socket 0x%08X", (unsigned)g_ss.sock);
        return false;
    }
    NetAddrIn to;
    sceClibMemset(&to, 0, sizeof(to));
    to.len = sizeof(to);
    to.family = kAfInet;
    to.port = netOrder16(c->port);
    to.addr = c->ip;
    // Without waiting on it, so that a stop is seen while it connects: up to
    // 5 s.
    setOpt(g_ss.sock, kSolSocket, kSoNbio, 1);
    int rc = ((FnConnect)g_netConnect.fn)(g_ss.sock, &to, sizeof(to));
    for (int i = 0; i < 100 && (rc == kNetEinprogress || rc == kNetEalready) && !ssStopAsked(); i++) {
        sceKernelDelayThread(50 * 1000);
        rc = ((FnConnect)g_netConnect.fn)(g_ss.sock, &to, sizeof(to));
    }
    if (rc == kNetEisconn) rc = 0;
    setOpt(g_ss.sock, kSolSocket, kSoNbio, 0);
    if (rc < 0) {
        char ip[16];
        ipText(c->ip, ip, sizeof(ip));
        logLine("sendspin: connect to %s:%d: 0x%08X", ip, c->port, (unsigned)rc);
        return false;
    }
    setOpt(g_ss.sock, kSolSocket, kSoRcvTimeo, 5000000);
    setOpt(g_ss.sock, kSolSocket, kSoSndTimeo, 5000000);
    // The upgrade, with a key of 16 random bytes.
    uint8_t raw[16];
    for (int i = 0; i < 16; i += 4) {
        const uint32_t r = ssRandom();
        for (int k = 0; k < 4; k++) raw[i + k] = (uint8_t)(r >> (8 * k));
    }
    char key[32], ip[16];
    b64Encode(raw, sizeof(raw), key, sizeof(key));
    ipText(c->ip, ip, sizeof(ip));
    const int n = sceClibSnprintf(g_ssOut, sizeof(g_ssOut),
                                  "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
                                  "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                                  "Sec-WebSocket-Version: 13\r\n\r\n",
                                  c->path, ip, c->port, key);
    if (!sendAll(g_ss.sock, g_ssOut, n)) {
        logLine("sendspin: the upgrade not sent");
        return false;
    }
    // Its answer, up to the blank line; what comes after is frames.
    int got = 0;
    const char* end = NULL;
    while (!end && got < (int)sizeof(g_ssText) - 1) {
        const int r = netFn<FnRecv>(kNetRecv)(g_ss.sock, g_ssText + got,
                                               sizeof(g_ssText) - 1 - got, 0);
        if (r <= 0) break;
        got += r;
        g_ssText[got] = 0;
        end = findText(g_ssText, "\r\n\r\n");
    }
    char line[64];
    int k = 0;
    while (k < got && k < (int)sizeof(line) - 1 && g_ssText[k] != '\r') line[k] = g_ssText[k], k++;
    line[k] = 0;
    if (!end || !startsWith(g_ssText, "HTTP/1.1 101")) {
        logLine("sendspin: the upgrade answered \"%s\"%s", line, end ? "" : ", cut short");
        return false;
    }
    wsInit(&g_ss.in);
    setOpt(g_ss.sock, kSolSocket, kSoRcvTimeo, (int)kSsTickUs);
    const int after = got - (int)(end + 4 - g_ssText);
    if (after > 0) {
        sceClibMemcpy(g_probeBuf, end + 4, after);
        if (wsFeed(&g_ss.in, g_probeBuf, (uint32_t)after, ssPiece, NULL) != 0) return false;
    }
    return true;
}

void ssClose() {
    if (g_ss.sock >= 0) netFn<FnClose>(kNetClose)(g_ss.sock);
    g_ss.sock = -1;
}

// One connection, to its end: false when it was lost, true when stopped.
bool ssSession(int app) {
    if (!ssConnect()) {
        ssClose();
        return ssStopAsked();
    }
    logLine("sendspin: connected; client/hello as \"%s\"", g_ss.cfg.name);
    ssSendHello();
    g_ss.hello = 0;
    ssRowTake(app);
    SceUInt64 helloUs = sceKernelGetSystemTimeWide();
    bool lost = false;
    while (!ssStopAsked()) {
        const int r = netFn<FnRecv>(kNetRecv)(g_ss.sock, g_probeBuf, sizeof(g_probeBuf), 0);
        if (r > 0) {
            const int rc = wsFeed(&g_ss.in, g_probeBuf, (uint32_t)r, ssPiece, NULL);
            if (rc != 0) {
                if (rc != -11) logLine("sendspin: the connection's frames: %d", rc);
                lost = true;
                break;
            }
        } else if (r == 0 || (r != kNetEagain && r != kNetEtimedout)) {
            logLine("sendspin: the connection lost: 0x%08X", (unsigned)r);
            lost = true;
            break;
        }
        const SceUInt64 now = sceKernelGetSystemTimeWide();
        if (!g_ss.hello && (uint32_t)(now - helloUs) > 10000000u) {
            logLine("sendspin: no server/hello in 10 s");
            lost = true;
            break;
        }
        const int press = __atomic_exchange_n(&g_carryPress, 0, __ATOMIC_SEQ_CST);
        if (press) ssPress(press);
        ssShellTick(now);
        if ((uint32_t)(now - g_ss.statusUs) >= kSsStatusUs)
            ssStatus(g_ss.streaming ? "streaming" : "connected");
    }
    if (!lost) ssSendGoodbye(__atomic_load_n(&g_ssStop, __ATOMIC_SEQ_CST) == 2 ? "restart"
                                                                              : "shutdown");
    ssStreamEnd(lost ? "the connection lost" : "stopped");
    ssClose();
    return !lost;
}

// The handover, run: the app's file read, the app gone, and the connection
// kept until stopped, made again when lost (after 1, 2, 5, 10 s, then each
// 30 s, for up to 30 min of failures in a row).
void ssRun(int app) {
    sceClibMemset(&g_ss, 0, sizeof(g_ss));
    g_ss.sock = g_ss.fd = -1;
    g_ss.wr = g_ss.sh = -1;
    g_ss.rng = (uint32_t)sceKernelGetSystemTimeWide() ^ 0xA5A5F00Du;
    char file[160];
    lock();
    const App* a = appAt(app);
    if (a) {
        copyText(g_ss.cfg.titleId, a->titleId, sizeof(g_ss.cfg.titleId));
        copyText(g_ss.cfg.folder, a->folder, sizeof(g_ss.cfg.folder));
        copyText(g_ss.cfg.appName, a->name, sizeof(g_ss.cfg.appName));
        appPath(a, SC_SENDSPIN, file, sizeof(file));
    }
    unlock();
    if (!a || !ssReadCfg(file)) {
        ssStatus("failed");
        return;
    }
    ssGain();
    sceClibSnprintf(g_ss.path[0], sizeof(g_ss.path[0]), "%s/sc-sendspin-a.wav", g_ss.cfg.folder);
    sceClibSnprintf(g_ss.path[1], sizeof(g_ss.path[1]), "%s/sc-sendspin-b.wav", g_ss.cfg.folder);
    if (!g_netFound && !netFind()) {
        logLine("sendspin: no SceNet");
        ssStatus("failed");
        return;
    }
    if (!g_netConnect.fn) {
        uintptr_t fn = 0;
        const int rc = scGetModuleExportFunc("SceNet", kNetLibrary, g_netConnect.nid, &fn);
        g_netConnect.fn = rc >= 0 ? fn : 0;
        if (rc < 0) {
            logLine("sendspin: sceNetConnect not in SceNet: 0x%08X", (unsigned)rc);
            ssStatus("failed");
            return;
        }
    }
    if (!findSvcObj("sendspin") || !g_getSvcObj()) {
        logLine("sendspin: no client for SceShell's music service");
        ssStatus("failed");
        return;
    }
    ssStatus("waiting");
    // The app gone first, so that its own connection has closed.
    for (uint32_t waited = 0; !ssStopAsked(); waited += 250) {
        SceUID pid = -1;
        if (sceAppMgrGetIdByName(&pid, g_ss.cfg.titleId) < 0) {
            logLine("sendspin: %s gone, %s s after it handed over", g_ss.cfg.appName,
                    secsOf(waited).s);
            break;
        }
        if (waited >= kSsGoneWaitMs) {
            logLine("sendspin: %s still runs after %u s; connecting anyway", g_ss.cfg.appName,
                    (unsigned)(kSsGoneWaitMs / 1000));
            break;
        }
        sceKernelDelayThread(250 * 1000);
    }
    // The queue the shell carried on has stopped (ssAsk asked it to).
    for (int i = 0; i < 50; i++) {
        lock();
        const int carry = g_carry;
        unlock();
        if (!carry) break;
        sceKernelDelayThread(100 * 1000);
    }
    static const uint32_t kBackoffS[] = {1, 2, 5, 10, 30};
    uint32_t failedS = 0;
    int tries = 0;
    while (!ssStopAsked()) {
        ssStatus("connecting");
        const SceUInt64 began = sceKernelGetSystemTimeWide();
        if (ssSession(app)) break;
        // A connection that held a minute starts the counts again.
        if ((uint32_t)(sceKernelGetSystemTimeWide() - began) > 60000000u) {
            tries = 0;
            failedS = 0;
        }
        const uint32_t wait = kBackoffS[tries < 4 ? tries : 4];
        tries++;
        failedS += wait;
        if (failedS > 30 * 60) {
            logLine("sendspin: no connection for 30 min; given up");
            break;
        }
        if (tries <= 5 || tries % 20 == 0)
            logLine("sendspin: connecting again in %u s (try %d)", (unsigned)wait, tries + 1);
        ssStatus("reconnecting");
        ssRowLines();
        SceUInt timeout = wait * 1000000u;
        sceKernelWaitSema(g_ssSema, 1, &timeout);
    }
    ssRowGiveBack();
}

void ssStep() {
    lock();
    const int app = g_ssWant;
    g_ssWant = -1;
    if (app >= 0) g_ssRunning = 1;
    unlock();
    if (app < 0) return;
    ssRun(app);
    lock();
    const int how = __atomic_load_n(&g_ssStop, __ATOMIC_SEQ_CST);
    const char* why = g_ssStopWhy;
    g_ssRunning = 0;
    __atomic_store_n(&g_ssStop, 0, __ATOMIC_SEQ_CST);
    g_ssStopWhy = NULL;
    // A queue handed over meanwhile goes now.
    if (g_playQueueDue && g_musicSema >= 0) sceKernelSignalSema(g_musicSema, 1);
    unlock();
    g_ss.groupPlaying = 0;
    ssStatus("stopped");
    logLine("sendspin: stopped%s%s", how ? ": " : "", how && why ? why : "");
}

class SsThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        while (!IsCanceled()) {
            sceKernelWaitSema(g_ssSema, 1, NULL);
            ssStep();
        }
    }
};

// Sendspin runs, or is about to. Under g_lock.
bool ssHolds() {
    return g_ssRunning || g_ssWant >= 0;
}

// Stops being the player, if it is (how 2: the app takes it back). Under
// g_lock.
void ssStop(const char* why, int how = 1) {
    if (g_ssWant >= 0) {
        g_ssWant = -1;
        logLine("sendspin: not to start: %s", why);
    }
    if (!g_ssRunning || __atomic_load_n(&g_ssStop, __ATOMIC_SEQ_CST)) return;
    g_ssStopWhy = why;
    __atomic_store_n(&g_ssStop, how, __ATOMIC_SEQ_CST);
    logLine("sendspin: to stop: %s", why);
    sceKernelSignalSema(g_ssSema, 1);
}

// SC_CMD_SENDSPIN_START from an app. Under g_lock.
void ssAsk(int app) {
    if (g_ssRunning || g_ssWant >= 0) {
        logLine("sendspin: already the player; %s's start passed over", appName(app));
        return;
    }
    if (g_probeRunning || g_probeWant >= 0) {
        logLine("sendspin: not while the probe runs");
        return;
    }
    if (g_ssSema < 0) g_ssSema = sceKernelCreateSema("ShellCompanion::Sendspin", 0, 0, 1, NULL);
    if (g_ssSema < 0) {
        logLine("sendspin: its semaphore: 0x%08X", (unsigned)g_ssSema);
        return;
    }
    if (!g_ssThread) {
        const int rc = (new SsThread(kThreadPriority, kFetchStack, "ShellCompanion::Sendspin"))->Start();
        if (rc < 0) {
            logLine("sendspin: its thread not started: 0x%08X", (unsigned)rc);
            return;
        }
        g_ssThread = 1;
    }
    stopCarry("Sendspin takes the music", kEndStopped);
    g_playQueueDue = 0;
    g_ssWant = app;
    sceKernelSignalSema(g_ssSema, 1);
}

// ── An app's update ─────────────────────────────────────────────────────────
// The system's installer, ScePromoterUtil, refuses a title while it runs, so
// an app cannot install its own update. SceShell, which no app closes, does
// it (SC_CMD_UPDATE_INSTALL): the app unpacks the update to SC_UPDATE_DIR in
// its folder, with its package head made, asks, and quits. This waits for it
// to be gone, has the installer install the folder, writes what came of it
// to SC_UPDATE_RESULT, and launches the app again, the old one if the
// install failed, which then says so. The installer is reached as
// Vitacompanion's SceShell module reaches it (devnoname120/vitacompanion,
// promote.c): its module loaded through SceSysmodule unless SceShell has it,
// its functions found by NID, and both left as they are after, the client
// being SceShell's too: no Exit, no unload. It is driven as VitaPlex's
// updater stub drove it: PromotePkg, its state looked at until idle, then
// its result. One update at a time.
const uint32_t kPromoterLibrary = 0x31F237B6;      // ScePromoterUtil, in module ScePromoterUtil
const unsigned kSysmoduleNotLoaded = 0x805A1001u;  // sceSysmoduleIsLoadedInternal's "not loaded"
// What the installer gave VitaPlex trying to install itself while it ran
// (its builds up to 2055).
const unsigned kPromoterInUse = 0x80101114u;
enum { kPromInit, kPromGetState, kPromPromote, kPromGetResult, kPromFns };
LibFn g_promoter[kPromFns] = {{"scePromoterUtilityInit", 0x93451536, 0},
                              {"scePromoterUtilityGetState", 0xABEC74D2, 0},
                              {"scePromoterUtilityPromotePkg", 0x716C81F4, 0},
                              {"scePromoterUtilityGetResult", 0x49B473F0, 0}};
typedef int (*FnPromInit)(void);
typedef int (*FnPromState)(int*);
typedef int (*FnPromote)(const char*, int);
// How long the app has to go, the installer to be idle, and an install to end.
const uint32_t kUpdateGoneMs = 30000, kUpdateIdleMs = 60000, kUpdateInstallMs = 300000;
const uint32_t kUpdateLookMs = 250;
// The update thread's stack. The installer's calls run on it, and its module's
// start as it loads, with what they take not known here: twice the fetch
// threads'.
const int kUpdateStack = 0x10000;
int g_updating;        // under g_lock: the update thread runs
int g_promoterInited;  // the update thread's: Init called once, its client kept
App g_updateApp;       // the update thread's: the app it updates
char g_updateDir[128]; // and that app's SC_UPDATE_DIR

// Waits up to `ms` while the app runs. true once it has gone.
bool updateWaitGone(uint32_t ms) {
    for (uint32_t waited = 0;; waited += kUpdateLookMs) {
        SceUID pid = -1;
        if (sceAppMgrGetIdByName(&pid, g_updateApp.titleId) < 0) {
            logLine("update: %s gone, %s s after it asked", g_updateApp.name, secsOf(waited).s);
            return true;
        }
        if (waited >= ms) return false;
        sceKernelDelayThread(kUpdateLookMs * 1000);
    }
}

// The installer's state once idle, waiting up to `ms`: 0, else what reading
// it gave, or 1 if it stayed busy.
int updateWaitIdle(uint32_t ms) {
    for (uint32_t waited = 0;; waited += kUpdateLookMs) {
        int state = -1;
        const int rc = ((FnPromState)g_promoter[kPromGetState].fn)(&state);
        if (rc < 0) return rc;
        if (state == 0) return 0;
        if (waited >= ms) return 1;
        if (waited % 10000 == 0)
            logLine("update: the installer busy (%d), %s s on", state, secsOf(waited).s);
        sceKernelDelayThread(kUpdateLookMs * 1000);
    }
}

// The installer loaded and its functions found. 0, else the code, with why.
int updateFindPromoter(char* why, int cap) {
    int rc = sceSysmoduleIsLoadedInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if ((unsigned)rc == kSysmoduleNotLoaded) {
        rc = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
        logLine("update: the system's installer loaded into SceShell: 0x%08X", (unsigned)rc);
    }
    if (rc < 0) {
        sceClibSnprintf(why, cap, "the system's installer would not load (0x%08X)", (unsigned)rc);
        return rc;
    }
    for (int i = 0; i < kPromFns; i++) {
        uintptr_t fn = 0;
        rc = scGetModuleExportFunc("ScePromoterUtil", kPromoterLibrary, g_promoter[i].nid, &fn);
        if (rc < 0 || !fn) {
            sceClibSnprintf(why, cap, "%s not in the system's installer (0x%08X)", g_promoter[i].name,
                            (unsigned)rc);
            return rc < 0 ? rc : -1;
        }
        g_promoter[i].fn = fn;
    }
    return 0;
}

// g_updateDir installed: once begun, its state looked at until idle, and
// its result read. 0, else the code, with why.
int updatePromote(char* why, int cap) {
    const SceUInt64 began = sceKernelGetSystemTimeWide();
    int rc = ((FnPromote)g_promoter[kPromPromote].fn)(g_updateDir, 0);
    if (rc < 0) {
        sceClibSnprintf(why, cap, "the system's installer would not begin (0x%08X)", (unsigned)rc);
        return rc;
    }
    const int idle = updateWaitIdle(kUpdateInstallMs);
    const uint32_t took = (uint32_t)(sceKernelGetSystemTimeWide() - began) / 1000u;
    if (idle != 0) {
        sceClibSnprintf(why, cap, "the system's installer %s after %s s (0x%08X)",
                        idle == 1 ? "had not finished" : "could not be asked how it went",
                        secsOf(took).s, (unsigned)idle);
        return idle < 0 ? idle : -1;
    }
    int result = 0;
    rc = ((FnPromState)g_promoter[kPromGetResult].fn)(&result);
    logLine("update: installed in %s s: result 0x%08X (0x%08X)", secsOf(took).s, (unsigned)result,
            (unsigned)rc);
    if (rc < 0 || result < 0) {
        const int code = rc < 0 ? rc : result;
        sceClibSnprintf(why, cap, "the system's installer refused it (0x%08X)", (unsigned)code);
        return code;
    }
    return 0;
}

// The whole install. 0, else the code, with why.
int updateInstall(char* why, int cap) {
    char eboot[160], head[160];
    sceClibSnprintf(eboot, sizeof(eboot), "%s/eboot.bin", g_updateDir);
    sceClibSnprintf(head, sizeof(head), "%s/sce_sys/package/head.bin", g_updateDir);
    if (!exists(eboot) || !exists(head)) {
        sceClibSnprintf(why, cap, "nothing to install at %s", g_updateDir);
        return -1;
    }
    if (!updateWaitGone(kUpdateGoneMs)) {
        sceClibSnprintf(why, cap, "%s was still running %s s after it asked", g_updateApp.name,
                        secsOf(kUpdateGoneMs).s);
        return -1;
    }
    int rc = updateFindPromoter(why, cap);
    if (rc < 0) return rc;
    // Idle first, as SceShell may be installing something itself; then its
    // client begun, once, as Vitacompanion's module does.
    const int idle = updateWaitIdle(kUpdateIdleMs);
    if (idle != 0) {
        sceClibSnprintf(why, cap, "the system's installer was busy (0x%08X)", (unsigned)idle);
        return idle < 0 ? idle : -1;
    }
    if (!g_promoterInited) {
        g_promoterInited = 1;
        rc = ((FnPromInit)g_promoter[kPromInit].fn)();
        logLine("update: the installer's client begun: 0x%08X", (unsigned)rc);
    }
    // The title can be taken for running a moment after its process has gone.
    for (int attempt = 1;; attempt++) {
        rc = updatePromote(why, cap);
        if ((unsigned)rc != kPromoterInUse || attempt == 3) return rc;
        logLine("update: %s still taken for running (0x%08X): again in 2 s", g_updateApp.name,
                (unsigned)rc);
        sceKernelDelayThread(2 * 1000 * 1000);
    }
}

// What came of it, for the app to read as it starts (SC_UPDATE_RESULT).
void updateResult(const App* a, int rc, const char* why) {
    char text[224], path[160];
    const int n = sceClibSnprintf(text, sizeof(text), "result=0x%08X\n%s%s%send\n", (unsigned)rc,
                                  why[0] ? "why=" : "", why, why[0] ? "\n" : "");
    appPath(a, SC_UPDATE_RESULT, path, sizeof(path));
    const int res = writeWhole(path, text, n);
    if (res < 0) logLine("update: %s not written: 0x%08X", path, (unsigned)res);
}

// The update thread: installs, says how it went, launches the app again.
int updateThread(SceSize, void*) {
    char why[128];
    why[0] = 0;
    const int rc = updateInstall(why, sizeof(why));
    if (rc == 0)
        logLine("update: %s updated", g_updateApp.name);
    else
        logLine("update: not installed (0x%08X): %s", (unsigned)rc, why);
    updateResult(&g_updateApp, rc, why);
    char uri[48];
    sceClibSnprintf(uri, sizeof(uri), "psgm:play?titleid=%s", g_updateApp.titleId);
    const int rcLaunch = sceAppMgrLaunchAppByUri(0x20000, uri);
    logLine("update: %s launched again: 0x%08X", g_updateApp.name, (unsigned)rcLaunch);
    lock();
    g_updating = 0;
    unlock();
    return sceKernelExitDeleteThread(0);
}

// From an app as it quits to be updated (SC_CMD_UPDATE_INSTALL), the update
// thread begun. Under g_lock.
void startUpdate(int app) {
    const App* a = appAt(app);
    if (g_updating) {
        logLine("update: %s asks while one is under way", a->name);
        return;
    }
    g_updateApp = *a;
    appPath(a, SC_UPDATE_DIR, g_updateDir, sizeof(g_updateDir));
    logLine("update: %s asks for its update at %s to be installed once it has gone", a->name,
            g_updateDir);
    const SceUID thread = sceKernelCreateThread("ShellCompanion::Update", updateThread,
                                                kThreadPriority, kUpdateStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        logLine("update: no thread to install it: 0x%08X", (unsigned)rc);
        // For the app to say so once it is opened again: nothing launches it.
        updateResult(a, rc, "the plugin had no thread to install it");
        return;
    }
    g_updating = 1;
}

// ── An app's downloads, carried on ──────────────────────────────────────────
// (docs/PROTOCOL.md §5) The system closes an app to start a game, and its
// downloads stopped with it until it was opened again. As it goes it lists
// them for SceShell (SC_CMD_DL_START), which carries them on from here once
// the app has gone: each into the file the app had begun, from where that
// file ends, by HTTP Range, with the list's header lines. A `url` item is
// fetched as it is; a Plex download-queue item (plexqueue, plexadd) is
// waited for here as VitaPlex waits for it, and one not yet in the queue is
// added to it with the query and profile VitaPlex would have sent. How far
// it has got goes to SC_DOWNLOADS_STATUS, which the app reads as it takes
// them back (SC_CMD_DL_TAKE_BACK), and to a progress notification of
// SceShell's own, the one the app shows while it downloads. One download at
// a time, one app's list at a time in the order they were handed over, on a
// thread of its own at the lowest priority, which gives way to the music's
// fetches. Its lines in the log begin "download:". The files' URLs and the
// header lines are never logged.
enum { kDlWaiting, kDlGoing, kDlDone, kDlGone, kDlFailed };
const char* const kDlStateNames[5] = {"waiting", "going", "done", "gone", "failed"};
const int kDlStack = 0x8000;
// Tries at a file before it is given up, each after a wait longer than the
// last; a try that gets some of it starts the count again.
const int kDlTries = 4;
const uint32_t kDlRetryMs[kDlTries] = {0, 5000, 15000, 60000};
// How long the server may answer 503 for a file it is still preparing, and
// how long a queue item may stay unprepared, as VitaPlex waits for one
// (downloads_manager.cpp, waitForQueueItem): an hour.
const uint32_t kDlPrepareMs = 60u * 60u * 1000u;
// The status and the notification, this often while it goes.
const uint32_t kDlStatusEveryMs = 2000;
const uint32_t kDlLogEveryMs = 10000;
// How long the app has to go before the downloads are begun.
const uint32_t kDlGoneMs = 30000;
const uint32_t kDlLookMs = 250;

// A download of the list. Under g_lock; the download thread's as it runs.
struct DlItem {
    char key[48];     // the app's id for it
    int state;        // kDl*
    uint64_t bytes;   // in its file
    uint64_t total;   // its full size, 0 when not known
    char queue[16];   // the download queue's id and the item's, "" for none
    char item[16];
    int pagesDone, pageCount;  // a "pages" download's, else 0
};
// The download thread's: a page of a "pages" download as it is fetched; the
// page it is, for the notification's line (dlSub), "" for none; and the
// fetch's begun and whole lines left out of the log, as a chapter has a
// file a page.
DlItem g_dlPage;
char g_dlPageNote[24];
int g_dlQuiet;
DlItem g_dl[SC_MAX_DOWNLOADS];
int g_dlCount;
int g_dlRunning;  // under g_lock: the download thread runs
int g_dlStop;     // under g_lock: stop after the piece being written
int g_dlAgain;    // under g_lock: a new list came as it ran; it is taken up
int g_dlPaused;   // under g_lock: stopped by the notification's Yes
int g_dlApp = -1; // under g_lock: the app whose list runs
// Under g_lock: apps whose lists wait for the one running, in order.
int g_dlPending[SC_MAX_APPS];
int g_dlPendingCount;

// The download thread's: the app it works for, as it was as its run began,
// and the list's head.
App g_dlFor;
struct DlHead {
    int notify;
    int battery;         // battery=: wait below this percent off power, 0 never
    char headers[2048];  // URL-decoded: "Name: value" lines
};
// Under g_lock: the battery's percent while the downloads wait for power
// (dlBatteryHold), else -1.
int g_dlBattery = -1;
DlHead g_dlHead;
// The download thread's: the list a line at a time, as the queue is read
// (LineReader), with room for an add's profile; a piece of a file on its way
// to the card; a URL; an answer of the server's.
struct DlReader {
    SceUID fd;
    char buf[2048];
    int len, pos;
};
DlReader g_dlReader;
char g_dlLine[4096];
uint8_t g_dlBuf[16384];
char g_dlUrl[1536];
char g_dlAnswer[4096];

// Milliseconds since `from`, in 32 bits (splitUs has why there is no 64-bit
// division).
uint32_t msSince(SceUInt64 from) {
    uint32_t secs, micros;
    splitUs(sceKernelGetSystemTimeWide() - from, &secs, &micros);
    return secs * 1000u + micros / 1000u;
}


bool dlStopped() {
    lock();
    const int stop = g_dlStop;
    unlock();
    return stop != 0;
}

// Waits ms, a look at a time, for as long as nothing stops the downloads.
// false if something did.
bool dlWait(uint32_t ms) {
    for (uint32_t waited = 0; waited < ms; waited += kDlLookMs) {
        if (dlStopped()) return false;
        sceKernelDelayThread(kDlLookMs * 1000);
    }
    return !dlStopped();
}

// A count of bytes as decimal, without a 64-bit division (splitUs has why).
struct Big {
    char s[24];
};
Big bigOf(uint64_t v) {
    Big b;
    uint32_t hi, lo;
    splitUs(v, &hi, &lo);
    if (hi)
        sceClibSnprintf(b.s, sizeof(b.s), "%u%06u", (unsigned)hi, (unsigned)lo);
    else
        sceClibSnprintf(b.s, sizeof(b.s), "%u", (unsigned)lo);
    return b;
}
uint64_t toU64(const char* s) {
    uint64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10u + (uint64_t)(*s - '0');
    return v;
}
// Megabytes, "x.yy", of a size that may pass 4 GB.
Num mb64Of(uint64_t bytes) {
    Num m;
    sceClibSnprintf(m.s, sizeof(m.s), "%u.%02u", (unsigned)(bytes >> 20),
                    (unsigned)((((uint32_t)bytes & 0xFFFFF) * 100u) >> 20));
    return m;
}
// bytes of total in percent, in 32 bits.
uint32_t percentOf(uint64_t bytes, uint64_t total) {
    if (bytes > total) bytes = total;
    while (total >= (1u << 24)) {
        total >>= 1;
        bytes >>= 1;
    }
    if (total == 0) return 0;
    return (uint32_t)bytes * 100u / (uint32_t)total;
}

// A file's size on the card, 0 when it is not there.
uint64_t fileSize(const char* path) {
    SceIoStat st;
    sceClibMemset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0 || st.st_size < 0) return 0;
    return (uint64_t)st.st_size;
}

// The list's next line in g_dlLine, as readerNext reads the queue. false at
// its end.
bool dlNext() {
    int n = 0;
    bool any = false;
    for (;;) {
        if (g_dlReader.pos == g_dlReader.len) {
            const int got = sceIoRead(g_dlReader.fd, g_dlReader.buf, sizeof(g_dlReader.buf));
            g_dlReader.len = got > 0 ? got : 0;
            g_dlReader.pos = 0;
            if (g_dlReader.len == 0) break;
        }
        any = true;
        const char c = g_dlReader.buf[g_dlReader.pos++];
        if (c == '\n') break;
        if (c != '\r' && n < (int)sizeof(g_dlLine) - 1) g_dlLine[n++] = c;
    }
    g_dlLine[n] = 0;
    return any;
}

bool dlOpenList() {
    char path[160];
    appPath(&g_dlFor, SC_DOWNLOADS, path, sizeof(path));
    g_dlReader.fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    g_dlReader.len = g_dlReader.pos = 0;
    return g_dlReader.fd >= 0;
}

void dlCloseList() {
    if (g_dlReader.fd >= 0) sceIoClose(g_dlReader.fd);
    g_dlReader.fd = -1;
}

// An item's fields, cut at its tabs and URL-decoded, in place: how many.
int dlFields(char* line, char** f, int cap) {
    int n = 0;
    f[n++] = line;
    for (char* p = line; *p && n < cap; p++)
        if (*p == '\t') {
            *p = 0;
            f[n++] = p + 1;
        }
    // Decoding never makes a field longer, so each is decoded where it is.
    for (int i = 0; i < n; i++)
        urlDecode(f[i], (int)sceClibStrnlen(f[i], sizeof(g_dlLine)), f[i],
                  (int)sceClibStrnlen(f[i], sizeof(g_dlLine)) + 1, false);
    return n;
}
// The fields every item has, then how it is fetched (kind) and the kind's
// own: url's URL and extra header lines; plexqueue's server, queue id and
// item id; plexadd's server, query and profile extra.
enum { kFKey, kFPath, kFSize, kFTitle, kFKind, kFArg1, kFArg2, kFArg3, kFMax };

// The list read through for its head and its items, which g_dl then holds,
// waiting. false when it is not there or not whole.
bool dlReadList() {
    sceClibMemset(&g_dlHead, 0, sizeof(g_dlHead));
    g_dlHead.battery = SC_BATTERY_PAUSE_PERCENT;
    if (!dlOpenList()) return false;
    int count = 0;
    bool whole = false;
    char* f[kFMax];
    while (dlNext()) {
        if (same(g_dlLine, "end")) {
            whole = true;
            break;
        }
        if (startsWith(g_dlLine, "notify=")) {
            g_dlHead.notify = g_dlLine[7] == '1';
        } else if (startsWith(g_dlLine, "battery=")) {
            const int pct = toInt(g_dlLine + 8);
            g_dlHead.battery = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        } else if (startsWith(g_dlLine, "headers=")) {
            urlDecode(g_dlLine + 8, (int)sceClibStrnlen(g_dlLine + 8, sizeof(g_dlLine)),
                      g_dlHead.headers, sizeof(g_dlHead.headers), true);
        } else if (startsWith(g_dlLine, "item=") && count < SC_MAX_DOWNLOADS) {
            const int nf = dlFields(g_dlLine + 5, f, kFMax);
            if (nf <= kFArg1) continue;
            const uint64_t bytes = fileSize(f[kFPath]);  // not under the lock: the card
            lock();
            DlItem* d = &g_dl[count];
            sceClibMemset(d, 0, sizeof(*d));
            copyText(d->key, f[kFKey], sizeof(d->key));
            d->state = kDlWaiting;
            d->total = toU64(f[kFSize]);
            d->bytes = bytes;
            if (same(f[kFKind], "pages") && nf > kFArg2) d->pageCount = toInt(f[kFArg2]);
            unlock();
            count++;
        }
    }
    dlCloseList();
    lock();
    g_dlCount = whole ? count : 0;
    unlock();
    return whole;
}

// The status (docs/PROTOCOL.md §5): written beside its name and moved in.
// Under nothing: g_dl is read under g_lock a line at a time.
int g_dlStatusErrors;
void dlStatus(bool running) {
    char text[200], path[160], part[168];
    appPath(&g_dlFor, SC_DOWNLOADS_STATUS, path, sizeof(path));
    sceClibSnprintf(part, sizeof(part), "%s.tmp", path);
    const SceUID fd = sceIoOpen(part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    int rc = fd;
    if (fd >= 0) {
        uint32_t secs, micros;
        splitUs(sceKernelGetSystemTimeWide(), &secs, &micros);
        lock();
        const int battery = g_dlBattery;
        const int paused = g_dlPaused       ? SC_PAUSED_ASKED
                         : battery >= 0     ? SC_PAUSED_BATTERY
                                            : 0;
        unlock();
        int n = sceClibSnprintf(text, sizeof(text), "written=%u.%06u\nrunning=%d\npaused=%d\n",
                                (unsigned)secs, (unsigned)micros, running ? 1 : 0, paused);
        if (battery >= 0)
            n += sceClibSnprintf(text + n, sizeof(text) - n, "battery=%d\n", battery);
        rc = sceIoWrite(fd, text, (SceSize)n) == n ? 0 : -1;
        for (int i = 0; rc == 0; i++) {
            lock();
            const bool more = i < g_dlCount;
            if (more) {
                const DlItem* d = &g_dl[i];
                n = sceClibSnprintf(text, sizeof(text), "item=%s\t%s\t%s\t%s\t%s\t%s", d->key,
                                    kDlStateNames[d->state], bigOf(d->bytes).s,
                                    bigOf(d->total).s, d->queue, d->item);
                if (d->pageCount > 0)
                    n += sceClibSnprintf(text + n, sizeof(text) - n, "\t%d/%d", d->pagesDone,
                                         d->pageCount);
                n += sceClibSnprintf(text + n, sizeof(text) - n, "\n");
            }
            unlock();
            if (!more) break;
            rc = sceIoWrite(fd, text, (SceSize)n) == n ? 0 : -1;
        }
        if (rc == 0) rc = sceIoWrite(fd, "end\n", 4) == 4 ? 0 : -1;
        const int rcClose = sceIoClose(fd);
        if (rc == 0) rc = rcClose;
    }
    if (rc >= 0) {
        // rename does not replace a file on every target (player.c's moveIn).
        sceIoRemove(path);
        rc = sceIoRename(part, path);
    }
    if (rc < 0 && g_dlStatusErrors < 3) {
        g_dlStatusErrors++;
        logLine("download: status not written: 0x%08X", (unsigned)rc);
    }
}

// ── The downloads' progress notification ──
// SceNotificationUtil's progress notification, the one a system download
// shows, from SceShell, found with taiHEN as the HTTP
// library is: SceShell's module of it, else the module loaded into SceShell
// through SceSysmodule. Its calls go to SceShell's notification service from
// this thread, never SceShell's UI thread, as the music service's do. Where it
// is not there, or Begin refuses, the downloads go on without it.
const uint32_t kNotifyProgressLibrary = 0x75602769;  // SceNotificationUtilProgress
enum { kNotifyBegin, kNotifyUpdate, kNotifyFinish, kNotifyFns };
LibFn g_notify[kNotifyFns] = {{"sceNotificationUtilProgressBegin", 0xFD0769B0, 0},
                              {"sceNotificationUtilProgressUpdate", 0xD9890A65, 0},
                              {"sceNotificationUtilProgressFinish", 0x8CD688A1, 0}};
typedef int (*FnNotify)(void*);
// The parameters, as vitasdk's notificationutil.h lays them out: two lines of
// 63 UTF-16 units, each followed by a 0, then the rest. Begin's handler, the
// one its module calls with each event, is the word at 0x4E8, not the header's
// eventHandler at 0x4EC, and must not be 0 (docs/DESIGN_NOTES.md, "Vita:
// notifications yes, background downloads no").
struct NotifyText {
    uint16_t text[63];
    int16_t sep0;
    uint16_t sub[63];
    int16_t sep1;
};
struct NotifyBeginParam {
    NotifyText t;
    uint8_t rest[0x4F0 - sizeof(NotifyText)];
    uint16_t cancelText[63];  // the question, at 0x4F0
    int16_t sep2;
};
static_assert(sizeof(NotifyBeginParam) == 0x570, "Begin sends 0x570 bytes");
struct NotifyUpdateParam {
    NotifyText t;
    float progress;
    uint8_t reserved[0x38];
};
struct NotifyFinishParam {
    NotifyText t;
    char path[0x3E8];
};
const uint32_t kNotifyHandlerOffset = 0x4E8;
// Begin sends SceShell 0x570 bytes of its parameters, not the 0x4F0 vitasdk's
// struct has (the method 0x7000B call in Begin's code, read from the build
// 2058 dump): the 0x80 after it are a third line, the question SceShell asks
// when the notification's cancel is pressed, with No and Yes under it. With
// the struct at 0x4F0 the question was whatever came after it in memory:
// blank here, two characters of the stack in VitaPlex (builds 2063 to 2065).
// The notification is the plugin's for all of a download run, the app's
// part of it included: the app shows its progress through here while it
// downloads (SC_CMD_DL_PROGRESS), so that the one notification goes on as
// SceShell carries the downloads on once the app has gone, and as the app
// takes them back. A notification is its process's, and no call names one
// (Update and Finish take only text): an app's own could be gone on with by
// nothing once it had quit, and SceShell had to begin a second one
// (VitaPlex's builds 2063 to 2069). Under g_notifyLock: the download thread
// and the receiving thread both show it, and the watch thread ends one left
// over.
SceUID g_notifyLock = -1;
int g_notifyFound;    // 0 not looked for, 1 there, -1 not there
int g_notifyLive;     // Begin has shown it
int g_notifyFailed;   // an Update of it has failed, logged once
SceUInt64 g_notifyHeldUs;  // taken back with it showing: when, for notifyHeldTick
union {
    NotifyBeginParam begin;
    NotifyUpdateParam update;
    NotifyFinishParam finish;
} g_notifyParam;
// Its owner, for its icon (statusAsOwner): the app it is shown for, its
// title (written under g_notifyLock, read by the hook without it), the hook,
// and the window after a Begin in which it answers.
int g_notifyApp = -1;
char g_notifyTitle[12];
TaiHookRef g_statusHookRef;
SceUInt64 g_ownerUntilUs;  // atomic: the window open, until then
const SceUInt64 kOwnerWindowUs = 5ull * 1000 * 1000;
// How long a notification left showing as the app takes the downloads back
// waits for the app to go on with it before it is ended.
const SceUInt64 kNotifyHeldUs = 30ull * 1000 * 1000;

// Its events: 0 when its question ("Pause downloads?", the third line of
// Begin's parameters) is answered Yes; No sends none (build 2072). Called
// from inside Update, on the thread that called it, with its module's lock
// held: nothing here calls back into it, and the pause is the watch thread's
// (notifyPauseTick).
int g_notifyPauseAsked;  // atomic
void notifyEvent(int id) {
    logLine("notification: event %d%s", id, id == 0 ? ", Yes: the downloads pause" : "");
    if (id == 0) __atomic_store_n(&g_notifyPauseAsked, 1, __ATOMIC_SEQ_CST);
}

bool notifyFind() {
    if (g_notifyFound) return g_notifyFound > 0;
    g_notifyFound = -1;
    uintptr_t fn = 0;
    int rc = scGetModuleExportFunc("SceNotificationUtil", kNotifyProgressLibrary,
                                           g_notify[0].nid, &fn);
    if (rc < 0) {
        rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
        logLine("notification: SceNotificationUtil loaded into SceShell: 0x%08X", (unsigned)rc);
        if (rc < 0) return false;
    }
    for (int i = 0; i < kNotifyFns; i++) {
        fn = 0;
        rc = scGetModuleExportFunc("SceNotificationUtil", kNotifyProgressLibrary,
                                           g_notify[i].nid, &fn);
        g_notify[i].fn = rc >= 0 ? fn : 0;
        if (rc < 0) {
            logLine("notification: %s not in SceNotificationUtil: 0x%08X", g_notify[i].name,
                    (unsigned)rc);
            return false;
        }
    }
    g_notifyFound = 1;
    return true;
}

// UTF-16 into one of the parameters' lines, cut at 63 units, ended with 0.
void lineOf(uint16_t* to, const uint16_t* from) {
    int n = 0;
    for (; from[n] && n < 63; n++) to[n] = from[n];
    for (; n < 63; n++) to[n] = 0;
}

// The notification shown, or brought up to date: progress from 0 to 1, or
// below 0 when not known. UTF-16 lines.
void notifyShow16(const uint16_t* text, const uint16_t* sub, float progress) {
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    g_notifyHeldUs = 0;
    if (!notifyFind()) {
        sceKernelUnlockMutex(g_notifyLock, 1);
        return;
    }
    sceClibMemset(&g_notifyParam, 0, sizeof(g_notifyParam));
    if (!g_notifyLive) {
        lineOf(g_notifyParam.begin.t.text, text);
        lineOf(g_notifyParam.begin.t.sub, sub);
        fromUtf8("Pause downloads?", g_notifyParam.begin.cancelText, 63);
        void (*handler)(int) = &notifyEvent;
        sceClibMemcpy((uint8_t*)&g_notifyParam.begin + kNotifyHandlerOffset, &handler,
                      sizeof(handler));
        // Its owner, for its icon (statusAsOwner).
        if (g_statusHookRef)
            __atomic_store_n(&g_ownerUntilUs, sceKernelGetSystemTimeWide() + kOwnerWindowUs,
                             __ATOMIC_SEQ_CST);
        const int rc = ((FnNotify)g_notify[kNotifyBegin].fn)(&g_notifyParam.begin);
        logLine("notification: begun: 0x%08X", (unsigned)rc);
        if (rc < 0) {
            g_notifyFound = -1;  // not tried again this boot
        } else {
            g_notifyLive = 1;
            g_notifyFailed = 0;
        }
        sceKernelUnlockMutex(g_notifyLock, 1);
        return;
    }
    lineOf(g_notifyParam.update.t.text, text);
    lineOf(g_notifyParam.update.t.sub, sub);
    // In percent: SceShell keeps the float as it comes (its 0x7000D handler
    // stores +0x100 straight into the notification) and its bar runs to 100,
    // so a fraction left the bar at its start (builds up to 2075).
    g_notifyParam.update.progress =
        progress < 0.0f ? 0.0f : progress > 1.0f ? 100.0f : progress * 100.0f;
    const int rc = ((FnNotify)g_notify[kNotifyUpdate].fn)(&g_notifyParam.update);
    // As VitaPlex's: closed in SceShell, it may refuse its updates.
    if (rc < 0 && !g_notifyFailed) {
        g_notifyFailed = 1;
        logLine("notification: not brought up to date: 0x%08X", (unsigned)rc);
    }
    sceKernelUnlockMutex(g_notifyLock, 1);
}

// Under g_notifyLock.
void notifyEndLocked(const uint16_t* text, const uint16_t* sub) {
    g_notifyHeldUs = 0;
    if (!g_notifyLive) return;
    g_notifyLive = 0;
    sceClibMemset(&g_notifyParam, 0, sizeof(g_notifyParam));
    lineOf(g_notifyParam.finish.t.text, text);
    lineOf(g_notifyParam.finish.t.sub, sub);
    const int rc = ((FnNotify)g_notify[kNotifyFinish].fn)(&g_notifyParam.finish);
    logLine("notification: finished: 0x%08X", (unsigned)rc);
}

void notifyEnd16(const uint16_t* text, const uint16_t* sub) {
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    notifyEndLocked(text, sub);
    sceKernelUnlockMutex(g_notifyLock, 1);
}

// The notification shown for app from now: its icon, and the pause file.
void notifyFor(int app) {
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    lock();
    const App* a = appAt(app);
    if (a) {
        g_notifyApp = app;
        sceClibMemcpy(g_notifyTitle, a->titleId, sizeof(g_notifyTitle));
    }
    unlock();
    sceKernelUnlockMutex(g_notifyLock, 1);
}

// The download thread's, in UTF-8, with the list's say.
void notifyShow(const char* text, const char* sub, float progress) {
    if (!g_dlHead.notify) return;
    uint16_t t[64], u[64];
    fromUtf8(text, t, 64);
    fromUtf8(sub, u, 64);
    notifyShow16(t, u, progress);
}

void notifyEnd(const char* text, const char* sub) {
    uint16_t t[64], u[64];
    fromUtf8(text, t, 64);
    fromUtf8(sub, u, 64);
    notifyEnd16(t, u);
}

// Taken back with the notification showing: the app goes on with it as it
// downloads again (SC_CMD_DL_PROGRESS). Ended by notifyHeldTick if it does not.
void notifyHold() {
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    if (g_notifyLive) g_notifyHeldUs = sceKernelGetSystemTimeWide();
    sceKernelUnlockMutex(g_notifyLock, 1);
}

// The watch thread's: a notification held for the app longer than
// kNotifyHeldUs, ended. Never under g_lock while it waits for its own.
void notifyHeldTick(SceUInt64 now) {
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    if (g_notifyLive && g_notifyHeldUs && now - g_notifyHeldUs > kNotifyHeldUs) {
        char name[48];
        lock();
        copyText(name, appName(g_notifyApp), sizeof(name));
        unlock();
        logLine("notification: %s did not go on with it in 30 s; ended", name);
        uint16_t t[64], u[64];
        fromUtf8(name, t, 64);
        fromUtf8("Downloads paused", u, 64);
        notifyEndLocked(t, u);
    }
    sceKernelUnlockMutex(g_notifyLock, 1);
}

// The watch thread's: the notification's Yes carried out. The downloads
// SceShell carries on stop, paused, and the notification ends here; those
// the app downloads pause as it finds SC_DOWNLOADS_PAUSE in its folder, and
// it ends the notification itself. Never under g_lock.
void notifyPauseTick() {
    if (!__atomic_exchange_n(&g_notifyPauseAsked, 0, __ATOMIC_SEQ_CST)) return;
    lock();
    const int running = g_dlRunning;
    if (running) {
        g_dlStop = 1;
        g_dlAgain = 0;
        g_dlPaused = 1;
    }
    unlock();
    if (running) {
        logLine("download: paused from the notification");
        return;  // dlRun ends the notification as it stops
    }
    char path[160], name[48];
    path[0] = 0;
    sceKernelLockMutex(g_notifyLock, 1, NULL);
    lock();
    const App* a = appAt(g_notifyApp);
    if (a) appPath(a, SC_DOWNLOADS_PAUSE, path, sizeof(path));
    copyText(name, appName(g_notifyApp), sizeof(name));
    unlock();
    sceKernelUnlockMutex(g_notifyLock, 1);
    if (!path[0]) {
        logLine("download: pause asked, with no app's downloads to pause");
        return;
    }
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
    logLine("download: %s asked to pause its downloads (0x%08X)", name, (unsigned)fd);
}

// From an app (SC_CMD_DL_PROGRESS, SC_CMD_DL_PROGRESS_END): what it asked
// for, taken by the receiving thread under g_lock and shown after it, so that
// no notification call is made under g_lock.
struct NotifyAsk {
    int due;    // 1 show, 2 end
    int app;
    uint32_t permille;
    uint16_t text[64], sub[64];
};
NotifyAsk g_notifyAsk;  // the receiving thread's alone

void notifyAsked() {
    if (!g_notifyAsk.due) return;
    const int due = g_notifyAsk.due;
    g_notifyAsk.due = 0;
    notifyFor(g_notifyAsk.app);
    if (due == 1)
        notifyShow16(g_notifyAsk.text, g_notifyAsk.sub,
                     g_notifyAsk.permille == SC_PROGRESS_UNKNOWN
                         ? -1.0f
                         : (float)(g_notifyAsk.permille > 1000 ? 1000 : g_notifyAsk.permille) /
                               1000.0f);
    else
        notifyEnd16(g_notifyAsk.text, g_notifyAsk.sub);
}

// ── Asking the server ──
// A request for the downloads: with the list's header lines, and `extra`
// lines after them (an item's own, or what a Plex download-queue request
// adds); POST when post. 0 with *conn and *req to read and close, else the
// failed call's code. Logged as `what`.
int dlOpen(int tmpl, const char* what, const char* url, const char* range, bool post,
           const char* extra, int* conn, int* req) {
    HttpAsk ask = getWith(g_dlHead.headers);
    ask.headers2 = extra;
    ask.range = range;
    if (post) ask.method = "POST";
    return httpOpen(tmpl, "download", what, url, ask, conn, req);
}

// A short answer of the server's, into g_dlAnswer, ended with 0: its HTTP
// status, or the failed call's code.
int dlAsk(int tmpl, const char* what, const char* url, bool post, const char* profileExtra) {
    int conn, req, status = 0;
    g_dlAnswer[0] = 0;
    // JSON asked for, and the profile an add carries.
    static char extra[1280];
    if (profileExtra)
        sceClibSnprintf(extra, sizeof(extra),
                        "Accept: application/json\r\nX-Plex-Client-Profile-Name: Generic\r\n"
                        "X-Plex-Client-Profile-Extra: %s\r\n",
                        profileExtra);
    else
        sceClibSnprintf(extra, sizeof(extra), "Accept: application/json\r\n");
    int rc = dlOpen(tmpl, what, url, NULL, post, extra, &conn, &req);
    if (rc < 0) return rc;
    httpFn<FnStatus>(kHttpStatus)(req, &status);
    int n = 0;
    while (n < (int)sizeof(g_dlAnswer) - 1) {
        const int got = httpFn<FnRead>(kHttpRead)(req, g_dlAnswer + n,
                                                  (unsigned)(sizeof(g_dlAnswer) - 1 - n));
        if (got <= 0) break;
        n += got;
    }
    g_dlAnswer[n] = 0;
    httpClose(conn, req);
    return status;
}

// The first number under "key" in a JSON answer, as VitaPlex's extractJsonInt
// takes it; 0 when there is none.
uint32_t jsonNumber(const char* json, const char* key) {
    char pat[40];
    const int plen = sceClibSnprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* v = findText(json, pat);
    if (!v) return 0;
    v += plen;
    while (*v == ' ' || *v == ':' || *v == '"') v++;
    return toUint(v);
}

// (plexadd) The item added to the client's download queue on a Plex server,
// with the query and profile VitaPlex would have added it with: the queue's
// id and the item's in d. true when the server gave both. The download
// queue is the client's: the list's header lines name it
// (X-Plex-Client-Identifier).
bool dlAdd(int tmpl, DlItem* d, const char* server, const char* query, const char* profile,
           const char* label) {
    sceClibSnprintf(g_dlUrl, sizeof(g_dlUrl), "%s/downloadQueue", server);
    int status = dlAsk(tmpl, "the download queue", g_dlUrl, true, NULL);
    const uint32_t queue = status == 200 ? jsonNumber(g_dlAnswer, "id") : 0;
    if (queue == 0) {
        logLine("download: %s: no download queue (HTTP %d)", label, status);
        return false;
    }
    const int len = sceClibSnprintf(g_dlUrl, sizeof(g_dlUrl), "%s/downloadQueue/%u/add%s",
                                    server, (unsigned)queue, query);
    if (len >= (int)sizeof(g_dlUrl)) {
        logLine("download: %s: the add's URL is longer than the %u bytes here", label,
                (unsigned)sizeof(g_dlUrl));
        return false;
    }
    status = dlAsk(tmpl, "the add", g_dlUrl, true, profile);
    const uint32_t item = status == 200 ? jsonNumber(g_dlAnswer, "id") : 0;
    if (item == 0) {
        logLine("download: %s: not added to download queue %u (HTTP %d)", label,
                (unsigned)queue, status);
        return false;
    }
    lock();
    sceClibSnprintf(d->queue, sizeof(d->queue), "%u", (unsigned)queue);
    sceClibSnprintf(d->item, sizeof(d->item), "%u", (unsigned)item);
    unlock();
    logLine("download: %s: added to download queue %u as item %u", label, (unsigned)queue,
            (unsigned)item);
    return true;
}

// What the notification's second line says, for n of count.
void dlSub(char* out, int cap, int n, int count, const char* what) {
    char page[32] = "";
    if (g_dlPageNote[0]) sceClibSnprintf(page, sizeof(page), "%s \xC2\xB7 ", g_dlPageNote);
    if (count > 1)
        sceClibSnprintf(out, cap, "%d of %d \xC2\xB7 %s%s", n + 1, count, page, what);
    else
        sceClibSnprintf(out, cap, "%s%s", page, what);
}

enum { kPrepReady, kPrepGone, kPrepNotReady, kPrepStopped };
// The queue item d waited for until the server has prepared its file, as
// VitaPlex waits for it (waitForQueueItem): its status looked at every 5 s at
// first, every 30 s after 5 minutes. kPrepReady, kPrepGone when the server no
// longer has it or gave it up, kPrepNotReady after an hour or five failures in a
// row, kPrepStopped when the downloads are taken back.
int dlPrepared(int tmpl, DlItem* d, const char* server, const char* title, int n, int count,
               const char* label) {
    sceClibSnprintf(g_dlUrl, sizeof(g_dlUrl), "%s/downloadQueue/%s/items/%s", server, d->queue,
                    d->item);
    char last[24], status[24], sub[96];
    last[0] = 0;
    int errors = 0;
    const SceUInt64 t0 = sceKernelGetSystemTimeWide();
    for (;;) {
        const uint32_t ms = msSince(t0);
        if (ms > kDlPrepareMs) {
            logLine("download: %s: not prepared on the server in an hour", label);
            return kPrepNotReady;
        }
        const int http = dlAsk(tmpl, "its queue item", g_dlUrl, false, NULL);
        if (http == 404) {
            logLine("download: %s: the server no longer has queue item %s", label, d->item);
            return kPrepGone;
        }
        if (http != 200) {
            if (++errors >= 5) {
                logLine("download: %s: its queue item would not answer (%d)", label, http);
                return kPrepNotReady;
            }
        } else {
            errors = 0;
            jsonString(g_dlAnswer, "status", status, sizeof(status));
            const uint32_t progress = jsonNumber(g_dlAnswer, "progress");
            if (!same(status, last)) {
                logLine("download: %s: queue item %s %s, %u%%", label, d->item, status,
                        (unsigned)progress);
                copyText(last, status, sizeof(last));
            }
            if (same(status, "available")) return kPrepReady;
            if (same(status, "error") || same(status, "expired")) return kPrepGone;
            char what[48];
            sceClibSnprintf(what, sizeof(what), "Preparing on server %u%%", (unsigned)progress);
            dlSub(sub, sizeof(sub), n, count, what);
            notifyShow(title, sub, progress > 0 && progress <= 100 ? (float)progress / 100.0f : -1.0f);
        }
        dlStatus(true);
        const uint32_t wait = ms < 30000 ? 5000 : ms < 120000 ? 10000 : ms < 300000 ? 20000 : 30000;
        if (!dlWait(wait)) return kPrepStopped;
    }
}

// ── The battery ──
// A download goes on only while the battery holds: below the list's
// battery= percent with the Vita off power, it waits, the connection
// closed, until the Vita is on power or the battery is 5 points above that,
// and the notification says why. ScePower is imported as it is: a library
// every process may call, which SceShell's own battery plugins (ShellBat)
// import too.
const uint32_t kDlBatteryLookMs = 5000;

// The battery's percent, or -1 when it is on power or holds (or cannot be
// read): what the downloads wait on.
int dlBatteryLow(int resumeAbove = 0) {
    const int limit = g_dlHead.battery;
    if (limit <= 0) return -1;
    if (scePowerIsPowerOnline() > 0 || scePowerIsBatteryCharging() > 0) return -1;
    const int pct = scePowerGetBatteryLifePercent();
    if (pct < 0) return -1;
    return pct < limit + resumeAbove ? pct : -1;
}

// Waits while the battery is low (dlBatteryLow), the notification saying so
// for download n of count. 0 when it was not, 1 when it waited and holds
// now, -1 when the downloads were stopped meanwhile.
int dlBatteryHold(int n, int count) {
    int pct = dlBatteryLow();
    if (pct < 0) return 0;
    logLine("download: the battery at %d%%, off power: waiting for it (below %d%%)", pct,
            g_dlHead.battery);
    const SceUInt64 began = sceKernelGetSystemTimeWide();
    int shownPct = -1;
    while (pct >= 0) {
        if (pct != shownPct) {
            lock();
            g_dlBattery = pct;
            unlock();
            char line[64], sub[96];
            sceClibSnprintf(line, sizeof(line), "Paused: battery at %d%%", pct);
            sceClibSnprintf(sub, sizeof(sub), "Charge the Vita to go on (%d of %d)", n + 1, count);
            notifyShow(line, sub, -1.0f);
            dlStatus(true);
            shownPct = pct;
        }
        if (!dlWait(kDlBatteryLookMs)) {
            lock();
            g_dlBattery = -1;
            unlock();
            return -1;
        }
        pct = dlBatteryLow(5);
    }
    lock();
    g_dlBattery = -1;
    unlock();
    logLine("download: going on after %s s waiting for power (battery %d%%%s)",
            secsOf(msSince(began)).s, scePowerGetBatteryLifePercent(),
            scePowerIsPowerOnline() > 0 ? ", on power" : "");
    dlStatus(true);
    return 1;
}

// kDl* of fetching url into path from where the file ends: the bytes and full
// size in d as they come.
int dlFetch(int tmpl, DlItem* d, const char* url, const char* path, const char* extra,
            const char* title, int n, int count, const char* label) {
    char range[48], sub[96], what[64];
    int tries = 0;
    SceUInt64 prepareFrom = 0;
    for (;;) {
        if (dlStopped()) return kDlGoing;
        if (dlBatteryHold(n, count) < 0) return kDlGoing;
        uint64_t have = fileSize(path);
        if (d->total > 0 && have == d->total) return kDlDone;
        if (have > 0) sceClibSnprintf(range, sizeof(range), "bytes=%s-", bigOf(have).s);
        int conn, req, status = 0;
        int rc = dlOpen(tmpl, label, url, have > 0 ? range : NULL, false, extra, &conn, &req);
        if (rc >= 0) {
            httpFn<FnStatus>(kHttpStatus)(req, &status);
            unsigned long long length = 0;
            const int rcLength = httpFn<FnLength>(kHttpLength)(req, &length);
            if (status == 416 && have > 0) {
                httpClose(conn, req);
                lock();
                d->bytes = d->total = have;
                unlock();
                return kDlDone;
            }
            if (status == 404 || status == 410) {
                httpClose(conn, req);
                logLine("download: %s: HTTP %d", label, status);
                return kDlGone;
            }
            if (status == 503) {
                // Still being prepared: waited for, a while at a time.
                httpClose(conn, req);
                if (!prepareFrom) prepareFrom = sceKernelGetSystemTimeWide();
                if (msSince(prepareFrom) > kDlPrepareMs) {
                    logLine("download: %s: the server answered 503 for an hour", label);
                    return kDlFailed;
                }
                dlSub(sub, sizeof(sub), n, count, "Waiting for the server");
                notifyShow(title, sub, -1.0f);
                dlStatus(true);
                if (!dlWait(10000)) return kDlGoing;
                continue;
            }
            if (status != 200 && status != 206) {
                httpClose(conn, req);
                logLine("download: %s: HTTP %d", label, status);
                rc = -1;
            } else {
                // A whole file when the server answers 200: written anew.
                const bool append = status == 206 && have > 0;
                if (!append) have = 0;
                const uint64_t total = rcLength >= 0 ? have + length : 0;
                if (append && d->total > 0 && total > 0 && total != d->total)
                    logLine("download: %s: the server gives its size as %s, not %s", label,
                            bigOf(total).s, bigOf(d->total).s);
                lock();
                d->state = kDlGoing;
                d->bytes = have;
                if (total > 0) d->total = total;
                unlock();
                const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT |
                                                      (append ? SCE_O_APPEND : SCE_O_TRUNC),
                                            0666);
                if (fd < 0) {
                    httpClose(conn, req);
                    logLine("download: %s: its file not opened: 0x%08X", label, (unsigned)fd);
                    return kDlFailed;
                }
                if (!g_dlQuiet)
                    logLine("download: %s: %s from %s MB of %s MB", label,
                            append ? "going on" : "begun", mb64Of(have).s, mb64Of(d->total).s);
                const SceUInt64 began = sceKernelGetSystemTimeWide();
                SceUInt64 shown = 0, logged = began;
                uint64_t shownBytes = have;
                const uint64_t from = have;
                for (;;) {
                    if (dlStopped()) {
                        rc = 1;
                        break;
                    }
                    // The music's fetch first: a song wanted now, a download not.
                    lock();
                    const bool songFetching = g_fetch.state == kFetchRunning;
                    unlock();
                    if (songFetching) sceKernelDelayThread(50 * 1000);
                    const int got = httpFn<FnRead>(kHttpRead)(req, g_dlBuf, sizeof(g_dlBuf));
                    if (got <= 0) {
                        rc = got;
                        break;
                    }
                    // Taken back as it read: not written, the app may have the
                    // file again.
                    if (dlStopped()) {
                        rc = 1;
                        break;
                    }
                    const int wrote = sceIoWrite(fd, g_dlBuf, (SceSize)got);
                    if (wrote != got) {
                        logLine("download: %s: not written: 0x%08X", label,
                                (unsigned)(wrote < 0 ? wrote : -1));
                        rc = -2;
                        break;
                    }
                    have += (uint64_t)got;
                    const SceUInt64 now = sceKernelGetSystemTimeWide();
                    if (now - shown >= kDlStatusEveryMs * 1000ull && dlBatteryLow() >= 0) {
                        rc = 3;  // waited for with the connection closed, then gone on
                        break;
                    }
                    if (now - shown >= kDlStatusEveryMs * 1000ull) {
                        lock();
                        d->bytes = have;
                        unlock();
                        const uint32_t ms = shown ? msSince(shown) : 0;
                        const uint32_t kbs =
                            ms ? (uint32_t)((have - shownBytes) >> 10) * 1000u / ms : 0;
                        if (d->total > 0 && kbs > 0)
                            sceClibSnprintf(what, sizeof(what), "%u%% \xC2\xB7 %u.%u MB/s",
                                            (unsigned)percentOf(have, d->total),
                                            (unsigned)(kbs >> 10),
                                            (unsigned)((kbs & 1023) * 10u >> 10));
                        else if (d->total > 0)
                            sceClibSnprintf(what, sizeof(what), "%u%%",
                                            (unsigned)percentOf(have, d->total));
                        else
                            sceClibSnprintf(what, sizeof(what), "%s MB", mb64Of(have).s);
                        dlSub(sub, sizeof(sub), n, count, what);
                        notifyShow(title, sub,
                                   d->total > 0 ? (float)percentOf(have, d->total) / 100.0f
                                                : -1.0f);
                        dlStatus(true);
                        shown = now;
                        shownBytes = have;
                    }
                    if (now - logged >= kDlLogEveryMs * 1000ull) {
                        logged = now;
                        logLine("download: %s: %s of %s MB", label, mb64Of(have).s,
                                mb64Of(d->total).s);
                    }
                }
                sceIoClose(fd);
                httpClose(conn, req);
                lock();
                d->bytes = have;
                unlock();
                const uint32_t ms = msSince(began);
                if (rc == 1) {
                    logLine("download: %s: stopped at %s MB", label, mb64Of(have).s);
                    return kDlGoing;
                }
                if (rc == 3) {
                    logLine("download: %s: held at %s MB for the battery", label,
                            mb64Of(have).s);
                    continue;  // dlBatteryHold waits, then it goes on from there
                }
                if (rc == 0 && (d->total == 0 || have >= d->total)) {
                    if (!g_dlQuiet)
                        logLine("download: %s: whole, %s MB, %s MB of it in %s s", label,
                                mb64Of(have).s, mb64Of(have - from).s, secsOf(ms).s);
                    lock();
                    if (d->total == 0) d->total = have;
                    unlock();
                    return kDlDone;
                }
                if (rc == -2) return kDlFailed;
                logLine("download: %s: cut off at %s MB (0x%08X)", label, mb64Of(have).s,
                        (unsigned)rc);
                if (have > from) tries = 0;  // it got somewhere: the count starts again
            }
        }
        if (++tries >= kDlTries) {
            logLine("download: %s: given up after %d tries", label, kDlTries);
            return kDlFailed;
        }
        dlStatus(true);
        if (!dlWait(kDlRetryMs[tries])) return kDlGoing;
    }
}

// ── A chapter: a file a page ──
// A "pages" download: a URL template and a file template with {page} in
// them, the page number from `first` on, or {page:N}, padded to N digits;
// and how many pages. Each page is fetched as a url download is, into
// "<file>.part", moved in once whole: a page whose file is there is done,
// with no request for it when the chapter goes on.
bool pageExpand(const char* tmpl, int page, char* out, int cap) {
    int n = 0;
    for (const char* p = tmpl; *p;) {
        int digits = 0;
        const char* after = NULL;
        if (startsWith(p, "{page}")) {
            after = p + 6;
        } else if (startsWith(p, "{page:") && p[6] >= '1' && p[6] <= '9' && p[7] == '}') {
            digits = p[6] - '0';
            after = p + 8;
        }
        if (after) {
            char v[16];
            const int k = sceClibSnprintf(v, sizeof(v), "%0*d", digits, page);
            for (int i = 0; i < k; i++, n++)
                if (n < cap - 1) out[n] = v[i];
            p = after;
        } else {
            if (n < cap - 1) out[n] = *p;
            n++;
            p++;
        }
    }
    if (n >= cap) {
        out[0] = 0;
        return false;
    }
    out[n] = 0;
    return true;
}

// The download's pages, f's: URL template, "count[,first]", extra header
// lines. Its state when it ends.
int dlPages(int tmpl, DlItem* d, char** f, int nf, int n, int count, const char* label) {
    const char* const urlTmpl = f[kFArg1];
    const char* const fileTmpl = f[kFPath];
    const char* const extra = nf > kFArg3 ? f[kFArg3] : NULL;
    const int pages = nf > kFArg2 ? toInt(f[kFArg2]) : 0;
    const char* comma = nf > kFArg2 ? findText(f[kFArg2], ",") : NULL;
    const int first = comma ? toInt(comma + 1) : 0;
    if (!urlTmpl[0] || !findText(urlTmpl, "{page") || !findText(fileTmpl, "{page") ||
        pages <= 0 || pages > SC_MAX_PAGES || first < 0) {
        logLine("download: %s: pages need {page} in the URL and the file, and 1 to %d pages", label,
                SC_MAX_PAGES);
        return kDlFailed;
    }
    static char url[1024], path[200], part[208];
    lock();
    d->pageCount = pages;
    d->pagesDone = 0;
    d->bytes = 0;
    unlock();
    int state = kDlDone, fetched = 0;
    const SceUInt64 began = sceKernelGetSystemTimeWide();
    for (int i = 0; i < pages && state == kDlDone; i++) {
        const int page = first + i;
        if (!pageExpand(urlTmpl, page, url, sizeof(url)) ||
            !pageExpand(fileTmpl, page, path, sizeof(path))) {
            logLine("download: %s: page %d too long a URL or file name", label, page);
            state = kDlFailed;
            break;
        }
        // The pages' folder, its last level, made with the first page.
        if (i == 0) {
            char dir[200];
            copyText(dir, path, sizeof(dir));
            int k = (int)sceClibStrnlen(dir, sizeof(dir));
            while (k > 0 && dir[k - 1] != '/') k--;
            if (k > 1) {
                dir[k - 1] = 0;
                sceIoMkdir(dir, 0777);
            }
        }
        uint64_t size = fileSize(path);
        if (size == 0) {
            sceClibSnprintf(part, sizeof(part), "%s.part", path);
            sceClibSnprintf(g_dlPageNote, sizeof(g_dlPageNote), "page %d/%d", i + 1, pages);
            char pageLabel[64];
            sceClibSnprintf(pageLabel, sizeof(pageLabel), "%s page %d", label, page);
            sceClibMemset(&g_dlPage, 0, sizeof(g_dlPage));
            g_dlQuiet = 1;
            state = dlFetch(tmpl, &g_dlPage, url, part, extra, f[kFTitle], n, count, pageLabel);
            g_dlQuiet = 0;
            if (state == kDlDone) {
                sceIoRemove(path);
                const int rc = sceIoRename(part, path);
                if (rc < 0) {
                    logLine("download: %s: page %d not moved in: 0x%08X", label, page, (unsigned)rc);
                    state = kDlFailed;
                }
                size = fileSize(path);
                fetched++;
            }
        }
        if (state == kDlDone) {
            lock();
            d->pagesDone = i + 1;
            d->bytes += size;
            unlock();
            dlStatus(true);
        }
    }
    g_dlPageNote[0] = 0;
    logLine("download: %s: %s, %d of %d pages, %d fetched now, %s MB, in %s s", label,
            kDlStateNames[state], d->pagesDone, pages, fetched, mb64Of(d->bytes).s,
            secsOf(msSince(began)).s);
    if (state == kDlDone) {
        lock();
        if (d->total == 0) d->total = d->bytes;
        unlock();
    }
    return state;
}

// One download of the list: its fields f. Its state when it ends.
int dlOne(int tmpl, int n, int count, char** f, int nf, const char* label) {
    DlItem* d = &g_dl[n];
    const char* const path = f[kFPath];
    const char* const title = f[kFTitle];
    const char* const kind = f[kFKind];
    if (same(kind, "pages")) {
        char sub[96];
        dlSub(sub, sizeof(sub), n, count, "Starting");
        notifyShow(title, sub, -1.0f);
        return dlPages(tmpl, d, f, nf, n, count, label);
    }
    if (d->total > 0 && fileSize(path) == d->total) {
        logLine("download: %s: whole already", label);
        return kDlDone;
    }
    if (same(kind, "url")) {
        if (!f[kFArg1][0]) {
            logLine("download: %s: no URL", label);
            return kDlFailed;
        }
        char sub[96];
        dlSub(sub, sizeof(sub), n, count, "Starting");
        notifyShow(title, sub, -1.0f);
        return dlFetch(tmpl, d, f[kFArg1], path, nf > kFArg2 ? f[kFArg2] : NULL, title, n, count,
                       label);
    }
    const char* server = nf > kFArg1 ? f[kFArg1] : "";
    if (same(kind, "plexadd") && nf > kFArg3) {
        if (!dlAdd(tmpl, d, server, f[kFArg2], f[kFArg3], label)) return kDlFailed;
    } else if (same(kind, "plexqueue") && nf > kFArg3) {
        lock();
        copyText(d->queue, f[kFArg2], sizeof(d->queue));
        copyText(d->item, f[kFArg3], sizeof(d->item));
        unlock();
    } else {
        logLine("download: %s: fetched in a way not known here (%s)", label, kind);
        return kDlFailed;
    }
    char sub[96];
    dlSub(sub, sizeof(sub), n, count, "Preparing on server");
    notifyShow(title, sub, -1.0f);
    const int prepared = dlPrepared(tmpl, d, server, title, n, count, label);
    if (prepared == kPrepStopped) return kDlGoing;
    if (prepared == kPrepGone) {
        lock();
        d->queue[0] = d->item[0] = 0;
        unlock();
        return kDlGone;
    }
    if (prepared != kPrepReady) return kDlFailed;
    // Its file, once prepared: not g_dlUrl, which dlAsk's callers fill.
    static char media[512];
    sceClibSnprintf(media, sizeof(media), "%s/downloadQueue/%s/item/%s/media", server, d->queue,
                    d->item);
    return dlFetch(tmpl, d, media, path, NULL, title, n, count, label);
}

// Waits up to ms while the app runs, as the update does (updateWaitGone):
// the file it writes is not touched while it might still write it. true once
// it has gone.
bool dlWaitGone(uint32_t ms) {
    for (uint32_t waited = 0;; waited += kDlLookMs) {
        SceUID pid = -1;
        if (sceAppMgrGetIdByName(&pid, g_dlFor.titleId) < 0) {
            logLine("download: %s gone, %s s after it handed them over", g_dlFor.name,
                    secsOf(waited).s);
            return true;
        }
        if (waited >= ms || dlStopped()) return false;
        sceKernelDelayThread(kDlLookMs * 1000);
    }
}

// The list carried on, once.
void dlRun() {
    if (!dlReadList()) {
        logLine("download: no whole list at %s/%s", g_dlFor.folder, SC_DOWNLOADS);
        dlStatus(false);
        return;
    }
    const int count = g_dlCount;
    logLine("download: %d download(s) handed over by %s%s", count, g_dlFor.name,
            g_dlHead.notify ? ", with their notification" : "");
    dlStatus(true);
    if (count == 0 || !dlWaitGone(kDlGoneMs)) {
        if (count > 0)
            logLine("download: %s; nothing carried on",
                    dlStopped() ? "taken back first" : "the app did not go");
        dlStatus(false);
        return;
    }
    int rcSsl = -1, rcHttp = -1, tmpl = -1;
    if (!g_netFound) g_netFound = findLibraries("download");
    if (g_netFound && httpStart("download", &rcSsl, &rcHttp)) tmpl = httpTemplate("download");
    if (tmpl < 0) {
        logLine("download: no way to ask the server here; nothing carried on");
        httpEnd(rcSsl, rcHttp);
        dlStatus(false);
        return;
    }
    // Shown at once, so that it is plain the downloads go on.
    char sub[96], heading[96];
    sceClibSnprintf(sub, sizeof(sub), "%d to go, in the background", count);
    sceClibSnprintf(heading, sizeof(heading), "%s downloads", g_dlFor.name);
    notifyShow(heading, sub, -1.0f);
    int done = 0, failed = 0, gone = 0, n = 0;
    const char* lastTitle = "";
    static char title[96];
    title[0] = 0;
    char* f[kFMax];
    bool stopped = false;
    if (dlOpenList()) {
        while (n < count && dlNext()) {
            if (!startsWith(g_dlLine, "item=")) continue;
            const int nf = dlFields(g_dlLine + 5, f, kFMax);
            if (nf <= kFArg1) continue;
            if (dlStopped()) {
                stopped = true;
                break;
            }
            char label[40];
            sceClibSnprintf(label, sizeof(label), "%d of %d (%s)", n + 1, count, f[kFKey]);
            lock();
            g_dl[n].state = kDlGoing;
            unlock();
            const int state = dlOne(tmpl, n, count, f, nf, label);
            lock();
            g_dl[n].state = state;
            if (state == kDlDone) g_dl[n].bytes = g_dl[n].total ? g_dl[n].total : g_dl[n].bytes;
            unlock();
            dlStatus(true);
            if (state == kDlDone) {
                done++;
                copyText(title, f[kFTitle], sizeof(title));
                lastTitle = title;
            } else if (state == kDlGone) {
                gone++;
            } else if (state == kDlFailed) {
                failed++;
            } else {
                stopped = true;
                break;
            }
            n++;
        }
        dlCloseList();
    }
    httpFn<FnId>(kHttpDeleteTemplate)(tmpl);
    httpEnd(rcSsl, rcHttp);
    dlStatus(false);
    logLine("download: %s: %d done, %d failed, %d gone from the server, %d not begun",
            stopped ? "stopped" : "all carried on", done, failed, gone, count - n);
    lock();
    const int paused = g_dlPaused;
    unlock();
    if (stopped && paused) {
        sceClibSnprintf(sub, sizeof(sub), "Resume them in %s", g_dlFor.name);
        notifyEnd("Downloads paused", sub);
    } else if (stopped) {
        // Left showing, for the app to go on with as it downloads again.
        notifyHold();
    } else if (failed + gone > 0) {
        sceClibSnprintf(sub, sizeof(sub), "%d downloaded, %d go on when %s opens", done,
                        failed + gone, g_dlFor.name);
        notifyEnd("Downloads stopped", sub);
    } else if (done == 1) {
        notifyEnd("Download complete", lastTitle);
    } else {
        sceClibSnprintf(sub, sizeof(sub), "%d items are now available offline", done);
        notifyEnd("Downloads complete", sub);
    }
}

// The download thread's: the app it runs for taken up, as it was now.
void dlTakeApp() {
    lock();
    const App* a = appAt(g_dlApp);
    if (a) g_dlFor = *a;
    unlock();
    sceClibMemset(&g_dlHead, 0, sizeof(g_dlHead));
    notifyFor(g_dlApp);
}

// The download thread: the list carried on, and again for each new one that
// comes meanwhile; then the next app's waiting.
int dlThread(SceSize, void*) {
    for (;;) {
        dlTakeApp();
        dlRun();
        sceClibMemset(&g_dlHead, 0, sizeof(g_dlHead));
        lock();
        if (g_dlAgain) {
            g_dlAgain = 0;
            g_dlStop = 0;
            unlock();
            continue;
        }
        if (g_dlPendingCount > 0) {
            g_dlApp = g_dlPending[0];
            for (int i = 1; i < g_dlPendingCount; i++) g_dlPending[i - 1] = g_dlPending[i];
            g_dlPendingCount--;
            g_dlStop = 0;
            g_dlPaused = 0;
            unlock();
            continue;
        }
        g_dlRunning = 0;
        g_dlApp = -1;
        unlock();
        break;
    }
    return sceKernelExitDeleteThread(0);
}

// From an app as it goes (SC_CMD_DL_START). Under g_lock.
void startDownloads(int app) {
    if (g_dlRunning && g_dlApp == app) {
        // The run under way stops at its next piece and takes the new list up.
        logLine("download: a new list from %s, taken up once the one under way stops",
                appName(app));
        g_dlAgain = 1;
        g_dlStop = 1;
        return;
    }
    if (g_dlRunning) {
        bool there = false;
        for (int i = 0; i < g_dlPendingCount; i++) there = there || g_dlPending[i] == app;
        if (!there && g_dlPendingCount < SC_MAX_APPS) g_dlPending[g_dlPendingCount++] = app;
        logLine("download: %s's list waits for %s's", appName(app), appName(g_dlApp));
        return;
    }
    g_dlStop = 0;
    g_dlAgain = 0;
    g_dlPaused = 0;
    g_dlApp = app;
    const SceUID thread = sceKernelCreateThread("ShellCompanion::Download", dlThread,
                                                kThreadPriority, kDlStack, 0, 0, NULL);
    const int rc = thread < 0 ? thread : sceKernelStartThread(thread, 0, NULL);
    if (rc < 0) {
        if (thread >= 0) sceKernelDeleteThread(thread);
        logLine("download: no thread to carry them on: 0x%08X", (unsigned)rc);
        g_dlApp = -1;
        return;
    }
    g_dlRunning = 1;
}

// From an app as it opens (SC_CMD_DL_TAKE_BACK). Under g_lock. A list still
// waiting is dropped, and its status says nothing runs, so that the app,
// which waits for running=0, goes on at once.
void takeDownloads(int app) {
    for (int i = 0; i < g_dlPendingCount; i++) {
        if (g_dlPending[i] != app) continue;
        for (int k = i + 1; k < g_dlPendingCount; k++) g_dlPending[k - 1] = g_dlPending[k];
        g_dlPendingCount--;
        char path[160];
        appPath(appAt(app), SC_DOWNLOADS_STATUS, path, sizeof(path));
        static const char kIdle[] = "running=0\npaused=0\nend\n";
        writeWhole(path, kIdle, (int)sizeof(kIdle) - 1);
        logLine("download: %s takes back its list, not yet begun", appName(app));
        return;
    }
    if (!g_dlRunning || g_dlApp != app) {
        logLine("download: %s takes the downloads back; none carried on here", appName(app));
        return;
    }
    logLine("download: %s takes the downloads back", appName(app));
    g_dlStop = 1;
    g_dlAgain = 0;
}

// ── Threads ─────────────────────────────────────────────────────────────────
// The note of an open begun, as the quick menu is seen open. Under g_lock.
void noteOpened() {
    sceClibMemset(&g_note, 0, sizeof(g_note));
    g_note.foundMs = g_note.takenMs = g_note.firstPutMs = -1;
    g_note.logIt = g_notesLeft > 0;
    if (g_note.logIt) g_notesLeft--;
}

// The note of the open just closed, logged if it is one to log: whether and
// when the row was taken, what was put back after, and the lines 1 s in.
// Under g_lock.
void noteClosed(SceUInt64 now) {
    if (!g_note.logIt) return;
    g_note.logIt = 0;
    char taken[64];
    if (g_note.takenMs >= 0)
        sceClibSnprintf(taken, sizeof(taken), "taken at %d ms", g_note.takenMs);
    else if (!g_active)
        sceClibSnprintf(taken, sizeof(taken), "nobody's");
    else if (g_note.foundMs >= 0)
        sceClibSnprintf(taken, sizeof(taken), "not taken, its buttons found at %d ms",
                        g_note.foundMs);
    else
        sceClibSnprintf(taken, sizeof(taken), "not taken, no buttons found");
    logLine("quick menu: open %d ms, the row %s, %s%s; then buttons %d, lines %d, lines shown "
            "%d, play icon %d, cover %d, lit %d, the first at %d ms",
            msOpen(now),
            g_note.takenMs >= 0 ? ownerName(g_note.owner, g_note.ownerApp)
                                : ownerName(g_owner, g_ownerApp),
            taken, g_note.takenMs >= 0 ? " for it" : "", g_note.buttons, g_note.lines,
            g_note.shown, g_note.playIcon, g_note.cover, g_note.lit, g_note.firstPutMs);
    if (g_note.snapped) logLine("quick menu: 1 s in: %s", g_note.snap);
}

// Watches the quick menu open and close, as ElevenMPV-A's plugin does, gives
// the row back when its owner has gone quiet, and writes the log.
class WatchThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        const SceUInt64 started = sceKernelGetSystemTimeWide();
        SceUInt64 lastLoop = started;
        int wasOpen = 0;
        while (!IsCanceled()) {
            SceAppMgrAppState st;
            sceClibMemset(&st, 0, sizeof(st));
            _sceAppMgrGetAppState(&st, sizeof(st), kAppStateVersion);
            const int open = st.isSystemUiOverlaid != 0;
            const SceUInt64 now = sceKernelGetSystemTimeWide();
            lock();
            // The system clock runs on while the Vita sleeps, and every
            // process sleeps with it (player.c logs "did not run for"): the
            // row's owner gets its time again, rather than being taken for
            // gone on the first look after. Writing the log can hold this
            // thread up too: 3.7 s once, as the quick menu's first opening
            // was logged while the player read every app's details (log
            // c1930c5a).
            if (now - lastLoop > 2000000ULL) {
                uint32_t secs, micros;
                splitUs(now - lastLoop, &secs, &micros);
                logLine("the plugin's watch did not run for %u.%u s (the Vita asleep, or the log "
                        "slow to write)",
                        (unsigned)secs, (unsigned)(micros / 100000));
                if (g_active) g_activatedUs = now;
            }
            lastLoop = now;
            if (open && !wasOpen) {
                g_openedUs = now;
                g_openSerial++;
                noteOpened();
            } else if (!open && wasOpen) {
                noteClosed(now);
            }
            g_open = open;
            if (g_active && g_owner != kOwnerShell &&
                now - g_activatedUs > SC_ROW_TIMEOUT_S * 1000000ULL) {
                // Its owner gone without giving the row back.
                g_active = 0;
                g_releaseDue = 1;
                logLine("nothing from %s for %d s: row given back", ownerName(g_owner, g_ownerApp),
                        SC_ROW_TIMEOUT_S);
            }
            // SceShell's UI thread has never called mainTick: the row is
            // ticked from here instead, as before build 2026.
            if (!g_fallback && !g_mainTicks && now - started > 3000000ULL) {
                g_fallback = 1;
                logLine("SceShell's UI thread has not called the plugin in 3 s: the row is "
                        "ticked from the plugin's own thread");
            }
            if (g_fallback) rowTick(now);
            unlock();
            wasOpen = open;
            notifyHeldTick(now);
            notifyPauseTick();
            logDrain();
            paf::thread::Sleep(100);
        }
    }
};

ScMessage g_message;  // the receiving thread's alone

// UTF-16 to UTF-8, into out, cut to fit (never in the middle of a
// character); a lone surrogate becomes U+FFFD.
void toUtf8(const uint16_t* s, int len, char* out, int cap) {
    int n = 0;
    for (int i = 0; i < len && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        char b[4];
        int k;
        if (c < 0x80) {
            b[0] = (char)c;
            k = 1;
        } else if (c < 0x800) {
            b[0] = (char)(0xC0 | (c >> 6));
            b[1] = (char)(0x80 | (c & 0x3F));
            k = 2;
        } else if (c < 0x10000) {
            b[0] = (char)(0xE0 | (c >> 12));
            b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[2] = (char)(0x80 | (c & 0x3F));
            k = 3;
        } else {
            b[0] = (char)(0xF0 | (c >> 18));
            b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            b[2] = (char)(0x80 | ((c >> 6) & 0x3F));
            b[3] = (char)(0x80 | (c & 0x3F));
            k = 4;
        }
        if (n + k > cap - 1) break;
        for (int x = 0; x < k; x++) out[n++] = b[x];
    }
    out[n] = 0;
}

// The app a message is from, by its title id: its index, or -1 if it has
// not said SC_CMD_HELLO this boot. Under g_lock.
int appOf(const ScMessage& m) {
    char title[12];
    copyText(title, m.titleId, sizeof(title));
    for (int i = 0; i < g_appCount; i++)
        if (same(g_apps[i].titleId, title)) return i;
    return -1;
}

// The features this plugin has, and what else an app may look for in
// SC_PLUGIN_INFO's caps= before it counts on it.
const uint32_t kFeatures = SC_FEAT_ROW | SC_FEAT_MUSIC | SC_FEAT_DOWNLOADS | SC_FEAT_UPDATE |
                           SC_FEAT_SENDSPIN;
const char* const kCaps =
    "queue.seek,queue.control,downloads.battery,sendspin.metadata,sendspin.controller,probe";
// Under g_lock: the app a HELLO is to be answered for, -1 for none: answered
// by the receiving thread once it has let the lock go (helloReply).
int g_helloReplyApp = -1;

// The answer to a HELLO: SC_PLUGIN_INFO in the app's folder, then
// SC_REPLY_HELLO on the pipe for it. The receiving thread's, under nothing.
void helloReply() {
    lock();
    const int i = g_helloReplyApp;
    g_helloReplyApp = -1;
    App a;
    if (i >= 0) a = g_apps[i];
    const int apps = g_appCount;
    unlock();
    if (i < 0) return;
    static char text[512], path[160];
    uint32_t secs, micros;
    splitUs(sceKernelGetSystemTimeWide(), &secs, &micros);
    const int n = sceClibSnprintf(
        text, sizeof(text),
        "written=%u.%06u\nprotocol=%d\nversion=%s\nversionCode=%d\nfeatures=0x%X\ngranted=0x%X\n"
        "caps=%s\napps=%d\nend\n",
        (unsigned)secs, (unsigned)micros, SC_PROTOCOL, SC_PLUGIN_VERSION, SC_PLUGIN_VERSION_CODE,
        (unsigned)kFeatures, (unsigned)(a.features & kFeatures), kCaps, apps);
    appPath(&a, SC_PLUGIN_INFO, path, sizeof(path));
    const int rcFile = writeWhole(path, text, n);
    ScPress msg;
    sceClibMemset(&msg, 0, sizeof(msg));
    msg.press = SC_REPLY_HELLO;
    sceClibMemcpy(msg.titleId, a.titleId, sizeof(msg.titleId) - 1);
    const int rcPipe = sceKernelTrySendMsgPipe(g_fromPlugin, &msg, sizeof(msg), kPipeModeFull, NULL);
    if (rcFile < 0 || rcPipe < 0)
        logLine("hello: %s answered: info 0x%08X, reply 0x%08X", a.name, (unsigned)rcFile,
                (unsigned)rcPipe);
    if (a.features & ~kFeatures)
        logLine("hello: %s asked for features 0x%X this plugin does not have", a.name,
                (unsigned)(a.features & ~kFeatures));
}

// An app registering itself, or again. Under g_lock.
void hello(const ScMessage& m) {
    char title[12], folder[96], name[48];
    copyText(title, m.titleId, sizeof(title));
    toUtf8(m.text1, 256, folder, sizeof(folder));
    toUtf8(m.text2, 256, name, sizeof(name));
    int len = (int)sceClibStrnlen(folder, sizeof(folder));
    while (len > 0 && folder[len - 1] == '/') folder[--len] = 0;
    // A folder on a device, in full, with no way out of it.
    bool ok = title[0] && len > 0;
    bool colon = false;
    for (int i = 0; i < len && ok; i++) {
        colon = colon || folder[i] == ':';
        ok = !(folder[i] == '.' && folder[i + 1] == '.');
    }
    if (!ok || !colon) {
        logLine("hello: refused: title \"%s\", folder \"%s\"", title, folder);
        return;
    }
    int i = appOf(m);
    const bool fresh = i < 0;
    if (fresh) {
        if (g_appCount == SC_MAX_APPS) {
            logLine("hello: %s refused: %d apps are registered already", title, SC_MAX_APPS);
            return;
        }
        i = g_appCount++;
    }
    App* a = &g_apps[i];
    copyText(a->titleId, title, sizeof(a->titleId));
    copyText(a->folder, folder, sizeof(a->folder));
    copyText(a->name, name[0] ? name : title, sizeof(a->name));
    a->features = m.arg;
    sceIoMkdir(folder, 0777);
    g_helloReplyApp = i;
    logLine("hello: %s (%s) %s, folder %s, features%s%s%s%s%s", a->name, a->titleId,
            fresh ? "registered" : "again", a->folder,
            (a->features & SC_FEAT_ROW) ? " row" : "", (a->features & SC_FEAT_MUSIC) ? " music" : "",
            (a->features & SC_FEAT_DOWNLOADS) ? " downloads" : "",
            (a->features & SC_FEAT_UPDATE) ? " update" : "",
            (a->features & SC_FEAT_SENDSPIN) ? " sendspin" : "");
}

// The feature a command needs, 0 for none.
uint32_t featureOf(uint32_t cmd) {
    switch (cmd) {
    case SC_CMD_ROW_ACTIVATE:
    case SC_CMD_ROW_RELEASE:
    case SC_CMD_ROW_SONG:
    case SC_CMD_ROW_PLAYING:
    case SC_CMD_ROW_ART:
        return SC_FEAT_ROW;
    case SC_CMD_MUSIC_PLAY:
    case SC_CMD_MUSIC_TAKE_BACK:
    case SC_CMD_PROBE_STREAM:
        return SC_FEAT_MUSIC;
    case SC_CMD_DL_START:
    case SC_CMD_DL_TAKE_BACK:
    case SC_CMD_DL_PROGRESS:
    case SC_CMD_DL_PROGRESS_END:
        return SC_FEAT_DOWNLOADS;
    case SC_CMD_UPDATE_INSTALL:
        return SC_FEAT_UPDATE;
    case SC_CMD_SENDSPIN_START:
    case SC_CMD_SENDSPIN_TAKE_BACK:
        return SC_FEAT_SENDSPIN;
    default:
        return 0;
    }
}

// Takes what the apps send; the row's tick shows it.
class ReceiveThread : public paf::thread::Thread {
public:
    using paf::thread::Thread::Thread;

    void EntryFunction() override {
        while (!IsCanceled()) {
            const int rc = sceKernelReceiveMsgPipe(g_toPlugin, &g_message, sizeof(g_message),
                                                   kPipeModeFull, NULL, NULL);
            if (rc < 0) {
                logLine("receive: 0x%08X", (unsigned)rc);
                paf::thread::Sleep(1000);
                continue;
            }
            lock();
            handle(g_message);
            unlock();
            helloReply();
            notifyAsked();
        }
    }

private:
    // Commands an app sends again and again, logged only the first few times
    // they are refused.
    static int s_refusals;

    static void handle(const ScMessage& m) {
        if (m.cmd == SC_CMD_HELLO) {
            hello(m);
            return;
        }
        const int app = appOf(m);
        const uint32_t need = featureOf(m.cmd);
        if (app < 0 || (need && !(g_apps[app].features & need))) {
            if (s_refusals < 20) {
                s_refusals++;
                char title[12];
                copyText(title, m.titleId, sizeof(title));
                logLine("command %u from \"%s\" refused: %s", (unsigned)m.cmd, title,
                        app < 0 ? "it has not said hello" : "it did not ask for that feature");
            }
            return;
        }
        const bool owns = g_active && g_owner == kOwnerApp && g_ownerApp == app;
        switch (m.cmd) {
        case SC_CMD_ROW_ACTIVATE:
            if (!owns) {
                logLine("row wanted by %s", appName(app));
                notesWanted();
            }
            // An app with the row has the music: SceShell leaves it to it,
            // and does not begin a queue handed over before. The app plays
            // its own, which SceShell's would play over, so that one stops.
            stopCarry("an app takes the row", kEndStopped);
            ssStop("an app takes the row");
            g_playQueueDue = 0;
            if (g_owner != kOwnerApp || g_ownerApp != app) g_artSerial = 0;  // the last owner's cover
            g_active = 1;
            g_owner = kOwnerApp;
            g_ownerApp = app;
            sceClibMemcpy(g_pressTitle, g_apps[app].titleId, sizeof(g_pressTitle));
            g_activatedUs = sceKernelGetSystemTimeWide();
            break;
        case SC_CMD_ROW_RELEASE:
            // Its queue SceShell carries on stops, the place written and the
            // music left, as the app gives the row back.
            if (g_carryApp == app) stopCarry("the row given back", kEndRowBack);
            if (!owns) break;  // another's row is not this app's to give back
            logLine("row given back by %s", appName(app));
            notesWanted();
            g_active = 0;
            g_releaseDue = 1;
            g_artSerial = 0;
            break;
        case SC_CMD_ROW_SONG:
            if (!owns) break;
            sceClibMemcpy(g_title, m.text1, sizeof(g_title));
            sceClibMemcpy(g_artist, m.text2, sizeof(g_artist));
            sceClibMemcpy(g_album, m.text3, sizeof(g_album));
            g_title[255] = g_artist[255] = g_album[255] = 0;
            break;
        case SC_CMD_ROW_PLAYING:
            if (owns) g_playing = (int)m.arg;
            break;
        case SC_CMD_ROW_ART:
            if (!owns) break;
            g_artSerial = m.arg;
            if (m.arg) {
                char name[24];
                sceClibSnprintf(name, sizeof(name), SC_ART_FMT, (unsigned)(m.arg & 1));
                appPath(&g_apps[app], name, g_artPath, sizeof(g_artPath));
            }
            break;
        case SC_CMD_MUSIC_PLAY:
            // A queue SceShell carries on yields to the new one, its music
            // left for the new one to stop, and nothing written.
            logLine("play queue: %s hands its queue to SceShell%s", appName(app),
                    g_carry ? "; the queue carried on stops first" : "");
            // The row is the shell's for it: an app giving its queue over
            // has given the row back first, or it goes now.
            if (owns) {
                g_active = 0;
                g_releaseDue = 1;
                g_artSerial = 0;
            }
            stopCarry("an app hands a queue over", kEndYield);
            probeStop("a queue handed over");
            ssStop("a queue handed over");
            if (startMusicThread()) {
                g_playQueueDue = 1;
                g_playQueueApp = app;
                sceKernelSignalSema(g_musicSema, 1);
            } else {
                logLine("play queue: no music thread, so nothing plays");
            }
            break;
        case SC_CMD_MUSIC_TAKE_BACK:
            logLine("take back: %s takes the music back%s", appName(app),
                    g_carry && g_carryApp == app ? "" : "; none of its queue carried on here");
            // A queue it handed over and not yet begun is not begun.
            if (g_playQueueDue && g_playQueueApp == app) g_playQueueDue = 0;
            if (g_carryApp == app) stopCarry("the app takes the music back", kEndTakeBack);
            break;
        case SC_CMD_UPDATE_INSTALL:
            startUpdate(app);
            break;
        case SC_CMD_SENDSPIN_START:
            logLine("sendspin: %s hands its player over", appName(app));
            ssAsk(app);
            break;
        case SC_CMD_SENDSPIN_TAKE_BACK:
            logLine("sendspin: %s takes its player back%s", appName(app),
                    g_ssRunning || g_ssWant >= 0 ? "" : "; the plugin is not it");
            ssStop("the app takes it back", 2);
            break;
        case SC_CMD_PROBE_STREAM: {
            char source[160];
            toUtf8(m.text1, 256, source, sizeof(source));
            logLine("probe: %s asks for mode %u%s", appName(app), (unsigned)m.arg,
                    source[0] ? ", from a WAV" : "");
            probeAsk(app, m.arg, source);
            break;
        }
        case SC_CMD_DL_START:
            startDownloads(app);
            break;
        case SC_CMD_DL_TAKE_BACK:
            takeDownloads(app);
            break;
        case SC_CMD_DL_PROGRESS:
        case SC_CMD_DL_PROGRESS_END:
            g_notifyAsk.due = m.cmd == SC_CMD_DL_PROGRESS ? 1 : 2;
            g_notifyAsk.app = app;
            g_notifyAsk.permille = m.arg;
            sceClibMemcpy(g_notifyAsk.text, m.text1, sizeof(g_notifyAsk.text) - 2);
            sceClibMemcpy(g_notifyAsk.sub, m.text2, sizeof(g_notifyAsk.sub) - 2);
            g_notifyAsk.text[63] = g_notifyAsk.sub[63] = 0;
            break;
        default:
            logLine("unknown command %u from %s", (unsigned)m.cmd, appName(app));
            break;
        }
    }
};
int ReceiveThread::s_refusals;

// ── SceShell's view of the BGM port ─────────────────────────────────────────
// As ElevenMPV-A's plugin does: SceShell reads the port's priority as no more
// than 0x80 (sceAppMgrGetCurrentBgmState2, SceAppMgrUser 0x62BEBD65), what an
// app gets while SceShell decodes for it. VitaPlex's own player takes 0x81.
typedef struct BgmState {
    int ownerId;
    int priority;
    int status1;
    int state;
    int status2;
} BgmState;

TaiHookRef g_bgmHookRef;

int bgmStateCapped(BgmState* state) {
    const TaiHookUser* cur = (const TaiHookUser*)g_bgmHookRef;
    const TaiHookUser* next = (const TaiHookUser*)cur->next;
    typedef int (*Fn)(BgmState*);
    const int ret = next ? ((Fn)next->func)(state) : ((Fn)cur->old)(state);
    if (state->priority > 0x80) state->priority = 0x80;
    return ret;
}

// ── The download notification's owner ──────────────────────────────────────
// A notification shows its owner's icon, and SceShell finds the owner of one
// begun through SceNotificationUtil by asking sceAppMgrGetStatusByAppId
// (SceAppMgrUser 0x47958DE5) about the app that called (0x8123ED38 on 3.60,
// read from SceShell's code, build 2072): the title id at 0xC of the status,
// used when the byte at 0x70 is 0, else NPXS10998 or NPXS10999 for 1 and 2.
// The plugin's caller is SceShell, no app, and its notification had no icon.
// So for the one lookup after the plugin begins its notification, the answer
// is the app it is shown for: its title, and its icon. Only an answer that names no app or a
// system title (NPXS...) is changed, so that a game's own is never taken, and
// only once, within kOwnerWindowUs of the Begin. Build 2074 changed only
// answers naming no app, and changed none: the lookup named one, which the
// log did not say; every lookup in the window is logged now.
const uint32_t kStatusTitleId = 0xC, kStatusKind = 0x70, kStatusSize = 0x74;

int statusAsOwner(int appId, uint8_t* status) {
    const TaiHookUser* cur = (const TaiHookUser*)g_statusHookRef;
    const TaiHookUser* next = (const TaiHookUser*)cur->next;
    typedef int (*Fn)(int, uint8_t*);
    const int ret = next ? ((Fn)next->func)(appId, status) : ((Fn)cur->old)(appId, status);
    const SceUInt64 until = __atomic_load_n(&g_ownerUntilUs, __ATOMIC_SEQ_CST);
    if (!until || !status || sceKernelGetSystemTimeWide() > until) return ret;
    char title[16];
    title[0] = 0;
    if (ret >= 0) copyText(title, (const char*)status + kStatusTitleId, 10);
    const bool system = startsWith(title, "NPXS");
    logLine("notification: owner lookup for app 0x%X gave 0x%08X, kind %d, title \"%s\"%s",
            (unsigned)appId, (unsigned)ret, ret >= 0 ? (int)status[kStatusKind] : -1, title,
            ret >= 0 && status[kStatusKind] == 0 && !system ? ", an app's: left"
                                                            : "; answered as the notification's app");
    if (ret >= 0 && status[kStatusKind] == 0 && !system) return ret;  // an app: its own
    __atomic_store_n(&g_ownerUntilUs, 0, __ATOMIC_SEQ_CST);
    char owner[12];
    copyText(owner, g_notifyTitle, sizeof(owner));
    if (!owner[0]) return ret;
    if (ret < 0) sceClibMemset(status, 0, kStatusSize);
    status[kStatusKind] = 0;
    copyText((char*)status + kStatusTitleId, owner, 10);
    return 0;
}

}  // namespace

extern "C" int module_start(SceSize args, const void* argp) {
    (void)args;
    (void)argp;
    logOpen();
    logLine("start: vita-shell-companion %s, protocol %d, pipes %s", SC_PLUGIN_VERSION, SC_PROTOCOL,
            SC_PIPE_TO_PLUGIN);

    // The quick menu's root, as ElevenMPV-A's plugin finds it: power_manage_
    // plugin's interface has a function giving it at 0x54.
    paf::Plugin* power = paf::Plugin::Find("power_manage_plugin");
    void* iface = power ? power->GetInterface(1) : NULL;
    typedef paf::ui::Widget* (*GetImposeRoot)();
    GetImposeRoot getRoot = iface ? *(GetImposeRoot*)((char*)iface + 0x54) : NULL;
    g_impose = getRoot ? getRoot() : NULL;
    if (!g_impose) {
        logLine("the quick menu not found (power_manage_plugin %p, interface %p): not staying",
                (void*)power, iface);
        logDrain();
        return SCE_KERNEL_START_NO_RESIDENT;
    }
    g_surfacePool = *(paf::graph::SurfacePool* const*)((const char*)power + kPluginSurfacePool);

    g_lock = sceKernelCreateMutex("ShellCompanion::Lock", 0, 0, NULL);
    g_netLock = sceKernelCreateMutex("ShellCompanion::Net", 0, 0, NULL);
    g_netHeadersLock = sceKernelCreateMutex("ShellCompanion::Headers", 0, 0, NULL);
    g_notifyLock = sceKernelCreateMutex("ShellCompanion::Notify", 0, 0, NULL);
    // As ElevenMPV-A's pipes are made: 0x40, user main memory; 0x81, whole
    // messages and openable by name.
    g_toPlugin = sceKernelCreateMsgPipe(SC_PIPE_TO_PLUGIN, 0x40, 0x81,
                                        sizeof(ScMessage) * SC_TO_PLUGIN_SLOTS, NULL);
    g_fromPlugin = sceKernelCreateMsgPipe(SC_PIPE_FROM_PLUGIN, 0x40, 0x81,
                                          sizeof(ScPress) * SC_FROM_PLUGIN_SLOTS, NULL);
    if (g_lock < 0 || g_netLock < 0 || g_netHeadersLock < 0 || g_notifyLock < 0 ||
        g_toPlugin < 0 || g_fromPlugin < 0) {
        logLine("pipes: 0x%08X, 0x%08X, lock 0x%08X: not staying", (unsigned)g_toPlugin,
                (unsigned)g_fromPlugin, (unsigned)g_lock);
        logDrain();
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    TaiHookArgs hook;
    hook.size = sizeof(hook);
    hook.module = "SceShell";
    hook.library_nid = 0xA6605D6F;
    hook.func_nid = 0x62BEBD65;
    hook.hook_func = (const void*)bgmStateCapped;
    const SceUID hooked = scHookFunctionImport(&g_bgmHookRef, &hook);
    if (hooked < 0) logLine("BGM priority SceShell reads not capped: 0x%08X", (unsigned)hooked);
    hook.func_nid = 0x47958DE5;  // sceAppMgrGetStatusByAppId, in the same library
    hook.hook_func = (const void*)statusAsOwner;
    const SceUID hookedStatus = scHookFunctionImport(&g_statusHookRef, &hook);
    if (hookedStatus < 0)
        logLine("the download notification's owner not hooked: 0x%08X", (unsigned)hookedStatus);

    // PAF's list of what its main (UI) thread calls each time round its loop;
    // ElevenMPV-A's app imports Register, so its NID is the console's.
    paf::common::MainThreadCallList::Register(mainTick, NULL);

    (new WatchThread(kThreadPriority, 0x4000, "ShellCompanion::Watch"))->Start();
    // Twice the watch thread's: it shows an app's download notification
    // too (notifyAsked), through SceShell's service, as the download thread does.
    (new ReceiveThread(kThreadPriority, 0x8000, "ShellCompanion::Receive"))->Start();
    logLine("ready");
    return SCE_KERNEL_START_SUCCESS;
}

// It stays: its threads and SceShell's widgets hold on to it.
extern "C" int module_stop(SceSize args, const void* argp) {
    (void)args;
    (void)argp;
    return SCE_KERNEL_STOP_CANCEL;
}
