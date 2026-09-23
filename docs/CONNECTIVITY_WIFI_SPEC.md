# Connectivity Spec — Wi-Fi / Remote Access (JC1060)

Status: DRAFT v1. Target: `firmware/main-deck-jc1060` on branch
`codex/ddj400-jc1060-integration`. Companion to
`docs/AUDIO_ENGINE_USB_SPEC.md`. Evidence base: `upstream/master`
(`feat/p4-dual-usb-host` merged) for the Wi-Fi architecture, plus this
project's own JC1060 bench history for board-specific reality.

## 1. What the original does (upstream/master)

Radio: the onboard **ESP32-C6** over **ESP-Hosted SDIO** (slot 1, 4-bit:
CLK18/CMD19/D0-D3 14-17, C6 CHIP_PU GPIO54). P4 runs the Wi-Fi API as a
remote shim; the C6 is the radio.

Components:

| Component | Content |
| --- | --- |
| `wifi_link` | Single owner of the radio lifecycle: `esp_hosted_init` → `esp_wifi_init` → SoftAP **`Pajoniiir`** (WPA2/WPA3-transition, password `Pajoniiir`) + web server + captive DNS. Async `wifi_link_request_enable()` so UI never blocks on the ~1-2 s SDIO bring-up; rapid toggles collapse to the latest request. `esp_wifi_set_ps(WIFI_PS_NONE)`. |
| `wifi_link_retry` | Separated retry policy. Never reconnect from inside the DISCONNECTED handler or the boot path; ~3 s backoff; scans on throwaway tasks. (These rules came from bench scars — hanging RPCs stall the whole event loop.) |
| `wifi_transition_lease` | Shared lease serializing radio transitions so a pull-OTA worker cannot have the SDIO transport deinit'd under it (the `sdio_drv` assert signature). |
| `web_server` | httpd mobile controller + captive DNS (`dns_server`/`dns_reply`), lifetime OWNED by wifi_link (starts only after AP init succeeds, fully torn down on stop). Static local web assets only — no network references (captive DNS blackholes them). |
| STA "visit" mode | `wifi_link_switch_to_sta()` (blocking, on the wifi worker, returns only when an IP is assigned) → pull OTA → `wifi_link_restore_ap()`. The AP is the guaranteed service surface; STA is a visit, never a state to live in. `wifi_link_probe_start()` is the one-shot round-trip proof used before building download code on top. |

sdkconfig (upstream defaults, IDF 6):

```
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y
CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y     # internal RAM dies otherwise
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=<board-pinned>
CONFIG_WIFI_RMT_ENABLE_WPA3_SAE=y             # authoritative symbols are WIFI_RMT_* on ESP-Hosted
CONFIG_WIFI_RMT_SOFTAP_SAE_SUPPORT=y
CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=16 / DYNAMIC_RX=64 / DYNAMIC_TX=64
CONFIG_WIFI_RMT_AMPDU_TX=y TX_BA_WIN=32 / AMPDU_RX=y RX_BA_WIN=32
```

Wi-Fi is user-toggled in Settings (persisted NVS `wifi_remote`, default OFF)
and re-applied at boot only when saved ON.

## 2. What is different on the JC1060 (board reality)

The JC1060P470C carries the same C6 on the same SDIO pins, so the upstream
architecture ports directly — but two bench facts change the plan:

1. **Wi-Fi is PARKED on this board.** The C6 runs the vendor ESPHome build
   (esp-hosted slave 2.12.12, flashed via EspControl browser recovery). The
   IDF-6 registry only offers host esp_hosted 2.12.13; that host/slave pairing
   hangs in scan/connect RPCs. EspControl (host 2.12.12, IDF 5.5) connects to
   an AP with this exact C6, so the radio and SDIO wiring are fine — the
   pairing is the problem. Required-with-that-slave host options
   (`CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y`,
   `CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS=8`) are already identified and
   the full investigation lives in the skill reference
   `esp-hosted-c6-wifi.md`. **SDIO clock is user-pinned at 40 MHz (20 MHz
   boot-loops) — never change it.**
2. **The board has Ethernet the original does not use.** IP101 RMII
   (MDC=31, MDIO=52, REF_CLK 50 MHz in on GPIO50, phy_addr=1, RESET_N=GPIO51
   handed to the driver). Ethernet + DHCP + TCP debug console (2333) +
   **OTA pull over Ethernet are already proven end to end on this board** —
   it is currently the working no-serial flash path.

Decision framework, therefore:

| Path | State | Role |
| --- | --- | --- |
| **Ethernet (IP101)** | Proven on this board | Primary remote/OTA/diagnostic transport for the JC1060 product. Deterministic, no co-proc firmware dependency, no RF cost next to USB iso audio. |
| **Wi-Fi SoftAP (C6/ESP-Hosted)** | Parked (host/slave version mismatch) | Port the upstream `wifi_link`/`web_server` architecture unchanged, gated behind a behind-a-flag bring-up; un-park only when the pairing question is resolved (option A below). |

This inverts the original's priority (Wi-Fi primary) but for a hardware
reason the original does not have: our bench has a working wired path and a
non-working radio pairing. Do not delete the Wi-Fi port effort — the web UI,
OTA pull and captive DNS layers are transport-agnostic above netif and are
shared by both.

## 3. Spec by layer

### 3.1 Transport-agnostic layers (port from upstream as-is)

- `web_server` + captive DNS: start/stop owned by whichever transport wins;
  asset rules unchanged (local fonts/stacks only, `max_open_sockets=5`
  lesson — enable `lru_purge_enable` or cap keep-alive).
- OTA pull client (`p4_ota_pull`, `p4_ota_pull_core`): unchanged; its "host"
  registration already hooks the TCP-console accept path, which works over
  Ethernet today.
- `/api/status` diagnostics surface: add an `eth` object (link, IP) beside
  the upstream `controller`/`wifi` objects.

### 3.2 Ethernet (new for this target, proven on bench)

- Bring-up stays defaults-only (the documented scar: any manual EMAC config
  or pre-start MDIO scan starves/blocks the stack and shows as LVGL flicker).
  `phy_config.reset_gpio_num = GPIO51`, `phy_addr = 1`.
- A small `eth_link` component owns lifecycle symmetric to `wifi_link`:
  init/start/stop, status snapshot (link up, IP), event-loop registration
  before handler registration, tolerate `ESP_ERR_INVALID_STATE` on re-init.
- TCP log console (2333) and heartbeat line (`[alive] uptime=… ETH x.x.x.x`)
  carry over unchanged — they are the accepted diagnostic surface.

### 3.3 Wi-Fi (ported upstream, parked until unblocked)

Port `wifi_link` + `wifi_link_retry` + `wifi_transition_lease` verbatim in
architecture (single owner, async request_enable, AP-as-home/STA-as-visit,
lease-serialized transitions, `WIFI_PS_NONE`, WIFI_RMT_* symbols, SPIRAM
mempool). JC1060 deltas:

- sdkconfig: keep the user-pinned `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000`;
  add the ESPHome-slave peer-data options only when actually pairing with
  that slave build.
- Compile-gate the whole component behind a Kconfig flag default OFF on this
  target so the parked state is explicit, not silent.
- Un-park options (in order of preference):
  - **A. Re-flash the C6 slave** to a build matching host 2.12.13 (or vice
    versa downgrade host to 2.12.12 on an IDF 5.5 side-branch — rejected for
    this tree, IDF 6 is pinned). C6 UART + CHIP_PU + IO9 are on the 2x10
    expansion header (continuity-check first). Note: EspControl flashing
    replaces the P4 app too — reflash Pajoniiir after.
  - **B. Accept the ESPHome slave** with peer-data handlers enabled and
    retest scan/connect (the missing-handlers symptom is `Dropping
    packet(s) from stream` + RPCs that never return — cheap to test first).
- Settings toggle, NVS persistence, boot re-apply: same as upstream
  (`wifi_remote`, default OFF).

### 3.4 RF/real-time interaction rules (carried from bench history)

- Wi-Fi and audio: ESP-Hosted SDIO DMA + USB iso + DSI all contend; keep
  mempool in PSRAM and validate any radio-enabled soak with the audio
  underrun counters (the "clean when X is off" correlation is not trusted —
  re-run the baseline after changes).
- Never `esp_wifi_stop()` to "test RF interference" and conclude from the
  result: the STA handler reconnects behind the gate unless the owner flag
  stops it (documented trap).

## 4. Risks

| # | Risk | Mitigation |
| --- | --- | --- |
| R1 | Host/slave esp_hosted pairing never resolves | Ethernet remains the product path; Wi-Fi stays flagged-off, feature is not blocked on it |
| R2 | SDIO DMA (radio) vs USB iso deadline | SPIRAM mempool, short SDIO bursts (existing ~1 KB CMD53 cap guidance), audio-soak validation on enable |
| R3 | Porting `web_server` before a transport exists | Gate startup on netif-ready (either eth or wifi), as upstream gates on AP success |
| R4 | Captive-DNS + Ethernet confusion (captive DNS makes sense on the AP only) | DNS server starts only with the SoftAP, same as upstream |
| R5 | Two lifecycle owners fight (eth_link vs wifi_link both starting web_server) | Single `net_service` arbitration: web_server/captive DNS start when the FIRST netif is up; wifi wins the captive-DNS role, eth wins the default route role; document precedence in one place |

## 5. Milestones

| Gate | Content |
| --- | --- |
| W0 | `eth_link` component: defaults-only bring-up, DHCP, heartbeat shows IP (re-proof after refactor) |
| W1 | `web_server` + `/api/*` served over Ethernet; OTA pull over Ethernet re-verified |
| W2 | Wi-Fi components ported and compiling behind the OFF flag; no runtime footprint when off |
| W3 | Wi-Fi un-park attempt (option B then A): probe round-trip `wifi_link_probe_start()` OK on hardware |
| W4 | AP mode + web UI + captive DNS over Wi-Fi; toggle/NVS/boot-apply behavior matches upstream |
| W5 | Combined soak: audio playing + chosen transport active, 30 min, underrun/late counters clean |

W3-W5 are parked behind the pairing resolution; W0-W2 are on the critical
path for the JC1060 product.
