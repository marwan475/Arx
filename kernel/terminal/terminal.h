#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct flanterm_context;

typedef struct kernel_framebuffer
{
    void*   address;
    size_t  width;
    size_t  height;
    size_t  pitch;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
} kernel_framebuffer_t;

bool                     terminal_init(const kernel_framebuffer_t* framebuffer);
struct flanterm_context* terminal_get_context(void);

#endif
