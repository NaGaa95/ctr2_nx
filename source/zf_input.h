#ifndef ZF_INPUT_H
#define ZF_INPUT_H
#include <stdint.h>
typedef void (*zf_touch_fn)(void *, void *, float, float, int, int);
typedef uint8_t (*zf_button_fn)(void *, void *);
void zf_input_init(void);
int zf_input_update(zf_touch_fn touch, zf_button_fn back, void *env, void *renderer);
void zf_input_draw_cursor(void);
#endif
