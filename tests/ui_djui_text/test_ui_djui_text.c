/* Host tests for ui_djui_text_fit (dj_ui Library phase 4: accents, UTF-8). */
#include "ui_djui_text.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void expect_fit(const char *name, size_t cap, const char *src, const char *want)
{
    char out[128];
    memset(out, 'Z', sizeof(out));
    size_t n = ui_djui_text_fit(out, cap, src);
    if (strcmp(out, want) != 0 || n != strlen(want)) {
        printf("FAIL %s: got \"%s\" (%u), want \"%s\"\n", name, out, (unsigned)n, want);
        s_failures++;
    }
}

static void test_ascii_passthrough(void)
{
    expect_fit("ascii", 64, "Daft Punk - One More Time", "Daft Punk - One More Time");
    expect_fit("empty", 64, "", "");
    expect_fit("null", 64, NULL, "");
}

#if UI_DJUI_FONT_LATIN
/* Latin-1 font: the family's range passes through, the rest still folds. */
static void test_french_titles(void)
{
    const char *fr = "\xC3\x89t\xC3\xA9 Paris \xE2\x80\x93 Gar\xC3\xA7on \xC2\xBB";
    expect_fit("fr kept", 64, fr, fr);
    expect_fit("ext-a kept", 64, "C\xC5\x93ur", "C\xC5\x93ur");
    expect_fit("dash folds", 64, "a\xE2\x80\x90" "b", "a-b");
    expect_fit("euro folds", 64, "5 \xE2\x82\xAC", "5 EUR");
    expect_fit("cap keeps sequence whole", 3, "a\xC3\xA9", "a");
}
#else
static void test_french_titles(void)
{
    /* "Été Paris – Garçon »" */
    expect_fit("fr", 64, "\xC3\x89t\xC3\xA9 Paris \xE2\x80\x93 Gar\xC3\xA7on \xC2\xBB",
               "Ete Paris - Garcon \"");
    expect_fit("ligatures", 64, "C\xC5\x93ur \xC3\x86on Stra\xC3\x9F" "e", "Coeur AEon Strasse");
    expect_fit("ext-a", 64, "\xC5\x81\xC3\xB3" "d\xC5\xBA \xC4\x8C" "e\xC5\xA1ky", "Lodz Cesky");
    expect_fit("punct", 64, "\xE2\x80\x9CLive\xE2\x80\x9D \xE2\x80\x98" "Edit\xE2\x80\x99\xE2\x80\xA6",
               "\"Live\" 'Edit'...");
    expect_fit("euro", 64, "5 \xE2\x82\xAC", "5 EUR");
}
#endif

static void test_unsupported_and_malformed(void)
{
    expect_fit("cjk", 64, "\xE6\x9D\xB1\xE4\xBA\xAC Mix", "?? Mix");
    expect_fit("emoji", 64, "Hit \xF0\x9F\x94\xA5", "Hit ?");
    expect_fit("stray continuation", 64, "a\x80" "b", "a?b");
    expect_fit("bad lead", 64, "a\xC0\xAF" "b", "a??b");
    expect_fit("latin1 byte", 64, "Caf\xE9 Bar", "Caf? Bar");
    expect_fit("control", 64, "A\tB\nC", "A B C");
    /* A sequence cut by an earlier byte-count truncation is dropped. */
    expect_fit("cut tail", 64, "Gar\xC3", "Gar");
    expect_fit("cut tail 3", 64, "x\xE2\x80", "x");
}

static void test_capacity(void)
{
    expect_fit("cap exact", 6, "Hello", "Hello");
    expect_fit("cap short", 4, "Hello", "Hel");
    expect_fit("cap one", 1, "Hello", "");
    /* Folding never cuts a replacement in half. */
    expect_fit("cap fold", 5, "abc\xC3\x86", "abc");
    expect_fit("cap ellipsis", 5, "ab\xE2\x80\xA6", "ab");
    char tiny[1] = { 'x' };
    if (ui_djui_text_fit(tiny, 0, "abc") != 0 || tiny[0] != 'x') {
        printf("FAIL cap zero wrote to dst\n");
        s_failures++;
    }
}

static void test_in_place(void)
{
#if !UI_DJUI_FONT_LATIN
    char buf[64] = "\xC3\x89t\xC3\xA9 \xE2\x80\x93 \xC5\x92uvre \xE2\x80\xA6";
    ui_djui_text_fit(buf, sizeof(buf), buf);
    if (strcmp(buf, "Ete - OEuvre ...") != 0) {
        printf("FAIL in place: \"%s\"\n", buf);
        s_failures++;
    }
#endif
}

int main(void)
{
    test_ascii_passthrough();
    test_french_titles();
    test_unsupported_and_malformed();
    test_capacity();
    test_in_place();
    if (s_failures) {
        printf("ui_djui_text: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("ui_djui_text: all tests passed\n");
    return 0;
}
