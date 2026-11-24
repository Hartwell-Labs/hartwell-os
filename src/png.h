/* Minimal PNG decoder for the media layer: 8-bit, non-interlaced, colour types
 * 0/2/3/4/6. No dependencies — inflate lives in png.c. */
#ifndef HWO_PNG_H
#define HWO_PNG_H

typedef struct {
    int w, h;
    unsigned char *rgba;   /* w*h*4, R,G,B,A */
} Png;

/* Returns 0 on success. Caller frees img->rgba and passes &img to png_free(). */
int png_load(const char *path, Png *img);
void png_free(Png *img);

#endif
