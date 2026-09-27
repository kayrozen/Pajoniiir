/* v270: PWV3 length refines the PDB whole-second duration, the time base of
 * the PVBR seek table and of every waveform/touch mapping. */
#include "rekordbox_anlz.h"

#include <assert.h>
#include <stdio.h>

/* Where decoding restarts for a seek, as the engine computes it: entry
 * idx = target * 400 / duration at idx * duration / 400. */
static uint32_t pvbr_entry_ms(uint32_t target_ms, uint32_t duration_ms)
{
    uint32_t idx = (uint32_t)(((uint64_t)target_ms * ANLZ_VBR_TABLE_LEN) / duration_ms);
    return (uint32_t)(((uint64_t)idx * duration_ms) / ANLZ_VBR_TABLE_LEN);
}

static void test_pwv3_refines_pdb(void)
{
    /* 222.4 s of audio, PDB truncated to 222 s. */
    assert(anlz_precise_duration_ms(222000u, 33360u) == 222400u);
    /* PDB rounded up. */
    assert(anlz_precise_duration_ms(223000u, 33360u) == 222400u);
    /* No PDB length at all. */
    assert(anlz_precise_duration_ms(0u, 33360u) == 222400u);
}

static void test_pdb_kept_without_usable_pwv3(void)
{
    assert(anlz_precise_duration_ms(222000u, 0u) == 222000u);
    /* PWV3 from another file or cut short: out of tolerance. */
    assert(anlz_precise_duration_ms(222000u, 30000u) == 222000u);
    assert(anlz_precise_duration_ms(222000u, 33600u) == 222000u);
    /* Clamped at the parser cap: truncated, never a length. */
    assert(anlz_precise_duration_ms(900000u, ANLZ_WAVEFORM_HIGH_MAX) == 900000u);
    assert(anlz_precise_duration_ms(0u, 0u) == 0u);
}

static void test_pvbr_landing_follows_the_time_base(void)
{
    /* The v269 HW seek: entry 304 of a 222 s base is 168720 ms, but of the
     * real 222.4 s track it is 169024 ms, 304 ms past where the engine and
     * the waveform put the audio. With the precise base they agree. */
    const uint32_t real_ms = anlz_precise_duration_ms(222000u, 33360u);
    const uint32_t coarse = pvbr_entry_ms(168923u, 222000u);
    const uint32_t exact = pvbr_entry_ms(168923u, real_ms);
    assert(coarse == 168720u);
    assert((uint64_t)304u * real_ms / ANLZ_VBR_TABLE_LEN == 169024u);
    assert(exact <= 168923u && 168923u - exact < real_ms / ANLZ_VBR_TABLE_LEN);
}

int main(void)
{
    test_pwv3_refines_pdb();
    test_pdb_kept_without_usable_pwv3();
    test_pvbr_landing_follows_the_time_base();
    printf("anlz_duration_jc1060: all tests passed\n");
    return 0;
}
