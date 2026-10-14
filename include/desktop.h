#ifndef HARIS_DESKTOP_H
#define HARIS_DESKTOP_H

void desktop_set_framebuffer(void *framebuffer, unsigned int width,
                             unsigned int height, unsigned int pixels_per_scanline,
                             unsigned int red_mask_mode);
void desktop_set_status(const char *line); /* one line shown in the welcome window */
void desktop_draw(void);

#endif
