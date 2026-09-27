// UTF-8 text fitting for the dj_ui presentation. See ui_djui_text.h.
#include "ui_djui_text.h"

#include <stdint.h>

/* ASCII for U+00A0..U+017F. Every entry is at most as long as the 2-byte
 * sequence it replaces, which keeps the in-place fit safe. */
static const char k_latin[0x180 - 0xA0][3] = {
    " ", "!", "c", "L", "?", "Y", "|", "S", /* U+00A0 */
    "\"", "C", "a", "\"", "-", "", "R", "-", /* U+00A8 */
    "o", "+", "2", "3", "'", "u", "P", ".", /* U+00B0 */
    ",", "1", "o", "\"", "?", "?", "?", "?", /* U+00B8 */
    "A", "A", "A", "A", "A", "A", "AE", "C", /* U+00C0 */
    "E", "E", "E", "E", "I", "I", "I", "I", /* U+00C8 */
    "D", "N", "O", "O", "O", "O", "O", "x", /* U+00D0 */
    "O", "U", "U", "U", "U", "Y", "TH", "ss", /* U+00D8 */
    "a", "a", "a", "a", "a", "a", "ae", "c", /* U+00E0 */
    "e", "e", "e", "e", "i", "i", "i", "i", /* U+00E8 */
    "d", "n", "o", "o", "o", "o", "o", "/", /* U+00F0 */
    "o", "u", "u", "u", "u", "y", "th", "y", /* U+00F8 */
    "A", "a", "A", "a", "A", "a", "C", "c", /* U+0100 */
    "C", "c", "C", "c", "C", "c", "D", "d", /* U+0108 */
    "D", "d", "E", "e", "E", "e", "E", "e", /* U+0110 */
    "E", "e", "E", "e", "G", "g", "G", "g", /* U+0118 */
    "G", "g", "G", "g", "H", "h", "H", "h", /* U+0120 */
    "I", "i", "I", "i", "I", "i", "I", "i", /* U+0128 */
    "I", "i", "IJ", "ij", "J", "j", "K", "k", /* U+0130 */
    "k", "L", "l", "L", "l", "L", "l", "L", /* U+0138 */
    "l", "L", "l", "N", "n", "N", "n", "N", /* U+0140 */
    "n", "n", "N", "n", "O", "o", "O", "o", /* U+0148 */
    "O", "o", "OE", "oe", "R", "r", "R", "r", /* U+0150 */
    "R", "r", "S", "s", "S", "s", "S", "s", /* U+0158 */
    "S", "s", "T", "t", "T", "t", "T", "t", /* U+0160 */
    "U", "u", "U", "u", "U", "u", "U", "u", /* U+0168 */
    "U", "u", "U", "u", "W", "w", "Y", "y", /* U+0170 */
    "Y", "Z", "z", "Z", "z", "Z", "z", "s", /* U+0178 */};

/* ASCII for U+2010..U+2026 (dashes, quotes, bullets, ellipsis). */
static const char k_punct[0x2027 - 0x2010][4] = {
    "-", "-", "-", "-", "-", "-", "|", "_", /* U+2010 */
    "'", "'", "'", "'", "\"", "\"", "\"", "\"", /* U+2018 */
    "+", "+", "*", ">", ".", "..", "...", /* U+2020 */
};

/* Decodes one sequence at p. Returns its length in bytes, 0 when src ends
 * inside it (NUL among the continuation bytes). *cp = UINT32_MAX when the
 * sequence is malformed; the length is then 1 so decoding resyncs. */
static size_t decode(const unsigned char *p, uint32_t *cp)
{
    unsigned char c = p[0];
    size_t n;
    uint32_t v;
    if (c < 0x80u) {
        *cp = c;
        return 1;
    } else if (c >= 0xC2u && c <= 0xDFu) {
        n = 2;
        v = c & 0x1Fu;
    } else if (c >= 0xE0u && c <= 0xEFu) {
        n = 3;
        v = c & 0x0Fu;
    } else if (c >= 0xF0u && c <= 0xF4u) {
        n = 4;
        v = c & 0x07u;
    } else {
        *cp = UINT32_MAX;
        return 1;
    }
    for (size_t i = 1; i < n; i++) {
        if (p[i] == 0u) {
            return 0;
        }
        if ((p[i] & 0xC0u) != 0x80u) {
            *cp = UINT32_MAX;
            return 1;
        }
        v = (v << 6) | (p[i] & 0x3Fu);
    }
    *cp = v;
    return n;
}

static int kept(uint32_t cp)
{
#if UI_DJUI_FONT_LATIN
    return (cp >= 0xA0u && cp < 0x180u) || (cp >= 0x2013u && cp <= 0x2026u);
#else
    (void)cp;
    return 0;
#endif
}

size_t ui_djui_text_fit(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return 0;
    }
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    const unsigned char *in = (const unsigned char *)src;
    size_t out = 0;
    while (*in) {
        uint32_t cp;
        size_t n = decode(in, &cp);
        if (n == 0) {
            break;
        }
        const char *rep;
        size_t rep_len;
        char one[2] = { 0, 0 };
        if (cp == UINT32_MAX) {
            rep = "?";
        } else if (cp < 0x20u || cp == 0x7Fu) {
            rep = " ";
        } else if (cp < 0x80u) {
            one[0] = (char)cp;
            rep = one;
        } else if (kept(cp)) {
            rep = NULL;
        } else if (cp >= 0xA0u && cp < 0x180u) {
            rep = k_latin[cp - 0xA0u];
        } else if (cp >= 0x2010u && cp <= 0x2026u) {
            rep = k_punct[cp - 0x2010u];
        } else if (cp == 0x20ACu) {
            rep = "EUR";
        } else if (cp == 0x2122u) {
            rep = "TM";
        } else {
            rep = "?";
        }
        rep_len = 0;
        if (rep) {
            while (rep[rep_len]) rep_len++;
        } else {
            rep_len = n;
        }
        if (out + rep_len + 1u > cap) {
            break;
        }
        for (size_t i = 0; i < rep_len; i++) {
            dst[out + i] = rep ? rep[i] : (char)in[i];
        }
        out += rep_len;
        in += n;
    }
    dst[out] = '\0';
    return out;
}
