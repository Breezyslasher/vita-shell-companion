/**
 * The PS4 background music module's files (bgm.c): where an app, or you
 * by FTP, puts its queue and commands, and reads what is playing.
 */

#ifndef SC_PS4_BGM_H
#define SC_PS4_BGM_H

#define SC_BGM_VERSION "0.1"
#define SC_BGM_PROTOCOL 1

#define SC_BGM_DIR "/data/ShellCompanion/ps4"
#define SC_BGM_MUSIC SC_BGM_DIR "/music"
#define SC_BGM_QUEUE SC_BGM_DIR "/queue.txt"
#define SC_BGM_CMD SC_BGM_DIR "/cmd"
#define SC_BGM_PORT SC_BGM_DIR "/port.txt"
#define SC_BGM_STATUS SC_BGM_DIR "/status.txt"
#define SC_BGM_PRX SC_BGM_DIR "/sc_bgm.prx"

#endif
