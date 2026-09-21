#include "wifi_component.h"
#ifdef USE_WIFI
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <type_traits>

#ifdef USE_ESP32
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
#include <esp_eap_client.h>
#else
#include <esp_wpa2.h>
#endif
#endif

#if defined(USE_ESP32)
#include <esp_wifi.h>
#endif
#ifdef USE_ESP8266
#include <user_interface.h>
#endif

#include <algorithm>
#include <new>
#include <utility>
#include "lwip/dns.h"
#include "lwip/err.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/string_ref.h"
#include "esphome/core/util.h"

#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

#ifdef USE_IMPROV_BLE
#include "esphome/components/improv_ble/improv_ble_component.h"
#endif

#ifdef USE_IMPROV_SERIAL
#include "esphome/components/improv_serial/improv_serial_component.h"
#endif

#ifdef USE_PROVISIONING
#include "esphome/components/provisioning/provisioning.h"
#endif

namespace esphome::wifi {

static const char *const TAG = "wifi";

// CompactString implementation
CompactString::CompactString(const char *str, size_t len) {
  if (len > MAX_LENGTH) {
    len = MAX_LENGTH;  // Clamp to max valid length
  }

  this->length_ = len;
  if (len <= INLINE_CAPACITY) {
    // Store inline with null terminator
    this->is_heap_ = 0;
    if (len > 0) {
      std::memcpy(this->storage_, str, len);
    }
    this->storage_[len] = '\0';
  } else {
    // Heap allocate with null terminator
    this->is_heap_ = 1;
    char *heap_data = new char[len + 1];  // NOLINT(cppcoreguidelines-owning-memory)
    std::memcpy(heap_data, str, len);
    heap_data[len] = '\0';
    this->set_heap_ptr_(heap_data);
  }
}

CompactString::CompactString(const CompactString &other) : CompactString(other.data(), other.size()) {}

CompactString &CompactString::operator=(const CompactString &other) {
  if (this != &other) {
    this->~CompactString();
    new (this) CompactString(other);
  }
  return *this;
}

CompactString::CompactString(CompactString &&other) noexcept : length_(other.length_), is_heap_(other.is_heap_) {
  // Copy full storage (includes null terminator for inline, or pointer for heap)
  std::memcpy(this->storage_, other.storage_, INLINE_CAPACITY + 1);
  other.length_ = 0;
  other.is_heap_ = 0;
  other.storage_[0] = '\0';
}

CompactString &CompactString::operator=(CompactString &&other) noexcept {
  if (this != &other) {
    this->~CompactString();
    new (this) CompactString(std::move(other));
  }
  return *this;
}

CompactString::~CompactString() {
  if (this->is_heap_) {
    delete[] this->get_heap_ptr_();  // NOLINT(cppcoreguidelines-owning-memory)
  }
}

bool CompactString::operator==(const CompactString &other) const {
  return this->size() == other.size() && std::memcmp(this->data(), other.data(), this->size()) == 0;
}
bool CompactString::operator==(const StringRef &other) const {
  return this->size() == other.size() && std::memcmp(this->data(), other.c_str(), this->size()) == 0;
}

/// WiFi Retry Logic - Priority-Based BSSID Selection
///
/// The WiFi component uses a state machine with priority degradation to handle connection failures
/// and automatically cycle through different BSSIDs in mesh networks or multiple configured networks.
///
/// Normal Connection Flow (SCAN_BASED):
/// ┌──────────────────────────────────────────────────────────────────────┐
/// │                      Fast Connect Path (Optional)                    │
/// ├──────────────────────────────────────────────────────────────────────┤
/// │  Entered if: configuration has 'fast_connect: true'                  │
/// │  Optimization to skip scanning when possible:                        │
/// │                                                                      │
/// │  1. INITIAL_CONNECT → Try one of:                                    │
/// │     a) Saved BSSID+channel (from previous boot)                      │
/// │     b) First configured non-hidden network (any BSSID)               │
/// │                          ↓                                           │
/// │     [FAILED] → Check if more configured networks available           │
/// │                          ↓                                           │
/// │  2. FAST_CONNECT_CYCLING_APS → Try remaining configured networks     │
/// │                                (1 attempt each, any BSSID)           │
/// │                          ↓                                           │
/// │     [All Failed] → Fall through to explicit hidden or scanning       │
/// │                                                                      │
/// │  Note: Fast connect data saved from previous successful connection   │
/// └──────────────────────────────────────────────────────────────────────┘
///                          ↓
/// ┌──────────────────────────────────────────────────────────────────────┐
/// │              Explicit Hidden Networks Path (Optional)                │
/// ├──────────────────────────────────────────────────────────────────────┤
/// │  Entered if: first configured network has 'hidden: true'             │
/// │                                                                      │
/// │  1. EXPLICIT_HIDDEN → Try consecutive hidden networks (1 attempt)    │
/// │                       Stop when visible network reached              │
/// │                          ↓                                           │
/// │     Example: Hidden1, Hidden2, Visible1, Hidden3, Visible2           │
/// │              Try: Hidden1, Hidden2 (stop at Visible1)                │
/// │                          ↓                                           │
/// │     [All Failed] → Fall back to scan-based connection                │
/// │                                                                      │
/// │  Note: Fast connect saves BSSID after first successful connection,   │
/// │        so subsequent boots use fast path instead of hidden mode      │
/// └──────────────────────────────────────────────────────────────────────┘
///                          ↓
/// ┌──────────────────────────────────────────────────────────────────────┐
/// │                    Scan-Based Connection Path                        │
/// ├──────────────────────────────────────────────────────────────────────┤
/// │                                                                      │
/// │  1. SCAN → Sort by priority (highest first), then RSSI               │
/// │     ┌─────────────────────────────────────────────────┐              │
/// │     │ scan_result_[0] = Best BSSID (highest priority) │              │
/// │     │ scan_result_[1] = Second best                   │              │
/// │     │ scan_result_[2] = Third best                    │              │
/// │     └─────────────────────────────────────────────────┘              │
/// │                          ↓                                           │
/// │  2. SCAN_CONNECTING → Try scan_result_[0] (2 attempts)               │
/// │                       (Visible1, Visible2 from example above)        │
/// │                          ↓                                           │
/// │  3. FAILED → Decrease priority: 0.0 → -1.0 → -2.0                    │
/// │              (stored in persistent sta_priorities_)                  │
/// │                          ↓                                           │
/// │  4. Check for hidden networks:                                       │
/// │     - If found → RETRY_HIDDEN (try SSIDs not in scan, 1 attempt)     │
/// │       Skip hidden networks before first visible one                  │
/// │       (Skip Hidden1/Hidden2, try Hidden3 from example)               │
/// │     - If none → Skip RETRY_HIDDEN, go to step 5                      │
/// │                          ↓                                           │
/// │  5. FAILED → RESTARTING_ADAPTER                                      │
/// │     - Normal: restart adapter, clear state                           │
/// │     - AP/improv active: skip restart, just disconnect                │
/// │                          ↓                                           │
/// │  6. Loop back to start:                                              │
/// │     - If first network is hidden → EXPLICIT_HIDDEN (retry cycle)     │
/// │     - If AP/improv active → RETRY_HIDDEN (blind retry, see below)    │
/// │     - Otherwise → SCAN_CONNECTING (rescan)                           │
/// │                          ↓                                           │
/// │  7. RESCAN → Apply stored priorities, sort again                     │
/// │     ┌─────────────────────────────────────────────────┐              │
/// │     │ scan_result_[0] = BSSID B (priority 0.0)  ← NEW │              │
/// │     │ scan_result_[1] = BSSID C (priority 0.0)        │              │
/// │     │ scan_result_[2] = BSSID A (priority -2.0) ← OLD │              │
/// │     └─────────────────────────────────────────────────┘              │
/// │                          ↓                                           │
/// │  8. SCAN_CONNECTING → Try scan_result_[0] (next best)                │
/// │                                                                      │
/// │  Key: Priority system cycles through BSSIDs ACROSS scan cycles       │
/// │       Full retry cycle: EXPLICIT_HIDDEN → SCAN → RETRY_HIDDEN        │
/// │       Always try best available BSSID (scan_result_[0])              │
/// └──────────────────────────────────────────────────────────────────────┘
///
/// Retry Phases:
/// - INITIAL_CONNECT: Try saved BSSID+channel (fast_connect), or fall back to normal flow
/// - FAST_CONNECT_CYCLING_APS: Cycle through remaining configured networks (1 attempt each, fast_connect only)
/// - EXPLICIT_HIDDEN: Try consecutive networks marked hidden:true before scanning (1 attempt per SSID)
/// - SCAN_CONNECTING: Connect using scan results (2 attempts per BSSID)
/// - RETRY_HIDDEN: Behavior controlled by RetryHiddenMode:
///   * SCAN_BASED: Try networks not found in scan (truly hidden, 1 attempt per SSID)
///   * BLIND_RETRY: Cycle through ALL networks when scanning disabled (AP active)
/// - RESTARTING_ADAPTER: Restart WiFi adapter to clear stuck state (restart skipped if AP active)
///
/// Hidden Network Handling:
/// - Networks marked 'hidden: true' before first non-hidden → Tried in EXPLICIT_HIDDEN phase
/// - Networks marked 'hidden: true' after first non-hidden → Tried in RETRY_HIDDEN phase
/// - After successful connection, fast_connect saves BSSID → subsequent boots use fast path
/// - Networks not in scan results → Tried in RETRY_HIDDEN phase
/// - Networks visible in scan + not marked hidden → Skipped in RETRY_HIDDEN phase
/// - Networks marked 'hidden: true' always use hidden mode, even if broadcasting SSID
///
/// ┌──────────────────────────────────────────────────────────────────────┐
/// │        Captive Portal / Improv Mode (AP active, scanning disabled)   │
/// ├──────────────────────────────────────────────────────────────────────┤
/// │  When captive_portal or improv_ble is active, WiFi scanning is       │
/// │  disabled because it disrupts AP clients (radio leaves AP channel    │
/// │  to hop through other channels, causing client disconnections).      │
/// │                                                                      │
/// │  Flow with RetryHiddenMode::BLIND_RETRY:                             │
/// │                                                                      │
/// │  1. RESTARTING_ADAPTER → In this mode, skip adapter restart and      │
/// │     just disconnect (normal mode restarts the adapter)               │
/// │     - Sets retry_hidden_mode_ = BLIND_RETRY                          │
/// │     - Enter extended cooldown (30s vs normal 500ms)                  │
/// │                          ↓                                           │
/// │  2. determine_next_phase_() returns RETRY_HIDDEN (skips scanning)    │
/// │                          ↓                                           │
/// │  3. RETRY_HIDDEN with BLIND_RETRY mode:                              │
/// │     - find_next_hidden_sta_() ignores scan_result_                   │
/// │     - ALL configured networks become candidates                      │
/// │     - Cycles through networks: Net1 → Net2 → Net3 → ...              │
/// │                          ↓                                           │
/// │  4. After exhausting all networks → Back to RESTARTING_ADAPTER       │
/// │     - Loop continues until connection succeeds or user configures    │
/// │       new credentials via captive portal                             │
/// │                                                                      │
/// │  The 30s cooldown gives users time to interact with captive portal   │
/// │  without constant connection attempts disrupting the AP.             │
/// └──────────────────────────────────────────────────────────────────────┘
///
/// ┌──────────────────────────────────────────────────────────────────────┐
/// │              Post-Connect Roaming (for stationary devices)           │
/// ├──────────────────────────────────────────────────────────────────────┤
/// │  Purpose: Handle AP reboot or power loss scenarios where device      │
/// │           connects to suboptimal AP and never switches back          │
/// │                                                                      │
/// │  State Machine (RoamingState):                                       │
/// │                                                                      │
/// │    ┌─────────────────────────────────────────────────────────────┐   │
/// │    │                         IDLE                                │   │
/// │    │  (waiting for 5 min timer, attempts < 3)                    │   │
/// │    └─────────────────────────┬───────────────────────────────────┘   │
/// │                              │ 5 min elapsed, RSSI < -49 dBm         │
/// │                              ↓                                       │
/// │    ┌─────────────────────────────────────────────────────────────┐   │
/// │    │                       SCANNING                              │   │
/// │    │  (attempts++ in check_roaming_ before entering this state)  │   │
/// │    └─────────────────────────┬───────────────────────────────────┘   │
/// │                              │                                       │
/// │               ┌──────────────┼──────────────┐                        │
/// │               ↓              ↓              ↓                        │
/// │         disconnect    no better AP    +10 dB better AP               │
/// │               │              │              │                        │
/// │               ↓              ↓              ↓                        │
/// │    ┌──────────────────────────────┐  ┌──────────────────────────┐    │
/// │    │  → RECONNECTING              │  │        CONNECTING        │    │
/// │    │  (counter preserved)         │  │  (process_roaming_scan_) │    │
/// │    └──────────────────────────────┘  └────────────┬─────────────┘    │
/// │                                                  │                   │
/// │                              ┌───────────────────┴───────────────┐   │
/// │                              ↓                                   ↓   │
/// │                        SUCCESS                              FAILED   │
/// │                              │                                   │   │
/// │                              ↓                                   ↓   │
/// │    ┌──────────────────────────────────┐    ┌─────────────────────────┐
/// │    │  → IDLE                          │    │      RECONNECTING       │
/// │    │  (counter reset to 0)            │    │  (retry_connect called) │
/// │    └──────────────────────────────────┘    └───────────┬─────────────┘
/// │                                                        │             │
/// │                                              ┌─────────┴─────────┐   │
/// │                                              ↓                   ↓   │
/// │                                      on target BSSID    on other AP  │
/// │                                              │                   │   │
/// │                                              ↓                   ↓   │
/// │                                   ┌──────────────────┐ ┌────────────┐│
/// │                                   │  → IDLE          │ │  → IDLE    ││
/// │                                   │  (counter reset) │ │  (counter  ││
/// │                                   │  (roam worked!)  │ │  preserved)││
/// │                                   └──────────────────┘ └────────────┘│
/// │                                                                      │
/// │  Key behaviors:                                                      │
/// │  - After 3 checks: attempts >= 3, stop checking                      │
/// │  - Non-roaming disconnect: clear_roaming_state_() resets counter     │
/// │  - Disconnect during scan (SCANNING→RECONNECTING): counter preserved │
/// │  - Disconnect after scan (within grace period): counter preserved    │
/// │  - Roaming success (CONNECTING→IDLE): counter reset (can roam again) │
/// │  - Roaming success via retry (on target BSSID): counter reset        │
/// │  - Roaming fail (RECONNECTING on other AP): counter preserved        │
/// └──────────────────────────────────────────────────────────────────────┘

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_INFO
#ifdef USE_WIFI_PHY_MODE
// Use if-chain instead of switch to avoid jump table in RODATA (wastes RAM on ESP8266)
static const LogString *phy_mode_to_log_string(WiFi8266PhyMode mode) {
  if (mode == WIFI_8266_PHY_MODE_11B)
    return LOG_STR("11B");
  if (mode == WIFI_8266_PHY_MODE_11G)
    return LOG_STR("11G");
  if (mode == WIFI_8266_PHY_MODE_11N)
    return LOG_STR("11N");
  return LOG_STR("Auto");
}
#endif
// Use if-chain instead of switch to avoid jump table in RODATA (wastes RAM on ESP8266)
static const LogString *retry_phase_to_log_string(WiFiRetryPhase phase) {
  if (phase == WiFiRetryPhase::INITIAL_CONNECT)
    return LOG_STR("INITIAL_CONNECT");
#ifdef USE_WIFI_FAST_CONNECT
  if (phase == WiFiRetryPhase::FAST_CONNECT_CYCLING_APS)
    return LOG_STR("FAST_CONNECT_CYCLING");
#endif
  if (phase == WiFiRetryPhase::EXPLICIT_HIDDEN)
    return LOG_STR("EXPLICIT_HIDDEN");
  if (phase == WiFiRetryPhase::SCAN_CONNECTING)
    return LOG_STR("SCAN_CONNECTING");
  if (phase == WiFiRetryPhase::RETRY_HIDDEN)
    return LOG_STR("RETRY_HIDDEN");
  if (phase == WiFiRetryPhase::RESTARTING_ADAPTER)
    return LOG_STR("RESTARTING");
  return LOG_STR("UNKNOWN");
}
#endif  // ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_INFO

bool WiFiComponent::went_through_explicit_hidden_phase_() const {
  // If first configured network is marked hidden, we went through EXPLICIT_HIDDEN phase
  // This means those networks were already tried and should be skipped in RETRY_HIDDEN
  return !this->sta_.empty() && this->sta_[0].get_hidden();
}

int8_t WiFiComponent::find_first_non_hidden_index_() const {
  // Find the first network that is NOT marked hidden:true
  // This is where EXPLICIT_HIDDEN phase would have stopped
  for (size_t i = 0; i < this->sta_.size(); i++) {
    if (!this->sta_[i].get_hidden()) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;  // All networks are hidden
}

// 2 attempts per BSSID in SCAN_CONNECTING phase
// Rationale: This is the ONLY phase where we decrease BSSID priority, so we must be very sure.
// Auth failures are common immediately after scan due to WiFi stack state transitions.
// Trying twice filters out false positives and prevents unnecessarily marking a good BSSID as bad.
// After 2 genuine failures, priority degradation ensures we skip this BSSID on subsequent scans.
static constexpr uint8_t WIFI_RETRY_COUNT_PER_BSSID = 2;

// 1 attempt per SSID in RETRY_HIDDEN phase
// Rationale: Try hidden mode once, then rescan to get next best BSSID via priority system
static constexpr uint8_t WIFI_RETRY_COUNT_PER_SSID = 1;

// 1 attempt per AP in fast_connect mode (INITIAL_CONNECT and FAST_CONNECT_CYCLING_APS)
// Rationale: Fast connect prioritizes speed - try each AP once to find a working one quickly
static constexpr uint8_t WIFI_RETRY_COUNT_PER_AP = 1;

/// Cooldown duration in milliseconds after adapter restart or repeated failures
/// Allows WiFi hardware to stabilize before next connection attempt
static constexpr uint32_t WIFI_COOLDOWN_DURATION_MS = 500;

/// Cooldown duration when fallback AP is active and captive portal may be running
/// Longer interval gives users time to configure WiFi without constant connection attempts
/// While connecting, WiFi can't beacon the AP properly, so needs longer cooldown
static constexpr uint32_t WIFI_COOLDOWN_WITH_AP_ACTIVE_MS = 30000;

/// Timeout for WiFi scan operations
/// This is a fallback in case we don't receive a scan done callback from the WiFi driver.
/// Normal scans complete via callback; this only triggers if something goes wrong.
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 31000;

/// Timeout for WiFi connection attempts
/// This is a fallback in case we don't receive connection success/failure callbacks.
/// Some platforms (especially LibreTiny/Beken) can take 30-60 seconds to connect,
/// particularly with fast_connect enabled where no prior scan provides channel info.
/// Do not lower this value - connection failures are detected via callbacks, not timeout.
/// If this timeout fires prematurely while a connection is still in progress, it causes
/// cascading failures: the subsequent scan will also fail because the WiFi driver is
/// still busy with the previous connection attempt.
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 46000;

static constexpr uint8_t get_max_retries_for_phase(WiFiRetryPhase phase) {
  switch (phase) {
    case WiFiRetryPhase::INITIAL_CONNECT:
#ifdef USE_WIFI_FAST_CONNECT
    case WiFiRetryPhase::FAST_CONNECT_CYCLING_APS:
#endif
      // INITIAL_CONNECT and FAST_CONNECT_CYCLING_APS both use 1 attempt per AP (fast_connect mode)
      return WIFI_RETRY_COUNT_PER_AP;
    case WiFiRetryPhase::EXPLICIT_HIDDEN:
      // Explicitly hidden network: 1 attempt (user marked as hidden, try once then scan)
      return WIFI_RETRY_COUNT_PER_SSID;

[... middle omitted — see footer ...]

    return false;
#else
  // If PSK given, only match for networks with auth (and vice versa)
  if (config.password_.empty() == this->with_auth_)
    return false;
#endif

  // If channel configured, only match networks on that channel.
  if (config.has_channel() && config.get_channel() != this->channel_) {
    return false;
  }
  return true;
}
bool WiFiScanResult::operator==(const WiFiScanResult &rhs) const { return this->bssid_ == rhs.bssid_; }

void WiFiComponent::clear_roaming_state_() {
  this->roaming_attempts_ = 0;
  this->roaming_last_check_ = 0;
  this->roaming_scan_end_ = 0;
  this->roaming_target_bssid_ = {};
  this->roaming_state_ = RoamingState::IDLE;
}

#ifdef USE_ESP32
void WiFiComponent::handle_driver_roam_(const bssid_t &bssid, uint8_t channel) {
  // A driver-initiated roam (e.g. 802.11v BTM) re-associates without the state
  // machine ever leaving STA_CONNECTED, so check_connecting_finished() never runs.
  // Redo its post-connect bookkeeping here. roaming_state_ is deliberately left
  // untouched so an in-flight roaming scan is not orphaned. The BSSID and
  // channel both come from the connected event so the saved pair is consistent:
  // the radio may be off-channel during a roaming scan, and a later queued
  // event may have moved the driver on again by the time this one is processed.
  this->roaming_last_check_ = App.get_loop_component_start_time();
  this->roaming_attempts_ = 0;
  this->roaming_scan_end_ = 0;
  this->clear_all_bssid_priorities_();
#ifdef USE_WIFI_FAST_CONNECT
  this->save_fast_connect_settings_(bssid, channel);
#endif
}
#endif

void WiFiComponent::release_scan_results_() {
  if (!this->keep_scan_results_) {
    ScanResultsLock lock(this);
#if defined(USE_RP2)
    // std::vector - use swap trick since shrink_to_fit is non-binding
    decltype(this->scan_result_)().swap(this->scan_result_);
#else
    // FixedVector::release() frees all memory
    this->scan_result_.release();
#endif
  }
}

#ifdef USE_WIFI_CONNECT_STATE_LISTENERS
void WiFiComponent::notify_connect_state_listeners_() {
  if (!this->pending_.connect_state)
    return;
  this->pending_.connect_state = false;
  // Get current SSID and BSSID from the WiFi driver
  char ssid_buf[SSID_BUFFER_SIZE];
  const char *ssid = this->wifi_ssid_to(ssid_buf);
  bssid_t bssid = this->wifi_bssid();
  for (auto *listener : this->connect_state_listeners_) {
    listener->on_wifi_connect_state(StringRef(ssid, strlen(ssid)), bssid);
  }
}

void WiFiComponent::notify_disconnect_state_listeners_() {
  constexpr uint8_t empty_bssid[6] = {};
  for (auto *listener : this->connect_state_listeners_) {
    listener->on_wifi_connect_state(StringRef(), empty_bssid);
  }
}
#endif  // USE_WIFI_CONNECT_STATE_LISTENERS

#ifdef USE_WIFI_IP_STATE_LISTENERS
void WiFiComponent::notify_ip_state_listeners_() {
  for (auto *listener : this->ip_state_listeners_) {
    listener->on_ip_state(this->wifi_sta_ip_addresses(), this->get_dns_address(0), this->get_dns_address(1));
  }
}
#endif  // USE_WIFI_IP_STATE_LISTENERS

#ifdef USE_WIFI_SCAN_RESULTS_LISTENERS
void WiFiComponent::notify_scan_results_listeners_() {
  for (auto *listener : this->scan_results_listeners_) {
    listener->on_wifi_scan_results(this->scan_result_);
  }
}
#endif  // USE_WIFI_SCAN_RESULTS_LISTENERS

void WiFiComponent::force_roam_check() {
  if (!this->is_connected() || this->roaming_state_ != RoamingState::IDLE || this->roaming_suppressed_()) {
    ESP_LOGD(TAG, "Roam check requested, but not able to check now");
    return;
  }
  // Reset the attempt counter so a prior run of failed roams doesn't block this explicit request
  // Note that this re-arms automatic roaming if enabled.
  this->roaming_attempts_ = 0;
  this->check_roaming_(millis());
}

void WiFiComponent::check_roaming_(uint32_t now) {
  // Guard: not for hidden networks (may not appear in scan)
  const WiFiAP *selected = this->get_selected_sta_();
  if (selected == nullptr || selected->get_hidden()) {
    this->roaming_attempts_ = ROAMING_MAX_ATTEMPTS;  // Stop checking forever
    return;
  }

  this->roaming_last_check_ = now;
  this->roaming_attempts_++;

  // Guard: skip scan if signal is already good (no meaningful improvement possible)
  int8_t rssi = this->wifi_rssi();
  if (rssi > ROAMING_GOOD_RSSI) {
    ESP_LOGD(TAG, "Roam check skipped, signal good (%d dBm, attempt %u/%u)", rssi, this->roaming_attempts_,
             ROAMING_MAX_ATTEMPTS);
    return;
  }

  ESP_LOGD(TAG, "Roam scan (%d dBm, attempt %u/%u)", rssi, this->roaming_attempts_, ROAMING_MAX_ATTEMPTS);
  this->roaming_state_ = RoamingState::SCANNING;
  if (!this->wifi_scan_start_(this->passive_scan_)) {
    // Scan failed to start (e.g. busy) - don't get stuck in SCANNING forever
    ESP_LOGD(TAG, "Roam scan failed to start");
    this->roaming_state_ = RoamingState::IDLE;
  }
}

void WiFiComponent::process_roaming_scan_() {
  this->scan_done_ = false;
  // Default to IDLE - will be set to CONNECTING if we find a better AP
  this->roaming_state_ = RoamingState::IDLE;
  // Record when scan completed so delayed disconnects (e.g., ESP8266 Beacon Timeout)
  // can be attributed to the scan and avoid resetting the attempts counter
  this->roaming_scan_end_ = millis();

  // Get current connection info
  int8_t current_rssi = this->wifi_rssi();
  // Guard: must still be connected (RSSI may have become invalid during scan)
  if (current_rssi == WIFI_RSSI_DISCONNECTED) {
    this->release_scan_results_();
    return;
  }

  char ssid_buf[SSID_BUFFER_SIZE];
  StringRef current_ssid(this->wifi_ssid_to(ssid_buf));
  bssid_t current_bssid = this->wifi_bssid();

  // Find best candidate: same SSID, different BSSID
  const WiFiScanResult *best = nullptr;
  char bssid_buf[MAC_ADDRESS_PRETTY_BUFFER_SIZE];

  for (const auto &result : this->scan_result_) {
    // Must be same SSID, different BSSID
    if (result.ssid_ != current_ssid || result.get_bssid() == current_bssid)
      continue;

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
    format_mac_addr_upper(result.get_bssid().data(), bssid_buf);
    ESP_LOGV(TAG, "Roam candidate %s %d dBm", bssid_buf, result.get_rssi());
#endif

    // Track the best candidate
    if (best == nullptr || result.get_rssi() > best->get_rssi()) {
      best = &result;
    }
  }

  // Check if best candidate meets minimum improvement threshold
  const WiFiAP *selected = this->get_selected_sta_();
  int8_t improvement = (best == nullptr) ? 0 : best->get_rssi() - current_rssi;
  if (selected == nullptr || improvement < ROAMING_MIN_IMPROVEMENT) {
    ESP_LOGV(TAG, "Roam best %+d dB (need +%d), attempt %u/%u", improvement, ROAMING_MIN_IMPROVEMENT,
             this->roaming_attempts_, ROAMING_MAX_ATTEMPTS);
    this->release_scan_results_();
    return;
  }

  format_mac_addr_upper(best->get_bssid().data(), bssid_buf);
  ESP_LOGI(TAG, "Roaming to %s (%+d dB)", bssid_buf, improvement);

  WiFiAP roam_params = *selected;
  apply_scan_result_to_params(roam_params, *best);

  // Mark as roaming attempt - affects retry behavior if connection fails
  this->roaming_state_ = RoamingState::CONNECTING;
  this->roaming_target_bssid_ = best->get_bssid();  // Must read before releasing scan results

  this->release_scan_results_();

  // Connect directly - wifi_sta_connect_ handles disconnect internally
  this->start_connecting(roam_params);
}

WiFiComponent *global_wifi_component;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::wifi
#endif


──────── [TRUNCATED] ────────
Showing 22,477 chars (head) + 7,494 chars (tail) of 111,688 total clean characters.
Full text saved to: /home/kay/.hermes/cache/web/raw.githubusercontent.com-bba8447a64.md
To read the omitted middle: read_file path="/home/kay/.hermes/cache/web/raw.githubusercontent.com-bba8447a64.md" offset=415 limit=200  (the file is the complete page; raise/lower offset to page through it).
─────────────────────────────