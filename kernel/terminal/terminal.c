#include <terminal/terminal.h>

#include <flanterm.h>
#include <flanterm_backends/fb.h>
#include <klib/klib.h>

bool terminal_init(const kernel_framebuffer_t* framebuffer)
{
    if (framebuffer == NULL || framebuffer->address == NULL || framebuffer->width == 0 || framebuffer->height == 0 || framebuffer->pitch == 0)
    {
        dispatcher.terminal_context = NULL;
        return false;
    }

    dispatcher.terminal_context = flanterm_fb_init(
        NULL,
        NULL,
        (uint32_t*) framebuffer->address,
        framebuffer->width,
        framebuffer->height,
        framebuffer->pitch,
        framebuffer->red_mask_size,
        framebuffer->red_mask_shift,
        framebuffer->green_mask_size,
        framebuffer->green_mask_shift,
        framebuffer->blue_mask_size,
        framebuffer->blue_mask_shift,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        0,
        0,
        1,
        0,
        0,
        0,
        0);

    return dispatcher.terminal_context != NULL;
}

struct flanterm_context* terminal_get_context(void)
{
    return dispatcher.terminal_context;
}
