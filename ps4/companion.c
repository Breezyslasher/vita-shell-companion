/**
 * Shell Companion's PS4 module, sc_companion.prx: the player (ps4/bgm) and
 * the music watch (ps4/mcw), one module loaded into SceShellUI once a boot.
 * module_start only starts their threads, so the shell, which waits for it,
 * is never kept waiting.
 */

#include <stddef.h>

void bgmStart(void);
void mcwStart(void);

__attribute__((visibility("default"))) int module_start(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    bgmStart();
    mcwStart();
    return 0;
}

__attribute__((visibility("default"))) int module_stop(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    return 0;
}
