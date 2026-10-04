# WiFi enable → disable : Core 0 panic reproducible (v321 + v323)

Local incident note — NOT published. Recorded 2026-10-02 (v323 era) for a later
dedicated fix version. Do not treat as an accepted design; this is a defect log.

## Reproduction

1. Boot normally (Ethernet/DJ Link fine).
2. Settings → enable WiFi, then disable it again shortly after.
3. Panic within ~1 s of the disable, **both times on v321 and v323**
   (ELF SHA256 `7f507fac8` and `da599e7c4` — build-independent).

## Serial evidence (identical sequence both times)

```
W httpd_uri: uri handler execution failed            (several, during disable)
E dns_server: recvfrom failed: errno 128             (captive portal DNS socket dead)
D esp_eth.netif.netif_glue: Del filter MAC: 01:00:5e:00:00:fb
E SD_HOST: sd_host_del_sdmmc_controller(353): host controller with slot registered
Guru Meditation Error: Core 0 panic'ed (Load access fault). Exception was unhandled.
  MCAUSE 0x00000005  MTVAL 0x0000009c                (load from NULL+offset, small struct member)
  MEPC/RA 0x4818xxxx (flash/XIP region), backtrace only 2 frames deep
  Backtrace: 0x48183504:0x4ff22f70 0x4ff002d4:0x00000000   (2nd: 0x4818394a)
Rebooting...  rst:0xc (SW_CPU_RESET)
```

Reset reason 4 on the following boot.

## Reading

- The failing step is the WiFi-off teardown path. The P4 WiFi is via
  ESP-Hosted over SDIO: `sd_host_del_sdmmc_controller` runs as part of it, and
  the error line already says "host controller with slot registered" — the
  teardown proceeds against a controller that still owns a slot, then someone
  dereferences a NULL handle (`MTVAL 0x9c` = small member load off NULL).
- Suspicion ranking: (1) our own SD card slot shares the same SDMMC host
  object and a teardown/pointer is freed or nulled while the SD card driver
  still uses it; (2) ESP-Hosted wifi-stop bug in the v6.0.2 tree; (3) our
  wifi_remote settings path tearing down in the wrong order (httpd → dns →
  eth filter removal all appear right before, so ordering of teardowns is
  suspect overall).
- Load-access fault (not store) → a driver state struct was freed/zeroed,
  a later read of `struct->member` (offset 0x9c) faults.

## Not yet done

- Symbolized backtrace (addr2line against the matching ELF — the two MEPCs
  0x48183504 / 0x4818394a + RA 0x4ff002d4).
- Verify whether the SD card mount survives on the panicking boot path
  (it did remount fine after reboot; the panic is during teardown only).
- Decide fix: guard the disable path (defer/serialize SD vs host teardown),
  or keep controller with slot and only stop the interface.

## Web findings (2026-10-02) — this is a KNOWN ESP-Hosted bug family

- esp_hosted changelog **v2.12.11**: "fixed SDIO deinit tearing down the shared
  SDMMC host (broke a co-existing SD card on the other slot)" — our exact
  symptom class (wifi OFF kills the co-existing SD card's host controller).
- esp_hosted **v3.0.4**: forbids enabling esp_hosted alongside native host
  Wi-Fi (shared SDIO bus); the SDIO transport/teardown rework happened in 3.x.
- Related open issues: espressif/esp-hosted-mcu #121 (P4/C6 unrecoverable host
  SDIO state, host Load-access-fault panic, "known case, host should ignore
  hosted and continue"), #167 (EHM-188), #197 (EHM-220, SDIO wedges under
  outbound TCP load).
- Our tree: main/idf_component.yml pins `espressif/esp_hosted ==2.12.12`,
  managed_components resolved shows version: 2.12.13 — so the 2.12.11 fix
  SHOULD be in the tree, yet we still crash → either our config hits a
  different path (our SD card slot registered on the SAME host controller
  esp_hosted deletes; the error "host controller with slot registered" proves
  the delete proceeds anyway), or a regression, or the pin mismatch means the
  fix is not actually linked.
- Next steps: confirm which esp_hosted revision is actually linked (grep
  build dir / CMake cache), read the 2.12.11 fix in managed_components
  (esp_wifi_remote/sd deinit path), test a bump to the newest 2.12.x/3.x on a
  throwaway build, or guard our side (serialize our SD slot vs hosted
  teardown; refuse the controller delete while our slot is registered).

## Separate finding: web UI unreachable via Ethernet (apparent boot race)

- app_main.c:657 starts the web server on Ethernet only if
  eth_bringup_got_ip() is already true when app_main runs that branch.
- DHCP completes at ~6-7 s; app_main reaches the branch at ~2 s → check
  false → web_server_start() never called → port 80 closed.
- Verified 2026-10-02 (v323 boot, IP 192.168.100.124): ping OK (0.4 ms),
  TCP/80 CLOSED, no `web UI available` line anywhere in serial_all.log.
- Second constraint by design: api_request_allowed() (web_server.c:270)
  only accepts Host pajoniiir.local or the AP address 192.168.4.1
  (DNS-rebinding guard) — accessing API via the eth IP would 403 even
  after the race is fixed. Fix should (a) defer web_server_start until
  the got-ip event, (b) add the eth IP to accepted hosts (or rely on
  pajoniiir.local over mDNS, which is already advertised on _http._tcp).
- If wifi_remote setting is ON, the server runs on the SoftAP portal
  instead, so this only shows when wifi_remote is OFF.
- Fixed in v330 (not yet HIL-checked): the server starts at boot without
  waiting for the lease (httpd on all interfaces, mDNS ETH netif enables on
  got-IP), and the host allow-list accepts the current Ethernet IP. See
  `docs/DEVELOPMENT_PLAN.md` v330. Turning the Wi-Fi remote OFF still stops
  the server (wifi_link owns that stop; same path as the panic above).

## Workaround for now

Avoid toggling WiFi OFF in a session you care about; a full power cycle
recovers it. WiFi OFF is the only known trigger (enable alone is fine).
