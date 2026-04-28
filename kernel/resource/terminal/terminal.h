#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct flanterm_context;
typedef void (*terminal_callback_t)(struct flanterm_context*, uint64_t, uint64_t, uint64_t, uint64_t);

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
void                     terminal_write(const char* buffer, size_t count);
void                     terminal_flush(void);
void                     terminal_full_refresh(void);
void                     terminal_deinit(void (*_free)(void* ptr, size_t size));
void                     terminal_get_dimensions(size_t* cols, size_t* rows);
void                     terminal_set_autoflush(bool state);
void                     terminal_set_callback(terminal_callback_t callback);
void                     terminal_get_cursor_pos(size_t* x, size_t* y);
void                     terminal_set_cursor_pos(size_t x, size_t y);
void                     terminal_set_text_fg(size_t colour, bool bright);
void                     terminal_set_text_bg(size_t colour, bool bright);
void                     terminal_reset_text_fg(void);
void                     terminal_reset_text_bg(void);
void                     terminal_clear(bool move);

#endif
