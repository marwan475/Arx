#include <flanterm.h>
#include <flanterm_backends/fb.h>
#include <klib/klib.h>
#include <terminal/terminal.h>

bool terminal_init(const kernel_framebuffer_t* framebuffer)
{
    if (framebuffer == NULL || framebuffer->address == NULL || framebuffer->width == 0 || framebuffer->height == 0 || framebuffer->pitch == 0)
    {
        dispatcher.terminal_context = NULL;
        return false;
    }

    dispatcher.terminal_context = flanterm_fb_init(NULL, NULL, (uint32_t*) framebuffer->address, framebuffer->width, framebuffer->height, framebuffer->pitch, framebuffer->red_mask_size, framebuffer->red_mask_shift, framebuffer->green_mask_size, framebuffer->green_mask_shift,
                                                   framebuffer->blue_mask_size, framebuffer->blue_mask_shift, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 1, 0, 0, 0, 0);

    return dispatcher.terminal_context != NULL;
}

struct flanterm_context* terminal_get_context(void)
{
    return dispatcher.terminal_context;
}

void terminal_write(const char* buffer, size_t count)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL || buffer == NULL || count == 0)
    {
        return;
    }

    flanterm_write(context, buffer, count);
}

void terminal_flush(void)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_flush(context);
}

void terminal_full_refresh(void)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_full_refresh(context);
}

void terminal_deinit(void (*_free)(void* ptr, size_t size))
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_deinit(context, _free);
    dispatcher.terminal_context = NULL;
}

void terminal_get_dimensions(size_t* cols, size_t* rows)
{
    struct flanterm_context* context = terminal_get_context();
    if (cols != NULL)
    {
        *cols = 0;
    }
    if (rows != NULL)
    {
        *rows = 0;
    }
    if (context == NULL)
    {
        return;
    }

    flanterm_get_dimensions(context, cols, rows);
}

void terminal_set_autoflush(bool state)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_set_autoflush(context, state);
}

void terminal_set_callback(terminal_callback_t callback)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_set_callback(context, callback);
}

void terminal_get_cursor_pos(size_t* x, size_t* y)
{
    struct flanterm_context* context = terminal_get_context();
    if (x != NULL)
    {
        *x = 0;
    }
    if (y != NULL)
    {
        *y = 0;
    }
    if (context == NULL)
    {
        return;
    }

    flanterm_get_cursor_pos(context, x, y);
}

void terminal_set_cursor_pos(size_t x, size_t y)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_set_cursor_pos(context, x, y);
}

void terminal_set_text_fg(size_t colour, bool bright)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_set_text_fg(context, colour, bright);
}

void terminal_set_text_bg(size_t colour, bool bright)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_set_text_bg(context, colour, bright);
}

void terminal_reset_text_fg(void)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_reset_text_fg(context);
}

void terminal_reset_text_bg(void)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_reset_text_bg(context);
}

void terminal_clear(bool move)
{
    struct flanterm_context* context = terminal_get_context();
    if (context == NULL)
    {
        return;
    }

    flanterm_clear(context, move);
}
