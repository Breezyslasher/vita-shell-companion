/**
 * A module's entry, for one the system's loader loads (a .prx): its
 * sce_module_param, and _init and _fini, the ELF's entry points, which call
 * module_start and module_stop. As GoldHEN's plugins have it
 * (GoldHEN_Plugins_SDK, crt/crtprx.c); OpenOrbis's crtlib.o has the param
 * alone.
 */

#include <stddef.h>

int module_start(size_t args, const void* argp);
int module_stop(size_t args, const void* argp);

__asm__(".section \".data.sce_module_param\", \"aw\"\n"
        ".balign 8\n"
        "_sceModuleParam:\n"
        "  .quad 0x18\n"          // its size
        "  .quad 0x13C13F4BF\n"   // magic
        "  .quad 0x4508101\n"     // SDK version
        ".data\n"
        ".balign 8\n"
        ".global __dso_handle\n"
        "__dso_handle: .quad 0\n"
        "_sceLibc: .quad 0\n"
        ".text\n");

__attribute__((visibility("default"))) int _init(size_t args, const void* argp) {
    return module_start(args, argp);
}

__attribute__((visibility("default"))) int _fini(size_t args, const void* argp) {
    return module_stop(args, argp);
}
