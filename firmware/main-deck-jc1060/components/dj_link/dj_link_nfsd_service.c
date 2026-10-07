#include "dj_link_nfsd_service.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

#include "dj_link_nfsd.h"
#include "dj_link_nfsd_fs.h"

static const char *TAG = "dj_nfsd";

#define NFSD_TASK_STACK   6144u
#define NFSD_TASK_PRIO    1u
#define NFSD_TASK_CORE    1
#define NFSD_NFS_PORT     2049u
#define NFSD_POLL_MS      250u       /* select timeout: how often availability is looked at */
#define NFSD_IDLE_MS      2000u      /* a cached file or directory closes after this */
#define NFSD_RX_MAX       1536u

/* PSRAM: ~10 KB of state and 10 KB of datagram buffers; none of it internal RAM
 * (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY). Used by the service task only,
 * which starts after the previous one has ended. */
EXT_RAM_BSS_ATTR static dj_link_nfsd_t s_srv;
EXT_RAM_BSS_ATTR static dj_link_nfsd_fs_posix_t s_fs;
EXT_RAM_BSS_ATTR static uint8_t s_rx[NFSD_RX_MAX];
EXT_RAM_BSS_ATTR static uint8_t s_tx[DJ_LINK_NFSD_REPLY_MAX];

static dj_link_nfsd_service_cfg_t s_cfg;
static char s_root[DJ_LINK_NFSD_FS_ROOT_MAX];
static uint32_t s_ip;
static int s_pmap_fd = -1;
static int s_nfs_fd = -1;
static volatile bool s_run;
static volatile bool s_running;      /* the task has not exited yet */
static volatile TaskHandle_t s_task;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int open_udp(uint32_t ip, uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void serve_one(int fd, bool portmap)
{
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int n = recvfrom(fd, s_rx, sizeof(s_rx), 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) {
        return;
    }
    const uint32_t client = ntohl(from.sin_addr.s_addr);
    if (client == s_ip) {
        return;                                    /* never answer ourselves */
    }
    dj_link_nfsd_fs_touch(&s_fs, now_ms());
    const size_t r = portmap
        ? dj_link_nfsd_portmap(&s_srv, s_rx, (size_t)n, s_tx, sizeof(s_tx))
        : dj_link_nfsd_rpc(&s_srv, client, s_rx, (size_t)n, s_tx, sizeof(s_tx));
    if (r > 0u) {
        (void)sendto(fd, s_tx, r, 0, (struct sockaddr *)&from, fl);
    }
}

static void nfsd_task(void *arg)
{
    (void)arg;
    uint32_t last_calls = 0u;
    uint32_t last_log_ms = now_ms();
    bool was_available = false;
    while (s_run) {
        const bool avail = s_cfg.available ? s_cfg.available() : false;
        if (avail != was_available) {
            was_available = avail;
            dj_link_nfsd_set_available(&s_srv, avail);
            if (!avail) {
                dj_link_nfsd_fs_close(&s_fs);
            }
            ESP_LOGW(TAG, "export /C/ %s (%s)", avail ? "on" : "off", s_root);
        }
        dj_link_nfsd_fs_idle(&s_fs, now_ms(), NFSD_IDLE_MS);

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s_pmap_fd, &rd);
        FD_SET(s_nfs_fd, &rd);
        struct timeval tv = { 0, NFSD_POLL_MS * 1000 };
        const int maxfd = (s_pmap_fd > s_nfs_fd ? s_pmap_fd : s_nfs_fd) + 1;
        if (select(maxfd, &rd, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(s_pmap_fd, &rd)) {
                serve_one(s_pmap_fd, true);
            }
            if (FD_ISSET(s_nfs_fd, &rd)) {
                serve_one(s_nfs_fd, false);
            }
        }
        /* One line per burst of activity, not per datagram. */
        const uint32_t t = now_ms();
        if (s_srv.calls != last_calls && (uint32_t)(t - last_log_ms) >= 5000u) {
            ESP_LOGI(TAG, "served: %u calls, %u lookups, %u reads (%llu KB), %u listings, "
                     "%u errors, %u refused writes",
                     (unsigned)s_srv.calls, (unsigned)s_srv.lookups, (unsigned)s_srv.reads,
                     (unsigned long long)(s_srv.bytes_read / 1024u), (unsigned)s_srv.readdirs,
                     (unsigned)s_srv.errors, (unsigned)s_srv.refused_writes);
            last_calls = s_srv.calls;
            last_log_ms = t;
        }
    }
    dj_link_nfsd_fs_close(&s_fs);
    s_running = false;
    s_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t dj_link_nfsd_service_start(const dj_link_nfsd_service_cfg_t *cfg, uint32_t ip)
{
    if (!cfg || !cfg->root || ip == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running || s_pmap_fd >= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cfg = *cfg;
    strncpy(s_root, cfg->root, sizeof(s_root) - 1u);
    s_root[sizeof(s_root) - 1u] = '\0';
    s_ip = ip;

    /* The access group a CDJ sees in EXPORT: our own network, as a player's. */
    char group[32];
    snprintf(group, sizeof(group), "%u.%u.%u.%u/255.255.0.0", (unsigned)(ip >> 24),
             (unsigned)((ip >> 16) & 0xffu), (unsigned)((ip >> 8) & 0xffu), (unsigned)(ip & 0xffu));
    dj_link_nfsd_fs_init(&s_fs, s_root, s_cfg.gate_begin, s_cfg.gate_end);
    const dj_link_nfsd_fs_t ops = dj_link_nfsd_fs_ops(&s_fs);
    dj_link_nfsd_init(&s_srv, &ops, NFSD_NFS_PORT, NFSD_NFS_PORT, DJ_LINK_NFSD_PMAP_PORT, group);

    s_pmap_fd = open_udp(ip, DJ_LINK_NFSD_PMAP_PORT);
    s_nfs_fd = s_pmap_fd >= 0 ? open_udp(ip, NFSD_NFS_PORT) : -1;
    if (s_pmap_fd < 0 || s_nfs_fd < 0) {
        ESP_LOGW(TAG, "cannot bind UDP %u / %u: no export", (unsigned)DJ_LINK_NFSD_PMAP_PORT,
                 (unsigned)NFSD_NFS_PORT);
        dj_link_nfsd_service_stop();
        return ESP_FAIL;
    }
    s_run = true;
    s_running = true;
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(nfsd_task, "dj_nfsd", NFSD_TASK_STACK, NULL, NFSD_TASK_PRIO,
                                        &task, NFSD_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_run = false;
        s_running = false;
        dj_link_nfsd_service_stop();
        return ESP_ERR_NO_MEM;
    }
    s_task = task;
    ESP_LOGW(TAG, "serving %s read-only as /C/ on %u.%u.%u.%u (UDP %u portmap, %u mount+nfs)",
             s_root, (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xffu),
             (unsigned)((ip >> 8) & 0xffu), (unsigned)(ip & 0xffu),
             (unsigned)DJ_LINK_NFSD_PMAP_PORT, (unsigned)NFSD_NFS_PORT);
    return ESP_OK;
}

void dj_link_nfsd_service_stop(void)
{
    if (s_running) {
        s_run = false;
        /* The task notices within NFSD_POLL_MS; wait for it so the sockets and
         * buffers are not freed under it. */
        for (int i = 0; i < 100 && s_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_running) {
            ESP_LOGE(TAG, "task did not stop");
            return;
        }
    }
    if (s_pmap_fd >= 0) {
        close(s_pmap_fd);
        s_pmap_fd = -1;
    }
    if (s_nfs_fd >= 0) {
        close(s_nfs_fd);
        s_nfs_fd = -1;
    }
}
