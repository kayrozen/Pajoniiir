# Pioneer DDJ-400 Controller Support for Pajoniiir

## Overview

This directory contains the MIDI Controller Interface (MIDI-CI) for the **Pioneer DDJ-400** DJ controller.

### Key Differences from DDJ-FLX4

| Feature | DDJ-400 | DDJ-FLX4 |
|---------|---------|----------|
| Hot Cues | **6** (A-F) | 8 (A-H) |
| LCD Display | ❌ No | ✅ Yes |
| Smart CFX | ❌ No | ✅ Yes |
| Pad FX | ✅ 13 effects | ✅ 14 effects |
| Loop Controls | Physical buttons | Touch pads |
| Jog Wheels | Mechanical + sensor | Platter + ring |

## Hardware Identification

- **Vendor ID**: `0x0853` (Pioneer)
- **Product ID**: `0x0504`
- **USB Class**: MIDI Class-Compliant

## Files

- `MIDI_MAP_RESEARCH.md` - Complete MIDI mapping documentation
- `ddj400_midi_ci.c` - MIDI parser implementation
- `ddj400_midi_ci.h` - Header file
- `profile.s3bin` - S3 controller profile (generated)

## Integration Status

| Component | Status | Notes |
|-----------|--------|-------|
| MIDI Parser | ✅ Implemented | Full mapping for all controls |
| S3 Profile | ⏳ Pending | Requires binary generation |
| UI Adaptation | ⏳ Pending | Hide hot cues G/H |
| Hardware Testing | ⏳ Pending | Requires physical DDJ-400 |

## Building

Add to your S3 firmware build by including in `components/CMakeLists.txt`:

```cmake
add_subdirectory(../../controllers/pioneer_ddj_400 ddj400_midi_ci)
```

## Usage Example

```c
#include "ddj400_midi_ci.h"

// Initialize
ddj400_init();

// Parse MIDI message
uint8_t midi_msg[] = {0x90, 0x0E, 0x7F}; // Deck 1 PLAY
control_link_event_t event;
esp_err_t err = ddj400_midi_parse(midi_msg, 3, &event);

if (err == ESP_OK) {
    // Forward to control_link
    control_link_send_event(&event);
}
```

## Testing

Run host tests:
```powershell
cd tests/ddj400_midi_host
.\run_ddj400_host_tests.ps1
```

## TODO

- [ ] Generate `profile.s3bin` from XML definition
- [ ] Add unit tests for all MIDI messages
- [ ] Validate with physical hardware
- [ ] UI adaptation for 6 hot cues
- [ ] Document Pad FX assignments

## References

- [Pioneer DDJ-400 Official Page](https://www.pioneerdj.com/en/product/controllers/ddj-400/)
- [Mixxx Controller Mapping](https://github.com/mixxxdj/mixxx/tree/master/resources/controllers)
- [MIDI Implementation Chart](https://www.pioneerdj.com/en/support/article/7e31e450-155f-487d-89e8-e2682f4e04cf/)
