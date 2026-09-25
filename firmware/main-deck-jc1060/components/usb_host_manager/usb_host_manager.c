/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_host_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb_host_topology.h"

/* Added to the pinned esp-usb build by
 * apply_espressif_usb_idle_recovery_patch.cmake. */
extern esp_err_t usb_host_lib_power_off_root_port_if_idle_by_index(
    uint8_t root_port_index);

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "hal/usb_wrap_ll.h"
#include "soc/usb_wrap_struct.h"
#endif

static const char *TAG = "usb_host_mgr";

#define TOPOLOGY_EVENT_DEPTH       16
#define RECOVERY_QUEUE_DEPTH       16
#define RECOVERY_SETTLE_MS         150u
#define RECOVERY_POWER_ON_RETRY_MS 20u
#define RECOVERY_POWER_ON_TIMEOUT_MS 1000u
#define RECOVERY_RETRY_BASE_MS     250u
#define RECOVERY_RETRY_MAX_MS      30000u
#define RESTART_RELEASE_TIMEOUT_MS 5000u
#define RESTART_POLL_MS            10u
#define RESTART_ALL_FREE_TIMEOUT_MS 2000u
#define RESTART_UNINSTALL_TIMEOUT_MS 1000u
#define RESTART_INSTALL_ATTEMPTS   3u
#define RESTART_INSTALL_RETRY_MS   500u

typedef enum {
    MANAGER_STOPPED = 0,
    MANAGER_STARTING,
    MANAGER_READY,
    MANAGER_FAILED,
    MANAGER_RESTARTING,
} manager_state_t;

static usb_host_manager_config_t s_config;
static TaskHandle_t s_daemon_task;
static TaskHandle_t s_topology_task;
static TaskHandle_t s_recovery_task;
static usb_host_client_handle_t s_topology_client;
static QueueHandle_t s_recovery_queue;
static esp_err_t s_install_result = ESP_ERR_INVALID_STATE;
static uint32_t s_daemon_iterations;
static uint32_t s_daemon_errors;
static uint32_t s_no_clients_events;
static uint32_t s_all_free_events;
static uint32_t s_root_power_requested_mask;
static uint32_t s_topology_observations;
static uint32_t s_topology_probe_failures;
static int32_t s_last_topology_result = ESP_ERR_INVALID_STATE;
static uint8_t s_last_topology_address;
static uint8_t s_last_topology_parent_port;
static bool s_last_topology_direct_root;
static uint32_t s_recovery_queue_drops;
static uint32_t s_recovery_requests;
static uint32_t s_recovery_coalesced_requests;
static uint32_t s_recovery_successes;
static uint32_t s_recovery_suppressed_active;
static uint32_t s_recovery_failures;
static usb_host_topology_t s_topology;
static usb_host_recovery_arbiter_t s_recovery_arbiter;
static portMUX_TYPE s_topology_mux = portMUX_INITIALIZER_UNLOCKED;
static manager_state_t s_state = MANAGER_STOPPED;
/* v238: Host Library calls in flight outside the daemon. A restart waits for
 * zero before usb_host_uninstall(); see host_call_enter(). */
static uint32_t s_host_calls;
static const char *s_restart_why = "";
static uint32_t s_host_generation;
static uint32_t s_host_restarts;
static uint32_t s_host_restart_failures;
static const char *s_participant_names[USB_HOST_MANAGER_RESTART_PARTICIPANTS_MAX];
static uint32_t s_participant_mask;
static uint32_t s_participant_released;
static portMUX_TYPE s_participant_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint8_t port;
    usb_host_recovery_reason_t reason;
} recovery_request_t;

typedef enum {
    RECOVERY_CYCLE_FAILED = 0,
    RECOVERY_CYCLE_COMPLETED,
    RECOVERY_CYCLE_SUPPRESSED_ACTIVE,
} recovery_cycle_result_t;

static inline manager_state_t state_get(void)
{
    return (manager_state_t)__atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
}

static inline void state_set(manager_state_t state)
{
    __atomic_store_n(&s_state, state, __ATOMIC_SEQ_CST);
}

/* Pairs with the SEQ_CST RESTARTING store in request_host_restart(): once the
 * daemon has seen s_host_calls == 0 after that store, no caller can still be
 * inside, or enter, a Host Library call. */
static bool host_call_enter(void)
{
    (void)__atomic_add_fetch(&s_host_calls, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_state, __ATOMIC_SEQ_CST) == MANAGER_READY) {
        return true;
    }
    (void)__atomic_sub_fetch(&s_host_calls, 1u, __ATOMIC_SEQ_CST);
    return false;
}

static inline void host_call_exit(void)
{
    (void)__atomic_sub_fetch(&s_host_calls, 1u, __ATOMIC_SEQ_CST);
}

static void topology_event_callback(
    const usb_host_client_event_msg_t *event_msg,
    void *arg)
{
    (void)arg;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        usb_device_handle_t device = NULL;
        usb_device_info_t info = {0};
        const uint8_t address = event_msg->new_dev.address;
        __atomic_store_n(&s_last_topology_address, address, __ATOMIC_RELEASE);
        esp_err_t rc = usb_host_device_open(s_topology_client, address, &device);
        if (rc == ESP_OK) {
            rc = usb_host_device_info(device, &info);
        }
        if (device) {
            (void)usb_host_device_close(s_topology_client, device);
        }
        __atomic_store_n(&s_last_topology_result, (int32_t)rc,
                         __ATOMIC_RELEASE);
        if (rc != ESP_OK) {
            (void)__atomic_add_fetch(&s_topology_probe_failures, 1u,
                                     __ATOMIC_RELAXED);
            ESP_LOGW(TAG, "topology probe addr=%u: %s",
                     (unsigned)address, esp_err_to_name(rc));
            return;
        }

        __atomic_store_n(&s_last_topology_parent_port, info.parent.port_num,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_last_topology_direct_root,
                         info.parent.dev_hdl == NULL, __ATOMIC_RELEASE);

        portENTER_CRITICAL(&s_topology_mux);
        (void)usb_host_topology_observe(&s_topology, address,
                                        info.parent.dev_hdl == NULL,
                                        info.parent.port_num);
        portEXIT_CRITICAL(&s_topology_mux);
        (void)__atomic_add_fetch(&s_topology_observations, 1u,
                                 __ATOMIC_RELAXED);
        ESP_LOGI(TAG, "topology addr=%u root=%u direct=%u",
                 (unsigned)address, (unsigned)info.parent.port_num,
                 info.parent.dev_hdl == NULL ? 1u : 0u);
        return;
    }

    if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_REMOVED) {
        portENTER_CRITICAL(&s_topology_mux);
        (void)usb_host_topology_remove(&s_topology,
                                       event_msg->dev_removed.address);
        portEXIT_CRITICAL(&s_topology_mux);
    }
}

static void topology_client_task(void *arg)
{
    const TaskHandle_t starter = (TaskHandle_t)arg;
    const usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = TOPOLOGY_EVENT_DEPTH,
        .flags = {
            .notify_dev_removed = 1u,
        },
        .async = {
            .client_event_callback = topology_event_callback,
            .callback_arg = NULL,
        },
    };
    const esp_err_t rc = usb_host_client_register(&config, &s_topology_client);
    s_install_result = rc;
    xTaskNotifyGive(starter);
    if (rc != ESP_OK) {
        s_topology_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    bool deregister_logged = false;
    for (;;) {
        const esp_err_t event_rc = usb_host_client_handle_events(
            s_topology_client, pdMS_TO_TICKS(100));
        if (event_rc != ESP_OK && event_rc != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "topology events: %s", esp_err_to_name(event_rc));
        }
        if (state_get() != MANAGER_RESTARTING) {
            continue;
        }
        /* v238: the reinstall re-creates this task with a new client. */
        const esp_err_t dereg_rc =
            usb_host_client_deregister(s_topology_client);
        if (dereg_rc != ESP_OK) {
            if (!deregister_logged) {
                ESP_LOGW(TAG, "host restart: topology deregister: %s",
                         esp_err_to_name(dereg_rc));
                deregister_logged = true;
            }
            continue;
        }
        s_topology_client = NULL;
        __atomic_store_n(&s_topology_task, NULL, __ATOMIC_RELEASE);
        vTaskDelete(NULL);
        return;
    }
}

static esp_err_t recovery_power_off_if_idle(uint8_t port)
{
    if (port >= 32u) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t bit = 1u << port;
    if ((s_config.peripheral_map & bit) == 0u) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!host_call_enter()) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t rc =
        usb_host_lib_power_off_root_port_if_idle_by_index(port);
    host_call_exit();
    if (rc == ESP_OK) {
        (void)__atomic_fetch_and(&s_root_power_requested_mask, ~bit,
                                __ATOMIC_ACQ_REL);
    }
    return rc;
}

static recovery_cycle_result_t recovery_power_cycle(
    uint8_t port,
    usb_host_recovery_reason_t reason)
{
    ESP_LOGW(TAG, "recovering USB%u reason=%u", (unsigned)port,
             (unsigned)reason);
    esp_err_t rc = recovery_power_off_if_idle(port);
    if (rc == ESP_ERR_NOT_FINISHED) {
        /* v234: WARN - the "recovering" line above is WARN too, and an
         * INFO here left a refused recovery looking like a silent success. */
        ESP_LOGW(TAG,
                 "USB%u recovery suppressed: attach/enumeration is active",
                 (unsigned)port);
        return RECOVERY_CYCLE_SUPPRESSED_ACTIVE;
    }
    const bool powered_off_now = rc == ESP_OK;
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB%u recovery power-off: %s", (unsigned)port,
                 esp_err_to_name(rc));
        return RECOVERY_CYCLE_FAILED;
    }
    if (powered_off_now) {
        vTaskDelay(pdMS_TO_TICKS(RECOVERY_SETTLE_MS));
    } else {
        /* A prior attempt can have completed POWER_OFF but timed out waiting
         * for the Host Library to accept POWER_ON. The arbiter retry must
         * resume that half-completed cycle instead of failing forever because
         * the root is already off. Manager-ready plus an enabled port makes
         * ESP_ERR_INVALID_STATE here the idempotent already-off case; active
         * attach/enumeration was distinguished above as NOT_FINISHED. */
        ESP_LOGI(TAG, "USB%u recovery resumes from already-off root",
                 (unsigned)port);
    }

    /* POWER_OFF can leave an HCD disconnect event pending. The Host Library
     * daemon processes that event concurrently, and the indexed POWER_ON API
     * returns ESP_ERR_INVALID_STATE until it has done so. Do not report that
     * transient state as a successful recovery: doing so leaves this root
     * unpowered indefinitely (the requested-power mask remains clear). This
     * bounded wait mirrors the mature storage owner's repeated recovery loop
     * while keeping all Host Library power calls under the shared manager. */
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(RECOVERY_POWER_ON_TIMEOUT_MS);
    do {
        rc = usb_host_manager_set_root_power_by_index(port, true);
        if (rc == ESP_OK) {
            return RECOVERY_CYCLE_COMPLETED;
        }
        if (rc != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "USB%u recovery power-on: %s", (unsigned)port,
                     esp_err_to_name(rc));
            return RECOVERY_CYCLE_FAILED;
        }
        vTaskDelay(pdMS_TO_TICKS(RECOVERY_POWER_ON_RETRY_MS));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);

    ESP_LOGW(TAG, "USB%u recovery power-on timed out: %s",
             (unsigned)port, esp_err_to_name(rc));
    return RECOVERY_CYCLE_FAILED;
}

static void recovery_task(void *arg)
{
    (void)arg;
    recovery_request_t request;
    for (;;) {
        while (xQueueReceive(s_recovery_queue, &request, 0) == pdTRUE) {
            const uint32_t coalesced_before =
                s_recovery_arbiter.coalesced_requests;
            (void)usb_host_recovery_arbiter_request(
                &s_recovery_arbiter, request.port, request.reason);
            (void)__atomic_add_fetch(&s_recovery_requests, 1u,
                                     __ATOMIC_RELAXED);
            if (s_recovery_arbiter.coalesced_requests != coalesced_before) {
                (void)__atomic_add_fetch(&s_recovery_coalesced_requests, 1u,
                                         __ATOMIC_RELAXED);
            }
        }

        uint8_t port = USB_HOST_RECOVERY_PORT_NONE;
        usb_host_recovery_reason_t reason = USB_HOST_RECOVERY_REASON_NONE;
        const uint32_t now = (uint32_t)xTaskGetTickCount();
        /* v238: hold queued recoveries while a host restart runs. */
        if (usb_host_manager_is_ready() &&
            usb_host_recovery_arbiter_acquire(&s_recovery_arbiter, now,
                                              &port, &reason)) {
            const recovery_cycle_result_t result =
                recovery_power_cycle(port, reason);
            (void)usb_host_recovery_arbiter_complete(
                &s_recovery_arbiter, port,
                result != RECOVERY_CYCLE_FAILED,
                (uint32_t)xTaskGetTickCount());
            uint32_t *counter = &s_recovery_failures;
            if (result == RECOVERY_CYCLE_COMPLETED) {
                counter = &s_recovery_successes;
            } else if (result == RECOVERY_CYCLE_SUPPRESSED_ACTIVE) {
                counter = &s_recovery_suppressed_active;
            }
            (void)__atomic_add_fetch(counter, 1u, __ATOMIC_RELAXED);
            continue;
        }

        request = (recovery_request_t) {
            .port = 0u,
            .reason = USB_HOST_RECOVERY_REASON_NONE,
        };
        if (xQueueReceive(s_recovery_queue, &request,
                          pdMS_TO_TICKS(10)) == pdTRUE) {
            const uint32_t coalesced_before =
                s_recovery_arbiter.coalesced_requests;
            (void)usb_host_recovery_arbiter_request(
                &s_recovery_arbiter, request.port, request.reason);
            (void)__atomic_add_fetch(&s_recovery_requests, 1u,
                                     __ATOMIC_RELAXED);
            if (s_recovery_arbiter.coalesced_requests != coalesced_before) {
                (void)__atomic_add_fetch(&s_recovery_coalesced_requests, 1u,
                                         __ATOMIC_RELAXED);
            }
        }
    }
}

/* Boot and the v238 restart both install from the daemon task: the Host
 * Library allocates the DWC interrupts on the calling core. */
static esp_err_t install_host_library(void)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = s_config.root_port_unpowered,
        .intr_flags = s_config.intr_flags,
        .enum_filter_cb = NULL,
        .peripheral_map = s_config.peripheral_map,
        /* P4 DWC DFIFO = 512 lines total, 4 bytes/line. DDJ-400 needs
         * interrupt IN/OUT (MIDI) AND iso OUT MPS 576 (UAC audio)
         * concurrently - no stock bias preset satisfies both.
         *   rx  160 lines -> in_mps     = (160-2)*4 = 632 >= 512
         *   nptx 160 lines -> nptx_mps  = 640 >= 512
         *   ptx  180 lines -> ptx_mps   = 720 >= 576
         * (Same split as main/usb_host_bringup.c; without it the FS DWC
         * defaults to ptx=100 lines = 400 bytes and the UAC EP alloc
         * fails with ESP_ERR_NOT_SUPPORTED.) */
        .fifo_settings_custom = {
            .rx_fifo_lines = 160,
            .nptx_fifo_lines = 160,
            .ptx_fifo_lines = 180,
        },
        /* Dual-port: per-port overrides (index = DWC instance).
         * Port 0 (HS, MSC stick): default bias keeps rx>=130 lines for
         * bulk 512. Port 1 (FS, DDJ): total FIFO is only ~200 lines, so
         * reserve ptx=150 lines (600 B >= UAC iso MPS 576) while keeping
         * rx=30 (in_mps 112 >= MIDI intr 64) and nptx=20 (ctrl 64). */
        .fifo_settings_per_port = {
            { .rx_fifo_lines = 0,  .nptx_fifo_lines = 0, .ptx_fifo_lines = 0   },
            { .rx_fifo_lines = 30, .nptx_fifo_lines = 20, .ptx_fifo_lines = 150 },
        },
    };

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    if (s_config.override_fs_phy_index) {
        usb_wrap_ll_phy_select(&USB_WRAP, s_config.fs_phy_index);
        ESP_LOGI(TAG, "USB Full-Speed root routed to PHY%u",
                 (unsigned)s_config.fs_phy_index);
    }
#endif

    return usb_host_install(&host_config);
}

/* The state must not be RESTARTING here: the task would leave at once. */
static esp_err_t start_topology_client(void)
{
    TaskHandle_t topology_task = NULL;
    const BaseType_t topology_created = xTaskCreate(
        topology_client_task, "usb_topology", 4096u,
        (void *)xTaskGetCurrentTaskHandle(),
        s_config.daemon_priority + 2u, &topology_task);
    if (topology_created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    __atomic_store_n(&s_topology_task, topology_task, __ATOMIC_RELEASE);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0u) {
        return ESP_ERR_TIMEOUT;
    }
    /* topology_client_task() stored its usb_host_client_register() result. */
    return s_install_result;
}

static void count_lib_events(uint32_t event_flags)
{
    if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0u) {
        (void)__atomic_add_fetch(&s_no_clients_events, 1u, __ATOMIC_RELAXED);
    }
    if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0u) {
        (void)__atomic_add_fetch(&s_all_free_events, 1u, __ATOMIC_RELAXED);
    }
}

/* Runs the Host Library for up to timeout_ms; returns the OR of the event
 * flags seen. Clients closing their devices need it to make progress. */
static uint32_t pump_lib_events(uint32_t timeout_ms)
{
    uint32_t event_flags = 0u;
    (void)usb_host_lib_handle_events(pdMS_TO_TICKS(timeout_ms), &event_flags);
    count_lib_events(event_flags);
    return event_flags;
}

static void log_unreleased_participants(uint32_t missing)
{
    for (uint32_t i = 0; i < USB_HOST_MANAGER_RESTART_PARTICIPANTS_MAX; ++i) {
        if ((missing & (1u << i)) != 0u) {
            ESP_LOGW(TAG, "host restart: %s still holds its client",
                     s_participant_names[i] ? s_participant_names[i] : "?");
        }
    }
}

static void host_restart_finish(bool reinstalled, esp_err_t rc)
{
    __atomic_store_n(&s_participant_released, 0u, __ATOMIC_RELEASE);
    if (rc == ESP_OK) {
        (void)__atomic_add_fetch(&s_host_restarts, 1u, __ATOMIC_RELAXED);
        state_set(MANAGER_READY);
    } else {
        (void)__atomic_add_fetch(&s_host_restart_failures, 1u,
                                 __ATOMIC_RELAXED);
        /* Old stack kept: still usable. Reinstall failure: nothing left. */
        state_set(reinstalled ? MANAGER_FAILED : MANAGER_READY);
    }
    /* Last: participants waiting on the generation read the state above. */
    (void)__atomic_add_fetch(&s_host_generation, 1u, __ATOMIC_SEQ_CST);
}

/* v238: uninstall + reinstall exactly like boot. Called on the daemon task
 * with the state already RESTARTING, so no new Host Library call starts. */
static void host_restart(void)
{
    const uint32_t required =
        __atomic_load_n(&s_participant_mask, __ATOMIC_ACQUIRE);
    ESP_LOGW(TAG, "host restart (%s): waiting for clients 0x%02X",
             s_restart_why, (unsigned)required);

    const TickType_t release_deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(RESTART_RELEASE_TIMEOUT_MS);
    uint32_t released = 0u;
    bool quiet = false;
    for (;;) {
        released = __atomic_load_n(&s_participant_released, __ATOMIC_ACQUIRE);
        quiet = (released & required) == required &&
                __atomic_load_n(&s_topology_task, __ATOMIC_ACQUIRE) == NULL &&
                __atomic_load_n(&s_host_calls, __ATOMIC_SEQ_CST) == 0u;
        if (quiet ||
            (int32_t)(release_deadline - xTaskGetTickCount()) <= 0) {
            break;
        }
        (void)pump_lib_events(RESTART_POLL_MS);
    }

    usb_host_lib_info_t info = {0};
    (void)usb_host_lib_info(&info);
    ESP_LOGW(TAG,
             "host restart: released=0x%02X/0x%02X topology=%s calls=%u "
             "clients=%d devices=%d",
             (unsigned)released, (unsigned)required,
             __atomic_load_n(&s_topology_task, __ATOMIC_ACQUIRE) ? "held"
                                                                 : "released",
             (unsigned)__atomic_load_n(&s_host_calls, __ATOMIC_SEQ_CST),
             info.num_clients, info.num_devices);
    if (!quiet) {
        log_unreleased_participants(required & ~released);
    }

    /* A registered client makes usb_host_uninstall() return
     * ESP_ERR_INVALID_STATE, and its handle would dangle after it; the
     * restart cannot be forced past it. Keep the old stack instead. */
    if (info.num_clients != 0 ||
        __atomic_load_n(&s_host_calls, __ATOMIC_SEQ_CST) != 0u) {
        ESP_LOGE(TAG, "host restart aborted: %d client(s) still registered, "
                      "old stack kept",
                 info.num_clients);
        if (__atomic_load_n(&s_topology_task, __ATOMIC_ACQUIRE) == NULL) {
            /* It already left: bring it back on the old stack. */
            state_set(MANAGER_STARTING);
            (void)start_topology_client();
        }
        host_restart_finish(false, ESP_ERR_INVALID_STATE);
        return;
    }

    esp_err_t rc = usb_host_device_free_all();
    if (rc == ESP_ERR_NOT_FINISHED) {
        const TickType_t deadline =
            xTaskGetTickCount() + pdMS_TO_TICKS(RESTART_ALL_FREE_TIMEOUT_MS);
        rc = ESP_ERR_TIMEOUT;
        do {
            if ((pump_lib_events(RESTART_POLL_MS) &
                 USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0u) {
                rc = ESP_OK;
                break;
            }
        } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
    }
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "host restart: free_all: %s (devices=%d)",
                 esp_err_to_name(rc), info.num_devices);
    }

    /* Uninstall needs no pending process/event flags: drain, then retry. */
    const TickType_t uninstall_deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(RESTART_UNINSTALL_TIMEOUT_MS);
    for (;;) {
        (void)pump_lib_events(0u);
        rc = usb_host_uninstall();
        if (rc != ESP_ERR_INVALID_STATE ||
            (int32_t)(uninstall_deadline - xTaskGetTickCount()) <= 0) {
            break;
        }
        (void)pump_lib_events(RESTART_POLL_MS);
    }
    if (rc != ESP_OK) {
        (void)usb_host_lib_info(&info);
        ESP_LOGE(TAG, "host restart: usb_host_uninstall: %s "
                      "(clients=%d devices=%d), old stack kept",
                 esp_err_to_name(rc), info.num_clients, info.num_devices);
        state_set(MANAGER_STARTING);
        (void)start_topology_client();
        host_restart_finish(false, rc);
        return;
    }

    portENTER_CRITICAL(&s_topology_mux);
    usb_host_topology_init(&s_topology);
    portEXIT_CRITICAL(&s_topology_mux);
    __atomic_store_n(&s_root_power_requested_mask,
                     s_config.root_port_unpowered ? 0u
                                                  : s_config.peripheral_map,
                     __ATOMIC_RELEASE);

    /* Leave RESTARTING first: the new topology task would otherwise
     * deregister at once. STARTING still refuses outside Host Library calls. */
    state_set(MANAGER_STARTING);
    for (uint32_t attempt = 1u; attempt <= RESTART_INSTALL_ATTEMPTS;
         ++attempt) {
        rc = install_host_library();
        if (rc == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "host restart: install attempt %u/%u: %s",
                 (unsigned)attempt, (unsigned)RESTART_INSTALL_ATTEMPTS,
                 esp_err_to_name(rc));
        vTaskDelay(pdMS_TO_TICKS(RESTART_INSTALL_RETRY_MS));
    }
    if (rc == ESP_OK) {
        rc = start_topology_client();
        if (rc != ESP_OK) {
            /* Clients can still register; only the root map is missing. */
            ESP_LOGE(TAG, "host restart: topology client: %s",
                     esp_err_to_name(rc));
            rc = ESP_OK;
        }
    }
    s_install_result = rc;
    if (rc == ESP_OK) {
        ESP_LOGW(TAG, "host restart done: USB Host Library reinstalled "
                      "(peripheral_map=0x%02X, roots unpowered=%u)",
                 s_config.peripheral_map,
                 s_config.root_port_unpowered ? 1u : 0u);
    }
    host_restart_finish(true, rc);
}

static void usb_host_daemon_task(void *arg)
{
    TaskHandle_t starter = (TaskHandle_t)arg;
    s_install_result = install_host_library();
    if (s_install_result == ESP_OK) {
        s_install_result = start_topology_client();
    }
    if (s_install_result == ESP_OK) {
        state_set(MANAGER_READY);
        ESP_LOGI(TAG,
                 "USB Host Library ready peripheral_map=0x%02X root_unpowered=%u",
                 s_config.peripheral_map,
                 s_config.root_port_unpowered ? 1u : 0u);
    } else {
        state_set(MANAGER_FAILED);
        ESP_LOGE(TAG, "usb_host_install failed: %s",
                 esp_err_to_name(s_install_result));
    }
    xTaskNotifyGive(starter);

    if (s_install_result != ESP_OK) {
        s_daemon_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        uint32_t event_flags = 0u;
        const esp_err_t rc =
            usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        (void)__atomic_add_fetch(&s_daemon_iterations, 1u, __ATOMIC_RELAXED);

        if (rc != ESP_OK && rc != ESP_ERR_TIMEOUT) {
            (void)__atomic_add_fetch(&s_daemon_errors, 1u, __ATOMIC_RELAXED);
            ESP_LOGE(TAG, "usb_host_lib_handle_events: %s",
                     esp_err_to_name(rc));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        count_lib_events(event_flags);
        if (state_get() == MANAGER_RESTARTING) {
            host_restart();
            if (state_get() == MANAGER_FAILED) {
                ESP_LOGE(TAG, "host restart left no USB Host Library");
                s_daemon_task = NULL;
                vTaskDelete(NULL);
                return;
            }
        }
    }
}

esp_err_t usb_host_manager_init(const usb_host_manager_config_t *config)
{
    if (!config || config->peripheral_map == 0u ||
        config->daemon_stack_size < 3072u || config->daemon_priority == 0u ||
        (config->override_fs_phy_index && config->fs_phy_index > 1u)) {
        return ESP_ERR_INVALID_ARG;
    }
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
    if (config->override_fs_phy_index) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    manager_state_t expected = MANAGER_STOPPED;
    if (!__atomic_compare_exchange_n(&s_state, &expected, MANAGER_STARTING,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        if (expected == MANAGER_READY) {
            return ESP_OK;
        }
        return expected == MANAGER_FAILED ? s_install_result
                                          : ESP_ERR_INVALID_STATE;
    }

    s_config = *config;
    s_install_result = ESP_ERR_INVALID_STATE;
    usb_host_topology_init(&s_topology);
    usb_host_recovery_arbiter_init(
        &s_recovery_arbiter,
        (uint32_t)pdMS_TO_TICKS(RECOVERY_RETRY_BASE_MS),
        (uint32_t)pdMS_TO_TICKS(RECOVERY_RETRY_MAX_MS));
    s_recovery_queue = xQueueCreate(RECOVERY_QUEUE_DEPTH,
                                    sizeof(recovery_request_t));
    if (!s_recovery_queue) {
        s_install_result = ESP_ERR_NO_MEM;
        state_set(MANAGER_FAILED);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(recovery_task, "usb_recovery", 4096u, NULL,
                    config->daemon_priority + 1u,
                    &s_recovery_task) != pdPASS) {
        vQueueDelete(s_recovery_queue);
        s_recovery_queue = NULL;
        s_install_result = ESP_ERR_NO_MEM;
        state_set(MANAGER_FAILED);
        return ESP_ERR_NO_MEM;
    }
    __atomic_store_n(&s_root_power_requested_mask,
                     config->root_port_unpowered ? 0u
                                                 : config->peripheral_map,
                     __ATOMIC_RELEASE);
    const TaskHandle_t starter = xTaskGetCurrentTaskHandle();

    BaseType_t created;
    if (config->daemon_core_id == tskNO_AFFINITY) {
        created = xTaskCreate(usb_host_daemon_task, "usb_hostd",
                              config->daemon_stack_size, (void *)starter,
                              config->daemon_priority, &s_daemon_task);
    } else {
        created = xTaskCreatePinnedToCore(
            usb_host_daemon_task, "usb_hostd", config->daemon_stack_size,
            (void *)starter, config->daemon_priority, &s_daemon_task,
            config->daemon_core_id);
    }

    if (created != pdPASS) {
        vTaskDelete(s_recovery_task);
        s_recovery_task = NULL;
        vQueueDelete(s_recovery_queue);
        s_recovery_queue = NULL;
        s_daemon_task = NULL;
        s_install_result = ESP_ERR_NO_MEM;
        state_set(MANAGER_FAILED);
        return ESP_ERR_NO_MEM;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0u) {
        return ESP_ERR_TIMEOUT;
    }
    return s_install_result;
}

/* jc1060 board extension (v169): the fork's virtual root hub merges the
 * two DWC controllers AND the external hub's downstream ports, so a stick
 * behind the hub shows up with root=2..N (direct=1). Expose the observed
 * root so route filters can decide by side instead of one exact index. */
esp_err_t usb_host_manager_device_root(uint8_t address,
                                       uint8_t *root_port_index_out,
                                       bool *known_out)
{
    if (!root_port_index_out || !known_out || address == 0u ||
        address >= USB_HOST_TOPOLOGY_ADDRESS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    *root_port_index_out = 0u;
    *known_out = false;
    if (!usb_host_manager_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_topology_mux);
    const usb_host_topology_entry_t *entry =
        &s_topology.entries[address];
    if (entry->observed && entry->present) {
        *root_port_index_out = entry->root_port_index;
        *known_out = true;
    }
    portEXIT_CRITICAL(&s_topology_mux);
    return *known_out ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t usb_host_manager_device_matches_root(uint8_t address,
                                               uint8_t root_port_index,
                                               bool require_direct_root,
                                               bool *matches_out)
{
    if (!matches_out || address == 0u || root_port_index >= 32u) {
        return ESP_ERR_INVALID_ARG;
    }
    *matches_out = false;
    if (!usb_host_manager_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    bool known = false;
    portENTER_CRITICAL(&s_topology_mux);
    const bool matches = usb_host_topology_matches_root(
        &s_topology, address, root_port_index, require_direct_root, &known);
    portEXIT_CRITICAL(&s_topology_mux);
    if (!known) {
        return ESP_ERR_NOT_FOUND;
    }
    *matches_out = matches;
    return ESP_OK;
}

esp_err_t usb_host_manager_request_recovery(
    uint8_t root_port_index,
    usb_host_recovery_reason_t reason)
{
    if (root_port_index >= USB_HOST_RECOVERY_PORT_COUNT ||
        reason == USB_HOST_RECOVERY_REASON_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_manager_is_ready() || !s_recovery_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    const recovery_request_t request = {
        .port = root_port_index,
        .reason = reason,
    };
    if (xQueueSend(s_recovery_queue, &request, 0) != pdTRUE) {
        (void)__atomic_add_fetch(&s_recovery_queue_drops, 1u,
                                 __ATOMIC_RELAXED);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool usb_host_manager_is_ready(void)
{
    return state_get() == MANAGER_READY;
}

esp_err_t usb_host_manager_set_all_root_power(bool enable)
{
    if (!host_call_enter()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t rc = usb_host_lib_set_root_port_power(enable);
    host_call_exit();
    if (rc == ESP_OK || rc == ESP_ERR_INVALID_STATE) {
        __atomic_store_n(&s_root_power_requested_mask,
                         enable ? s_config.peripheral_map : 0u,
                         __ATOMIC_RELEASE);
    }
    return rc;
}

esp_err_t usb_host_manager_set_root_power_by_index(uint8_t root_port_index,
                                                   bool enable)
{
    if (root_port_index >= 32u) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t bit = 1u << root_port_index;
    if ((s_config.peripheral_map & bit) == 0u) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!host_call_enter()) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t rc = usb_host_lib_set_root_port_power_by_index(
        root_port_index, enable);
    host_call_exit();
    if (rc == ESP_OK) {
        if (enable) {
            (void)__atomic_fetch_or(&s_root_power_requested_mask, bit,
                                    __ATOMIC_ACQ_REL);
        } else {
            (void)__atomic_fetch_and(&s_root_power_requested_mask, ~bit,
                                     __ATOMIC_ACQ_REL);
        }
    }
    return rc;
}

esp_err_t usb_host_manager_get_library_info(usb_host_lib_info_t *info_out)
{
    if (!info_out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!host_call_enter()) {
        memset(info_out, 0, sizeof(*info_out));
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t rc = usb_host_lib_info(info_out);
    host_call_exit();
    return rc;
}

esp_err_t usb_host_manager_restart_participant_register(const char *name,
                                                        uint32_t *id_out)
{
    if (!name || !id_out) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t rc = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_participant_mux);
    for (uint32_t i = 0; i < USB_HOST_MANAGER_RESTART_PARTICIPANTS_MAX; ++i) {
        const uint32_t bit = 1u << i;
        if ((s_participant_mask & bit) == 0u) {
            s_participant_names[i] = name;
            __atomic_fetch_or(&s_participant_mask, bit, __ATOMIC_RELEASE);
            *id_out = bit;
            rc = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&s_participant_mux);
    return rc;
}

esp_err_t usb_host_manager_request_host_restart(const char *why)
{
    /* Counted as a Host Library call so the daemon cannot uninstall before
     * usb_host_lib_unblock() below has returned. */
    if (!host_call_enter()) {
        return ESP_ERR_INVALID_STATE;
    }
    manager_state_t expected = MANAGER_READY;
    const bool won = __atomic_compare_exchange_n(
        &s_state, &expected, MANAGER_RESTARTING, false, __ATOMIC_SEQ_CST,
        __ATOMIC_SEQ_CST);
    if (won) {
        s_restart_why = why ? why : "?";
        __atomic_store_n(&s_participant_released, 0u, __ATOMIC_RELEASE);
        (void)usb_host_lib_unblock();
    }
    host_call_exit();
    return won ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool usb_host_manager_host_restart_pending(void)
{
    return state_get() == MANAGER_RESTARTING;
}

uint32_t usb_host_manager_host_generation(void)
{
    return __atomic_load_n(&s_host_generation, __ATOMIC_SEQ_CST);
}

void usb_host_manager_host_restart_release(uint32_t id, uint32_t generation)
{
    /* A release from an earlier restart must not count for the next one. */
    if (usb_host_manager_host_generation() != generation ||
        !usb_host_manager_host_restart_pending()) {
        return;
    }
    (void)__atomic_fetch_or(&s_participant_released, id, __ATOMIC_ACQ_REL);
}

esp_err_t usb_host_manager_wait_host_restart(uint32_t generation,
                                             TickType_t timeout)
{
    const TickType_t start = xTaskGetTickCount();
    while (usb_host_manager_host_generation() == generation) {
        if (xTaskGetTickCount() - start >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return usb_host_manager_is_ready() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void usb_host_manager_get_diagnostics(usb_host_manager_diagnostics_t *diag_out)
{
    if (!diag_out) {
        return;
    }
    const uint32_t requested_mask =
        __atomic_load_n(&s_root_power_requested_mask, __ATOMIC_ACQUIRE);
    *diag_out = (usb_host_manager_diagnostics_t) {
        .install_result = s_install_result,
        .daemon_iterations =
            __atomic_load_n(&s_daemon_iterations, __ATOMIC_ACQUIRE),
        .daemon_errors =
            __atomic_load_n(&s_daemon_errors, __ATOMIC_ACQUIRE),
        .no_clients_events =
            __atomic_load_n(&s_no_clients_events, __ATOMIC_ACQUIRE),
        .all_free_events =
            __atomic_load_n(&s_all_free_events, __ATOMIC_ACQUIRE),
        .root_power_requested_mask = requested_mask,
        .topology_observations =
            __atomic_load_n(&s_topology_observations, __ATOMIC_ACQUIRE),
        .topology_probe_failures =
            __atomic_load_n(&s_topology_probe_failures, __ATOMIC_ACQUIRE),
        .last_topology_result =
            __atomic_load_n(&s_last_topology_result, __ATOMIC_ACQUIRE),
        .last_topology_address =
            __atomic_load_n(&s_last_topology_address, __ATOMIC_ACQUIRE),
        .last_topology_parent_port =
            __atomic_load_n(&s_last_topology_parent_port, __ATOMIC_ACQUIRE),
        .last_topology_direct_root =
            __atomic_load_n(&s_last_topology_direct_root, __ATOMIC_ACQUIRE),
        .recovery_queue_drops =
            __atomic_load_n(&s_recovery_queue_drops, __ATOMIC_ACQUIRE),
        .recovery_requests =
            __atomic_load_n(&s_recovery_requests, __ATOMIC_ACQUIRE),
        .recovery_coalesced_requests =
            __atomic_load_n(&s_recovery_coalesced_requests,
                            __ATOMIC_ACQUIRE),
        .recovery_successes =
            __atomic_load_n(&s_recovery_successes, __ATOMIC_ACQUIRE),
        .recovery_suppressed_active =
            __atomic_load_n(&s_recovery_suppressed_active,
                            __ATOMIC_ACQUIRE),
        .recovery_failures =
            __atomic_load_n(&s_recovery_failures, __ATOMIC_ACQUIRE),
        .host_restarts =
            __atomic_load_n(&s_host_restarts, __ATOMIC_ACQUIRE),
        .host_restart_failures =
            __atomic_load_n(&s_host_restart_failures, __ATOMIC_ACQUIRE),
        .host_generation = usb_host_manager_host_generation(),
        .peripheral_map = s_config.peripheral_map,
        .fs_phy_override_requested = s_config.override_fs_phy_index,
        .fs_phy_index = s_config.fs_phy_index,
        .ready = usb_host_manager_is_ready(),
        .root_power_requested =
            s_config.peripheral_map != 0u &&
            (requested_mask & s_config.peripheral_map) ==
                s_config.peripheral_map,
    };
}
