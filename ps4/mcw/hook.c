/**
 * Inline hooks (hook.h). A hooked function starts with a 5-byte jump
 * (jmp rel32) to its entry, in memory of ours within 2 GB of it; the entry
 * loads the Hook into r11 and jumps to the thunk. The instructions the jump
 * replaced are moved to the Hook's stub, their relative addresses (jumps,
 * calls, rip-relative operands) recomputed for where they now are, then a
 * jump back to the rest of the function. With no memory of ours that near,
 * the function starts with a 14-byte absolute jump instead, and the moved
 * instructions must have no relative address.
 *
 * Memory for entries and stubs, the first that works: an anonymous mapping
 * read/write/execute, as GoldHEN's plugin SDK maps its stubs (Detour.c);
 * one read/write, made read/execute once written; a reserved part of this
 * module's own code. Code is written to a function, and to that reserved
 * part, after sceKernelMprotect, as the SDK patches, or else through
 * GoldHEN's PROCESS_RW (system call 500, command 5).
 */

#include "hook.h"

#include "../probe/sys.h"
#include "hde64.h"

int sceKernelMprotect(const void* addr, size_t len, int prot);
int sceKernelMmap(void* addr, size_t len, int prot, int flags, int fd, int64_t off, void** out);
int sceKernelMunmap(void* addr, size_t len);
int sceKernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);

_Static_assert(sizeof(Frame) <= HOOK_FRAME_SIZE && HOOK_FRAME_SIZE % 16 == 0, "the thunk's frame");
_Static_assert(__builtin_offsetof(Frame, retRax) == 0xC0, "Frame.retRax");
_Static_assert(__builtin_offsetof(Frame, callerStack) == 0xE0, "Frame.callerStack");
_Static_assert(__builtin_offsetof(Frame, r10) == 0xF8, "Frame.r10");
_Static_assert(__builtin_offsetof(Hook, stub) == 0, "Hook.stub");

const char* g_hookHow = "nothing yet";

// The thunk. At its start: r11 the Hook, [rsp] the return address, the
// caller's stack arguments above it. Frame at rsp, 16-byte aligned for the
// calls.
void mcwThunk(void);
__asm__(".text\n"
        ".balign 16\n"
        ".type mcwThunk, @function\n"
        "mcwThunk:\n"
        ".cfi_startproc\n"
        "  pushq %rbp\n"
        ".cfi_def_cfa_offset 16\n"
        ".cfi_offset %rbp, -16\n"
        "  movq %rsp, %rbp\n"
        ".cfi_def_cfa_register %rbp\n"
        "  subq $0xD20, %rsp\n"  // HOOK_FRAME_SIZE
        "  movq %rdi, 0x00(%rsp)\n"
        "  movq %rsi, 0x08(%rsp)\n"
        "  movq %rdx, 0x10(%rsp)\n"
        "  movq %rcx, 0x18(%rsp)\n"
        "  movq %r8, 0x20(%rsp)\n"
        "  movq %r9, 0x28(%rsp)\n"
        "  movq %rax, 0x30(%rsp)\n"
        "  movq %r11, 0x38(%rsp)\n"
        "  movdqu %xmm0, 0x40(%rsp)\n"
        "  movdqu %xmm1, 0x50(%rsp)\n"
        "  movdqu %xmm2, 0x60(%rsp)\n"
        "  movdqu %xmm3, 0x70(%rsp)\n"
        "  movdqu %xmm4, 0x80(%rsp)\n"
        "  movdqu %xmm5, 0x90(%rsp)\n"
        "  movdqu %xmm6, 0xA0(%rsp)\n"
        "  movdqu %xmm7, 0xB0(%rsp)\n"
        // r10 kept (Frame.r10) for the function: Mono passes its IMT/RGCTX
        // argument there. (r11 is the entry's, the Hook.)
        "  movq %r10, 0xF8(%rsp)\n"
        "  leaq 16(%rbp), %r10\n"
        "  movq %r10, 0xE0(%rsp)\n"
        "  movq %rsp, %rdi\n"
        "  callq mcwPre\n"
        // The caller's first 8 stack arguments, copied below the frame.
        "  subq $0x40, %rsp\n"
        "  movq 16(%rbp), %rax\n  movq %rax, 0x00(%rsp)\n"
        "  movq 24(%rbp), %rax\n  movq %rax, 0x08(%rsp)\n"
        "  movq 32(%rbp), %rax\n  movq %rax, 0x10(%rsp)\n"
        "  movq 40(%rbp), %rax\n  movq %rax, 0x18(%rsp)\n"
        "  movq 48(%rbp), %rax\n  movq %rax, 0x20(%rsp)\n"
        "  movq 56(%rbp), %rax\n  movq %rax, 0x28(%rsp)\n"
        "  movq 64(%rbp), %rax\n  movq %rax, 0x30(%rsp)\n"
        "  movq 72(%rbp), %rax\n  movq %rax, 0x38(%rsp)\n"
        "  movq 0x40(%rsp), %rdi\n"
        "  movq 0x48(%rsp), %rsi\n"
        "  movq 0x50(%rsp), %rdx\n"
        "  movq 0x58(%rsp), %rcx\n"
        "  movq 0x60(%rsp), %r8\n"
        "  movq 0x68(%rsp), %r9\n"
        "  movdqu 0x80(%rsp), %xmm0\n"
        "  movdqu 0x90(%rsp), %xmm1\n"
        "  movdqu 0xA0(%rsp), %xmm2\n"
        "  movdqu 0xB0(%rsp), %xmm3\n"
        "  movdqu 0xC0(%rsp), %xmm4\n"
        "  movdqu 0xD0(%rsp), %xmm5\n"
        "  movdqu 0xE0(%rsp), %xmm6\n"
        "  movdqu 0xF0(%rsp), %xmm7\n"
        "  movq 0x78(%rsp), %r11\n"
        "  movq 0x138(%rsp), %r10\n"
        "  movq 0x70(%rsp), %rax\n"
        "  callq *(%r11)\n"  // Hook.stub: the function
        "  addq $0x40, %rsp\n"
        "  movq %rax, 0xC0(%rsp)\n"
        "  movq %rdx, 0xC8(%rsp)\n"
        "  movdqu %xmm0, 0xD0(%rsp)\n"
        "  movq %rsp, %rdi\n"
        "  callq mcwPost\n"
        "  movq 0xC0(%rsp), %rax\n"
        "  movq 0xC8(%rsp), %rdx\n"
        "  movdqu 0xD0(%rsp), %xmm0\n"
        "  leave\n"
        ".cfi_def_cfa %rsp, 8\n"
        "  retq\n"
        ".cfi_endproc\n"
        ".size mcwThunk, .-mcwThunk\n"
        // A reserved part of this module's code, for entries and stubs if
        // no memory can be mapped for them.
        ".balign 16384\n"
        "mcwCave:\n"
        "  .fill 16384, 1, 0xCC\n");
extern uint8_t mcwCave[];
#define CAVE_SIZE 16384u

// ── Memory ───────────────────────────────────────────────────────────────────

int hookProt(const void* p) {
    void *start = NULL, *end = NULL;
    int prot = 0;
    if (sceKernelQueryMemoryProtection((void*)p, &start, &end, &prot) != 0) return -1;
    if ((uintptr_t)p < (uintptr_t)start || (uintptr_t)p >= (uintptr_t)end) return -1;
    return prot;
}

size_t hookReadable(const void* p, size_t n) {
    const uintptr_t a = (uintptr_t)p;
    if (a < 0x10000u || a >= 0x0000800000000000ull) return 0;
    void *start = NULL, *end = NULL;
    int prot = 0;
    if (sceKernelQueryMemoryProtection((void*)p, &start, &end, &prot) != 0) return 0;
    if (a < (uintptr_t)start || a >= (uintptr_t)end || !(prot & 1)) return 0;
    const size_t left = (size_t)((uintptr_t)end - a);
    return left < n ? left : n;
}

typedef struct {
    uint64_t address;
    void* data;
    uint64_t length;
    uint64_t writeFlags;
} __attribute__((packed)) ProcRw;  // GoldHEN SDK's proc_rw

// Writes n bytes of code at dst, wherever it is: 0, or < 0.
static int writeCode(void* dst, const void* src, size_t n) {
    const uintptr_t page = (uintptr_t)dst & ~(uintptr_t)0x3FFF;
    const size_t span = (size_t)((uintptr_t)dst + n - page);
    const int was = hookProt(dst);
    if (sceKernelMprotect((void*)page, span, 7) == 0) {
        if (n == 8 && ((uintptr_t)dst & 63) <= 56) {
            // One store (1.51): inside one cache line, a thread running
            // the function sees the old 8 bytes or the new, never half.
            uint64_t v;
            memcpy(&v, src, 8);
            __asm__ volatile("movq %1, %0" : "=m"(*(volatile uint64_t*)dst) : "r"(v) : "memory");
        } else {
            memcpy(dst, src, n);
        }
        sceKernelMprotect((void*)page, span, was > 0 ? was : 5);
        g_hookHow = "code written after sceKernelMprotect";
    } else {
        static ProcRw rw;
        rw.address = (uint64_t)dst;
        rw.data = (void*)src;
        rw.length = n;
        rw.writeFlags = 1;
        const long rc = sysCall(500, 5 /* PROCESS_RW */, (long)&rw, 0, 0, 0, 0);
        if (rc != 0) return -2;
        g_hookHow = "code written through GoldHEN's PROCESS_RW";
    }
    return memcmp(dst, src, n) == 0 ? 0 : -3;
}

static int nearTo(uintptr_t a, uintptr_t b) {
    const int64_t d = (int64_t)(a - b);
    return d > -0x7FF00000ll && d < 0x7FF00000ll;
}

// Where entries and stubs go.
enum { MEM_RWX, MEM_RW_THEN_RX, MEM_CAVE };
static int g_mode = MEM_RWX;
static uint8_t* g_region;  // the RWX region in use, and how much of it is
static size_t g_regionUsed;
static size_t g_caveUsed;
#define REGION 0x4000u
#define SLOT 128u  // an entry (24 bytes) at 0, its stub at 32

static void* mapNear(uintptr_t near, int prot) {
    for (int k = 0; k <= 128; k++) {
        const int64_t step = (int64_t)(k / 2 + 1) * 0x400000ll * ((k & 1) ? -1 : 1);
        void* hint = k == 128 ? NULL : (void*)((near & ~(uintptr_t)0x3FFF) + step);
        void* p = NULL;
        if (sceKernelMmap(hint, REGION, prot, 0x1000 | 0x2 /* ANON, PRIVATE */, -1, 0, &p) < 0)
            return (void*)-1;  // refused: no use asking again
        if (p && nearTo((uintptr_t)p, near)) return p;
        if (p && k == 128) return p;  // far, but memory
        if (p) sceKernelMunmap(p, REGION);
    }
    return NULL;
}

// A slot for one hook, *isNear whether it is within 2 GB of near.
static uint8_t* slotFor(uintptr_t near, int* isNear) {
    for (;;) {
        if (g_mode == MEM_RWX) {
            if (g_region && g_regionUsed + SLOT <= REGION && nearTo((uintptr_t)g_region, near)) {
                uint8_t* s = g_region + g_regionUsed;
                g_regionUsed += SLOT;
                *isNear = 1;
                return s;
            }
            void* p = mapNear(near, 7);
            if (p == (void*)-1 || !p) {
                g_mode = MEM_RW_THEN_RX;
                continue;
            }
            g_region = p;
            g_regionUsed = SLOT;
            *isNear = nearTo((uintptr_t)p, near);
            g_hookHow = "stubs in a read/write/execute mapping";
            return p;
        }
        if (g_mode == MEM_RW_THEN_RX) {
            void* p = mapNear(near, 3);
            if (p == (void*)-1 || !p) {
                g_mode = MEM_CAVE;
                continue;
            }
            *isNear = nearTo((uintptr_t)p, near);
            g_hookHow = "stubs in a read/write mapping, then read/execute";
            return p;
        }
        if (g_caveUsed + SLOT > CAVE_SIZE) return NULL;
        uint8_t* s = mcwCave + g_caveUsed;
        g_caveUsed += SLOT;
        *isNear = nearTo((uintptr_t)s, near);
        g_hookHow = "stubs in this module's own code";
        return s;
    }
}

static int commitSlot(uint8_t* slot, const uint8_t* code, size_t n) {
    if (g_mode == MEM_RWX) {
        memcpy(slot, code, n);
        return 0;
    }
    if (g_mode == MEM_RW_THEN_RX) {
        memcpy(slot, code, n);
        return sceKernelMprotect(slot, REGION, 5) == 0 ? 0 : -4;
    }
    return writeCode(slot, code, n);
}

// ── Moving instructions ──────────────────────────────────────────────────────

static void put32(uint8_t* p, int32_t v) {
    memcpy(p, &v, 4);
}

static int fits32(int64_t v) {
    return v >= -0x80000000ll && v <= 0x7FFFFFFFll;
}

// Moves at least need bytes of instructions from src to out (to run at
// dst): bytes written to out, *taken the bytes of src moved, or < 0.
static int moveCode(const uint8_t* src, size_t need, uint8_t* out, uintptr_t dst, size_t cap,
                    uint32_t* taken, int canRelocate, const char** why) {
    size_t in = 0, o = 0;
    while (in < need) {
        hde64s hs;
        const unsigned len = hde64_disasm(src + in, &hs);
        if ((hs.flags & F_ERROR) || !len) {
            *why = "an instruction it could not decode";
            return -1;
        }
        if (o + len + 8 > cap) {
            *why = "too many instructions";
            return -1;
        }
        const uintptr_t from = (uintptr_t)src + in + len;  // where it ended
        const uint8_t op = hs.opcode;
        const int ripRel = (hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5;
        if ((op >= 0xE0 && op <= 0xE3) || (op == 0xC3 || op == 0xC2 || op == 0xCC)) {
            *why = (op >= 0xE0 && op <= 0xE3) ? "a loop instruction" : "the function ends too soon";
            return -1;
        }
        if ((hs.flags & F_RELATIVE) || ripRel) {
            if (!canRelocate) {
                *why = "a relative address, and no memory near the function";
                return -1;
            }
        }
        if ((hs.flags & F_RELATIVE) && (hs.flags & F_IMM8)) {
            const uintptr_t to = from + (int8_t)hs.imm.imm8;
            // Into the instructions moved: this one is the last if it
            // reaches need, else up to 15 bytes more may follow it.
            const size_t moved = in + len >= need ? in + len : need + 15;
            if (to >= (uintptr_t)src && to < (uintptr_t)src + moved) {
                *why = "a short jump within its first bytes";
                return -1;
            }
            int64_t rel;
            if (op == 0xEB) {
                out[o] = 0xE9;
                rel = (int64_t)(to - (dst + o + 5));
                if (!fits32(rel)) return *why = "a jump too far", -1;
                put32(out + o + 1, (int32_t)rel);
                o += 5;
            } else if (op >= 0x70 && op <= 0x7F) {
                out[o] = 0x0F;
                out[o + 1] = (uint8_t)(0x80 + (op - 0x70));
                rel = (int64_t)(to - (dst + o + 6));
                if (!fits32(rel)) return *why = "a jump too far", -1;
                put32(out + o + 2, (int32_t)rel);
                o += 6;
            } else {
                *why = "a short relative instruction";
                return -1;
            }
            if (op == 0xEB && in + len < need) {
                *why = "the function jumps away too soon";
                return -1;
            }
            in += len;
            continue;
        }
        memcpy(out + o, src + in, len);
        if ((hs.flags & F_RELATIVE) && (hs.flags & F_IMM32)) {
            // call, jmp, jcc rel32: the rel32 at the end.
            const uintptr_t to = from + (int32_t)hs.imm.imm32;
            const int64_t rel = (int64_t)(to - (dst + o + len));
            if (!fits32(rel)) return *why = "a call too far", -1;
            put32(out + o + len - 4, (int32_t)rel);
            if (op == 0xE9 && in + len < need) {
                *why = "the function jumps away too soon";
                return -1;
            }
        } else if (hs.flags & F_RELATIVE) {
            *why = "a relative instruction it cannot move";
            return -1;
        } else if (ripRel) {
            unsigned imm = (hs.flags & F_IMM64)   ? 8
                           : (hs.flags & F_IMM32) ? 4
                           : (hs.flags & F_IMM16) ? 2
                           : (hs.flags & F_IMM8)  ? 1
                                                  : 0;
            const unsigned at = len - imm - 4;
            const uintptr_t to = from + (int32_t)hs.disp.disp32;
            const int64_t disp = (int64_t)(to - (dst + o + len));
            if (!fits32(disp)) return *why = "data too far", -1;
            put32(out + o + at, (int32_t)disp);
        }
        o += len;
        in += len;
    }
    *taken = (uint32_t)in;
    return (int)o;
}

static size_t jumpAbs(uint8_t* p, uint64_t to) {
    p[0] = 0xFF;
    p[1] = 0x25;
    put32(p + 2, 0);
    memcpy(p + 6, &to, 8);
    return 14;
}

int hookInstall(Hook* h, void* target, const char** why) {
    *why = "";
    if (!target || hookReadable(target, 32) < 32) {
        *why = "no function there";
        return -1;
    }
    if (!(hookProt(target) & 4)) {
        *why = "not code";
        return -1;
    }
    const uint8_t* fn = target;
    int isNear = 0;
    uint8_t* slot = slotFor((uintptr_t)fn, &isNear);
    if (!slot) {
        *why = "no memory for its stub";
        return -1;
    }
    uint8_t code[SLOT];
    memset(code, 0xCC, sizeof(code));
    // The entry: movabs r11, h; jmp [rip+0] -> mcwThunk.
    code[0] = 0x49;
    code[1] = 0xBB;
    memcpy(code + 2, &h, 8);
    jumpAbs(code + 10, (uint64_t)(uintptr_t)mcwThunk);
    // The stub.
    const size_t patch = isNear ? 5 : 14;
    uint32_t taken = 0;
    const int moved = moveCode(fn, patch, code + 32, (uintptr_t)slot + 32, SLOT - 32 - 14, &taken,
                               isNear, why);
    if (moved < 0) return -2;
    jumpAbs(code + 32 + moved, (uint64_t)(uintptr_t)(fn + taken));
    memcpy(h->original, fn, sizeof(h->original));
    h->stolen = taken;
    h->target = target;
    h->stub = slot + 32;
    if (commitSlot(slot, code, sizeof(code)) != 0) {
        *why = "its stub could not be written";
        return -3;
    }
    // The function's first bytes: a jump to the entry.
    uint8_t jmp[14];
    if (isNear) {
        jmp[0] = 0xE9;
        put32(jmp + 1, (int32_t)(int64_t)((uintptr_t)slot - ((uintptr_t)fn + 5)));
    } else {
        jumpAbs(jmp, (uint64_t)(uintptr_t)slot);
    }
    // The jump's bytes past the first 8 (the 14-byte jump's last 6) first,
    // then the first 8 in one store, the bytes after the jump as they were
    // (1.51; a plain copy before, which a thread running the function's
    // first instructions could see half written).
    int rc = 0;
    uint8_t word[8];
    if (hookReadable(fn, 8) == 8) {
        memcpy(word, fn, 8);
        memcpy(word, jmp, patch < 8 ? patch : 8);
        if (patch > 8) rc = writeCode((void*)(uintptr_t)(fn + 8), jmp + 8, patch - 8);
        if (rc == 0) {
            rc = writeCode(target, word, 8);
            if (rc != 0 && patch > 8)  // the tail as it was: the function unchanged
                writeCode((void*)(uintptr_t)(fn + 8), h->original + 8, patch - 8);
        }
    } else {
        rc = writeCode(target, jmp, patch);
    }
    if (rc != 0) {
        *why = rc == -2 ? "the function could not be written (mprotect and PROCESS_RW refused)"
                        : "the function did not change when written";
        return -4;
    }
    h->installed = 1;
    return 0;
}
