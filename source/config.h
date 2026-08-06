#ifndef CTR2_CONFIG_H
#define CTR2_CONFIG_H
#include <stddef.h>
#define MMAP_ARENA_ALIGN_36 ((size_t) 64 * 1024 * 1024)
#define MMAP_ARENA_ALIGN_39 ((size_t)128 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)256 * 1024 * 1024)
#define OC_WINDOW_BYTES     ((size_t)256 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t)256 * 1024 * 1024)
#define GAME_TITLE        "Cut the Rope 2"
#define GAME_PACKAGE      "com.zeptolab.ctr2.f2p.google"
#define GAME_VERSION_CODE 2579468
#define GAME_VERSION_NAME "1.47.1"
#define GAME_HOME         "sdmc:/switch/ctr2_nx"
#define GAME_LIBRARY      "lib/arm64-v8a/libctr2.so"
#define GAME_LIBRARY_SIZE 18176936LL
#define ZF_RENDERER_API   2
#define DEFAULT_PORTRAIT  1
#define CONFIG_NAME "config.txt"
extern int screen_width;
extern int screen_height;
#define LANG_AUTO 0
#define LANG_JA 1
#define LANG_EN 2
typedef struct { int screen_width, screen_height, language, portrait; } Config;
extern Config config;
int read_config(const char *file);
int write_config(const char *file);
#endif
