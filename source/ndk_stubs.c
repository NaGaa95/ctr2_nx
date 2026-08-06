/* NDK compatibility symbols required by the Android libraries. Active Unity
 * input and asset access use the JNI implementations in this port. */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/types.h>
#include "asset_pack.h"
#include "config.h"
#include "util.h"

/* AInputQueue / AInputEvent / AMotionEvent / AKeyEvent */
void    AInputQueue_attachLooper(void *a, void *b, int c, void *d, void *e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void    AInputQueue_detachLooper(void *a) { (void)a; }
int32_t AInputQueue_getEvent(void *a, void **b) { (void)a;(void)b; return -1; }   /* no events */
int32_t AInputQueue_preDispatchEvent(void *a, void *b) { (void)a;(void)b; return 0; }
void    AInputQueue_finishEvent(void *a, void *b, int c) { (void)a;(void)b;(void)c; }
int32_t AInputEvent_getType(const void *a) { (void)a; return 0; }
int32_t AMotionEvent_getAction(const void *a) { (void)a; return 0; }
size_t  AMotionEvent_getPointerCount(const void *a) { (void)a; return 0; }
int32_t AMotionEvent_getPointerId(const void *a, size_t b) { (void)a;(void)b; return 0; }
float   AMotionEvent_getX(const void *a, size_t b) { (void)a;(void)b; return 0.0f; }
float   AMotionEvent_getY(const void *a, size_t b) { (void)a;(void)b; return 0.0f; }
int32_t AKeyEvent_getKeyCode(const void *a) { (void)a; return 0; }
int32_t AKeyEvent_getFlags(const void *a) { (void)a; return 0; }
int32_t AKeyEvent_getRepeatCount(const void *a) { (void)a; return 0; }

/* AConfiguration */
void *AConfiguration_new(void) { return NULL; }
void  AConfiguration_fromAssetManager(void *a, void *b) { (void)a;(void)b; }
void  AConfiguration_getLanguage(void *a, char *out) { (void)a; if (out) { out[0]='e'; out[1]='n'; } }
void  AConfiguration_getCountry(void *a, char *out) { (void)a; if (out) { out[0]='U'; out[1]='S'; } }
void  AConfiguration_delete(void *a) { (void)a; }

/* Native AssetManager support. Android plug-ins use libandroid's
 * AAsset API directly instead of going through Unity's Java AssetManager. */
typedef struct {
  FILE *file;
  int packed_fd;
  int64_t length;
  void *buffer;
} NxAsset;

typedef struct {
  void *dir;
  int packed;
} NxAssetDir;

static int g_asset_manager;

static int asset_path(char *out, size_t cap, const char *name) {
  if (!out || !cap || !name) return 0;
  while (*name == '/') name++;
  if (strstr(name, "..") || strchr(name, ':')) {
    return 0;
  }
  const int n = snprintf(out, cap, "%s/assets/%s", GAME_HOME, name);
  return n > 0 && (size_t)n < cap;
}

void *AAssetManager_fromJava(void *env, void *manager) {
  (void)env;(void)manager;
  return &g_asset_manager;
}

void *AAssetManager_open(void *manager, const char *name, int mode) {
  (void)manager;
  (void)mode;
  char path[768];
  if (!asset_path(path, sizeof path, name)) return NULL;
  int packed_fd = asset_pack_open_path(path);
  if (packed_fd >= 0) {
    uint64_t length = 0;
    if (!asset_pack_fstat_fd(packed_fd, &length, NULL, NULL) ||
        length > INT64_MAX) {
      asset_pack_close_fd(packed_fd);
      return NULL;
    }
    NxAsset *asset = calloc(1, sizeof *asset);
    if (!asset) {
      asset_pack_close_fd(packed_fd);
      return NULL;
    }
    asset->packed_fd = packed_fd;
    asset->length = (int64_t)length;
    return asset;
  }
  FILE *file = fopen(path, "rb");
  if (!file)return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  const long end = ftell(file);
  if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  NxAsset *asset = calloc(1, sizeof *asset);
  if (!asset) {
    fclose(file);
    return NULL;
  }
  asset->file = file;
  asset->packed_fd = -1;
  asset->length = end;
  return asset;
}

int AAsset_read(void *opaque, void *buffer, size_t count) {
  NxAsset *asset = opaque;
  if (!asset || (!asset->file && asset->packed_fd < 0) || (!buffer && count))
    return -1;
  if (asset->packed_fd >= 0) {
    long got = asset_pack_read_fd(asset->packed_fd, buffer, count);
    if (got < 0) return -1;
    return got > INT32_MAX ? INT32_MAX : (int)got;
  }
  const size_t got = fread(buffer, 1, count, asset->file);
  if (!got && ferror(asset->file)) return -1;
  return got > INT32_MAX ? INT32_MAX : (int)got;
}

int64_t AAsset_seek(void *opaque, int64_t offset, int whence) {
  NxAsset *asset = opaque;
  if (!asset || (!asset->file && asset->packed_fd < 0)) return -1;
  if (asset->packed_fd >= 0)
    return asset_pack_lseek_fd(asset->packed_fd, (long)offset, whence);
  if (fseek(asset->file, (long)offset, whence) != 0) return -1;
  return (int64_t)ftell(asset->file);
}

const void *AAsset_getBuffer(void *opaque) {
  NxAsset *asset = opaque;
  if (!asset || (!asset->file && asset->packed_fd < 0)) return NULL;
  if (asset->buffer) return asset->buffer;
  const size_t size = (size_t)asset->length;
  void *buffer = malloc(size ? size : 1);
  if (!buffer) return NULL;
  if (asset->packed_fd >= 0) {
    if (size && asset_pack_pread_fd(asset->packed_fd, buffer, size, 0) !=
                    (long)size) {
      free(buffer);
      return NULL;
    }
    asset->buffer = buffer;
    return buffer;
  }
  const long saved = ftell(asset->file);
  if (fseek(asset->file, 0, SEEK_SET) != 0 ||
      (size && fread(buffer, 1, size, asset->file) != size)) {
    free(buffer);
    if (saved >= 0) fseek(asset->file, saved, SEEK_SET);
    return NULL;
  }
  if (saved >= 0) fseek(asset->file, saved, SEEK_SET);
  asset->buffer = buffer;
  return buffer;
}

int64_t AAsset_getLength(void *opaque) {
  NxAsset *asset = opaque;
  return asset ? asset->length : 0;
}

int64_t AAsset_getRemainingLength(void *opaque) {
  NxAsset *asset = opaque;
  if (!asset) return 0;
  int64_t at = AAsset_seek(asset, 0, SEEK_CUR);
  if (at < 0 || at > asset->length) return 0;
  return asset->length - at;
}

void AAsset_close(void *opaque) {
  NxAsset *asset = opaque;
  if (!asset) return;
  if (asset->file) fclose(asset->file);
  if (asset->packed_fd >= 0) asset_pack_close_fd(asset->packed_fd);
  free(asset->buffer);
  free(asset);
}

void *AAssetManager_openDir(void *manager, const char *name) {
  (void)manager;
  char path[768];
  if (!asset_path(path, sizeof path, name ? name : "")) return NULL;
  void *packed_dir = asset_pack_opendir_path(path);
  if (packed_dir) {
    NxAssetDir *asset_dir = calloc(1, sizeof *asset_dir);
    if (!asset_dir) {
      asset_pack_closedir_path(packed_dir);
      return NULL;
    }
    asset_dir->dir = packed_dir;
    asset_dir->packed = 1;
    return asset_dir;
  }
  DIR *dir = opendir(path);
  if (!dir) {
    return NULL;
  }
  NxAssetDir *asset_dir = calloc(1, sizeof *asset_dir);
  if (!asset_dir) {
    closedir(dir);
    return NULL;
  }
  asset_dir->dir = dir;
  return asset_dir;
}

void AAssetDir_close(void *opaque) {
  NxAssetDir *asset_dir = opaque;
  if (!asset_dir) return;
  if (asset_dir->dir) {
    if (asset_dir->packed) asset_pack_closedir_path(asset_dir->dir);
    else closedir((DIR *)asset_dir->dir);
  }
  free(asset_dir);
}

/* AndroidBitmap (engine dynamic-text path; unused) */
int AndroidBitmap_getInfo(void *a, void *b, void *c) { (void)a;(void)b;(void)c; return -1; }
int AndroidBitmap_lockPixels(void *a, void *b, void **c) { (void)a;(void)b;(void)c; return -1; }
int AndroidBitmap_unlockPixels(void *a, void *b) { (void)a;(void)b; return 0; }
