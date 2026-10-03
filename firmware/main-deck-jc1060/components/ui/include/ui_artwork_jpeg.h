/* v302: why a cover JPEG is refused, without decoding it. TJpgDec (the LVGL
 * copy ui_artwork_thumb decodes with) takes baseline Huffman frames whose Y
 * sampling is 1x1, 2x1 or 2x2 and whose Cb/Cr are 1x1. v301 hardware: no
 * cover on DJ Link tracks of a vynull peer. vynull re-encodes every cover
 * with ffmpeg mjpeg; a PNG source comes out yuvj444p with all three
 * components 1x2 (sampling 0x12), which TJpgDec rejects (JDR_FMT3), while a
 * JPEG source comes out 4:2:0 and decodes. Pure, host-tested. */
#ifndef UI_ARTWORK_JPEG_H
#define UI_ARTWORK_JPEG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_ARTWORK_JPEG_OK = 0,         /* TJpgDec takes the frame header */
    UI_ARTWORK_JPEG_NOT_JPEG,       /* no SOI, or no frame header before the scan */
    UI_ARTWORK_JPEG_PROGRESSIVE,    /* SOF2 or any other non-baseline frame */
    UI_ARTWORK_JPEG_SAMPLING,       /* chroma layout TJpgDec does not decode */
} ui_artwork_jpeg_verdict_t;

typedef struct {
    ui_artwork_jpeg_verdict_t verdict;
    uint8_t  sof;                   /* frame marker (0xC0..0xCF), 0 = none */
    uint8_t  components;
    uint8_t  sampling[3];           /* H << 4 | V of the first three components */
    uint16_t width, height;
} ui_artwork_jpeg_info_t;

ui_artwork_jpeg_info_t ui_artwork_jpeg_probe(const uint8_t *jpg, size_t len);
const char *ui_artwork_jpeg_verdict_name(ui_artwork_jpeg_verdict_t verdict);

#ifdef __cplusplus
}
#endif

#endif
