/**
 * Inline hooks for the music watch (mcw.c): a function's first instructions
 * are replaced with a jump to a thunk that calls mcwPre, then the function
 * itself (its first instructions, moved, then the rest where it is), then
 * mcwPost, and returns what the function returned. The thunk keeps every
 * argument register (rdi, rsi, rdx, rcx, r8, r9, rax, xmm0-7) and copies 8
 * of the caller's stack arguments, so a function of up to 14 integer
 * arguments sees what it was called with; rax, rdx and xmm0 are returned
 * as the function left them.
 */

#ifndef SC_PS4_HOOK_H
#define SC_PS4_HOOK_H

#include <stddef.h>
#include <stdint.h>

#define HOOK_PRE_BYTES 512

typedef struct Hook Hook;

// One call, on the caller's stack, as the thunk (hook.c) lays it out.
typedef struct {
    uint64_t gpr[6];        // 0x000: rdi, rsi, rdx, rcx, r8, r9
    uint64_t rax;           // 0x030
    Hook* hook;             // 0x038
    uint8_t xmm[8][16];     // 0x040
    uint64_t retRax;        // 0x0C0
    uint64_t retRdx;        // 0x0C8
    uint8_t retXmm0[16];    // 0x0D0
    uint64_t* callerStack;  // 0x0E0: the caller's stack arguments
    uint64_t t0;            // 0x0E8: mcwPre's
    uint32_t seq;           // 0x0F0
    uint32_t flags;         // 0x0F4
    uint64_t pad;           // 0x0F8
    int32_t preLen[6];      // 0x100: bytes in pre[i], 0 for no pointer
    uint8_t pre[6][HOOK_PRE_BYTES];  // 0x118
} Frame;

#define HOOK_FRAME_SIZE 0xD20  // sizeof(Frame), rounded to 16

struct Hook {
    void* stub;          // 0x00: the moved instructions, then a jump back (the thunk's)
    const char* name;    // 0x08
    void* target;        // the function
    uint32_t calls;      // calls seen
    uint32_t same;       // calls not logged since the last, their answer unchanged
    uint64_t lastHash;   // the last logged answer's
    uint64_t lastLogUs;
    int quiet;           // a getter: logged only when its answer changes
    uint32_t logged;     // calls logged
    int installed;
    int json;            // a managed method's (mcw.c, monoHooks): 1 a JSON key, 2 forced true, 3 a float logged
    uint8_t original[16];  // its first bytes before the hook
    uint32_t stolen;       // how many were moved
};

// Called by the thunk, on the caller's thread (mcw.c).
void mcwPre(Frame* f);
void mcwPost(Frame* f);

// Hooks target: 0, or < 0 with why in *why. Not for two threads at once.
int hookInstall(Hook* h, void* target, const char** why);

// Whether [p, p + n) can be read: how many bytes of it can be, from p.
size_t hookReadable(const void* p, size_t n);
// The protection of the memory at p (1 read, 2 write, 4 execute), or -1.
int hookProt(const void* p);

// How hookInstall got its memory and wrote the code: for the log.
extern const char* g_hookHow;

#endif
