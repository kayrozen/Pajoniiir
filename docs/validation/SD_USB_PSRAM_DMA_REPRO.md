# SDMMC + USB-DWC concurrent PSRAM DMA: isochronous OUT stream stops

Status: **local draft, NOT published.** Root cause confirmed and workaround
validated on hardware (v322 A/B, v323). Do not file it with Espressif (esp-idf /
esp-usb) without the operator's explicit agreement.

## Summary

On ESP32-P4 (ESP-IDF v6.0.2), when the SDMMC host DMAs a microSD transfer
directly to/from PSRAM while the USB-DWC host DMAs isochronous OUT URBs that
also live in PSRAM (`CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`), the
isochronous OUT stream eventually stops **silently**:

- no URB completes any more (isoc callbacks drop to 0 per window);
- no transfer error, no packet error, no BNA recovery, no `USB_TRANSFER_STATUS_*`
  other than COMPLETED before the stop;
- the application producer keeps running but finds the UAC ring full, so its
  pacing falls back to a deadline and the block rate drops (~34 blk/s instead
  of 187).

Making every SDMMC transfer that targets PSRAM bounce through an internal-RAM
buffer (`sdmmc_host_t.check_buffer_alignment` hook + `dma_aligned_buffer`)
removes the failure completely in the same scenario.

## Environment

| Item | Value |
| --- | --- |
| SoC / board | ESP32-P4, Guition JC1060P470 (32 MB hex PSRAM, 200 MHz) |
| Chip revision | v1.x silicon: the image targets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, `CONFIG_ESP32P4_REV_MIN_FULL=100`, so it runs only on v1.0-v1.99. Exact ECO **to be read** (`esptool.py --port /dev/ttyUSB0 chip_id`, or a boot with the bootloader log at INFO, which prints `chip revision: vX.Y`); the HIL builds log the bootloader at level 0 and the app at WARN, so no captured log carries it |
| ESP-IDF | v6.0.2 (picolibc, FreeRTOS 1000 Hz) |
| USB host | esp-usb `host/usb` 1.5.0 from the fork `https://github.com/dvucinozd/esp-usb.git` at `cc65dc268f9fb6e89b8b3c6c9e94f5aa1dbb2ccb` (same commit for `usb_host_msc`; `components/usb_storage/idf_component.yml`, `dependencies.lock`) |
| Local esp-usb patches | build-time source transforms, generated under `build/` (`cmake/apply_espressif_usb_fifo_patch.cmake`: per-controller HS/FS FIFO split; `apply_espressif_usb_idle_recovery_patch.cmake`: indexed USB0 idle recovery; `apply_usb_host_msc_teardown_patch.cmake`) and a link wrap of `usb_dwc_hal_chan_decode_intr` (BNA recovery, esp-idf #19111) in `components/usb_storage`. A clean repro should run on unpatched upstream esp-usb to rule them out |
| USB ports | UAC (and MIDI) on the FS DWC controller (USB1, direct root); USB mass storage on the HS DWC controller (USB0) |
| UAC device | Pioneer DDJ-400, full speed |
| UAC stream | 48 kHz, 4 ch, 24-bit, 3 isoc OUT URBs x 4 packets (1 ms each) in flight |
| microSD | SDMMC slot 0, 4-bit, high speed; SDMMC controller shared with ESP-Hosted SDIO (slot 1) |
| FATFS | `CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y` |
| USB DMA | `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y` (non-default; upstream `main-deck-p4` keeps it off) |
| SD wait | `sdmmc_wait_for_idle` wrapped to poll once per tick (esp-idf #19034 behaviour, fixed on release/v6.0 by `a2b2b36a57`, not in v6.0.2) |

Before any publication: fill in the exact chip ECO (row above) and rerun the
minimal repro below on unpatched upstream esp-usb.

## Trigger

Concurrent load, both DMA engines on PSRAM:

1. UAC isochronous OUT stream running (audio playback to the DDJ-400).
2. A large file written to the microSD from a 64-byte-aligned PSRAM buffer in
   64 KB `write()` calls (DJ Link download, ~12 MB at 150-450 KB/s), while the
   audio loader reads 32 KB pages from another file on the same card into
   64-byte-aligned PSRAM buffers.

With the buffers aligned, `sdmmc_host_check_buffer_alignment()` accepts them
and the card DMAs straight into/out of PSRAM.

## Evidence (HIL, 2026-09/10)

| Build | SD DMA target | USB DMA buffers | Boots with download | Isoc OUT stop |
| --- | --- | --- | --- | --- |
| <= v317 | internal bounce (PSRAM buffers unaligned) | PSRAM | 11 | 0 |
| v318-v321 | direct PSRAM (buffers aligned) | PSRAM | 5 | 4 |
| v322 (A/B hook) | internal bounce forced for PSRAM | PSRAM | several downloads, 1 long session | 0 |
| v323 (hook permanent) | internal bounce forced for PSRAM | PSRAM | downloads during playback | 0 |

v322 heartbeat during and after four 12 MB downloads: `isoc_cb` 660-702 per
~2.67 s window (250/s nominal) throughout, `xfer_fail`/`pkt_fail`/`lost`/`bna`
unchanged, no `rate=34` window, UAC ring low water 834 frames.

v323 (hook permanent, isochronous cadence probe): 0 stop, `bna=0`, no
`[HEAP] alloc FAILED`, longest gap between completed URBs steady at ~4000 us
(one URB = 4 x 1 ms packets) with no gap of 6 ms or more, decoder runway full;
no audible crackle. Validated on hardware: the workaround is permanent.

Cost of the workaround: each 64 KB write becomes 8 x 8 KB commands; v322 SD
write time per 64 KB averaged 96-111 ms (max 487-525 ms) versus 28-33 ms
with direct DMA.

## Minimal repro plan (not yet built)

A standalone app is needed before publication, without the DJ application:

1. `CONFIG_SPIRAM=y`, `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`.
2. USB host + any UAC 1.0/2.0 full-speed sink; 3 isoc OUT URBs x 4 packets,
   resubmitted from the completion callback with silence; count completions
   per second.
3. FATFS on SDMMC; one task writes 64 KB chunks from a
   `heap_caps_aligned_alloc(64, 65536, MALLOC_CAP_SPIRAM)` buffer to a file
   in a loop; a second task reads 32 KB pages from another file into an
   aligned PSRAM buffer.
4. Expected: completions/s falls to 0 within minutes, no error reported.
   Control: same app with a `check_buffer_alignment` hook returning false
   for `esp_ptr_external_ram()` buffers; completions stay at 250/s.

Open questions for Espressif: whether concurrent SDMMC and DWC DMA to PSRAM
is supported on P4, and whether a DWC descriptor or cache-line writeback
interaction (cf. esp-idf #18235, already in v6.0.2) can stall the channel
without raising an interrupt.

## Project workaround

`firmware/main-deck-jc1060/components/bsp_jc1060p470/src/bsp_sd.c`:
`bsp_sd_check_buffer_alignment()` reports PSRAM buffers as unaligned so the
driver bounces them through an 8 KB internal DMA buffer taken at first mount.
Boot log confirmation: `no SD DMA to PSRAM (bounced via 8192 B internal)`.
