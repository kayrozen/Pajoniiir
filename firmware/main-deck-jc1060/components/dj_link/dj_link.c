#include "dj_link.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/def.h"
#include "lwip/ip.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

#include "djlink/beat.h"
#include "djlink/dbserver.h"
#include "djlink/media.h"
#include "djlink/nfs.h"
#include "djlink/packet.h"
#include "djlink/status.h"
#include "djlink/sync.h"

#include "dj_link_anlz.h"
#include "dj_link_master.h"
#include "dj_link_pdb.h"
#include "dj_link_session.h"
#include "sd_io_gate.h"
#include "sd_idle_wait.h"

static const char *TAG = "dj_link";

/* Name other players and rekordbox show for us. */
#define DJ_LINK_DEVICE_NAME "PAJONIIIR"

/* Below LVGL (4) and every audio/USB task; core 1, away from the core 0
 * audio task. Starving it only drops observation packets. The v248 dbserver
 * client parses replies on this stack; v249 adds FATFS calls (cache file,
 * directory scan), so the stack grew to 8 KB and moved to PSRAM like
 * ui_load's (SD/FATFS I/O already runs from PSRAM stacks, see ui_library). */
#define DJ_LINK_TASK_STACK    8192u
#define DJ_LINK_TASK_PRIO     1u
#define DJ_LINK_TASK_CORE     1
#define DJ_LINK_RX_WAIT_MS    100u  /* also bounds the stop latency */
#define DJ_LINK_IDLE_POLL_MS  200u  /* waiting for an IP / retrying a bind */
#define DJ_LINK_PUBLISH_MS    100u
#define DJ_LINK_REFRESH_MS    500u
#define DJ_LINK_LOAD_TIMEOUT_MS 3000u  /* UI must take a network load by then */

/* Datagrams are copied out of the lwIP pbuf in the tcpip thread into this
 * single-producer / single-consumer ring, so nothing is allocated per packet
 * and the decode runs at DJ_LINK_TASK_PRIO, not in the tcpip thread. The ring
 * (~4 KB) lives in PSRAM, allocated once when the task starts and freed when
 * it stops, so an OFF observer costs no internal RAM. */
#define DJ_LINK_RING_SLOTS 8u
typedef struct {
    uint16_t port;
    uint16_t len;
    uint32_t rx_ms;
    uint32_t src_ip;   /* host order */
    bool     unicast;  /* addressed to our IP, not broadcast/multicast */
    uint8_t  data[DJLINK_MAX_PACKET];
} dj_link_rx_slot_t;

static dj_link_rx_slot_t *s_ring;    /* DJ_LINK_RING_SLOTS, set while the task runs */
static atomic_uint s_ring_head;      /* written by the tcpip thread only */
static atomic_uint s_ring_tail;      /* written by the dj_link task only */
static atomic_uint s_ring_filtered;  /* dropped before the ring: size/magic/full */

enum { DJ_LINK_PCB_DISCOVERY, DJ_LINK_PCB_BEAT, DJ_LINK_PCB_STATUS, DJ_LINK_PCB_COUNT };
static const uint16_t s_ports[DJ_LINK_PCB_COUNT] = {
    DJLINK_PORT_DISCOVERY, DJLINK_PORT_BEAT, DJLINK_PORT_STATUS,
};
static struct udp_pcb *s_pcbs[DJ_LINK_PCB_COUNT]; /* touched in the tcpip thread only */
static TaskHandle_t s_rx_task;       /* valid while any PCB exists */
static u8_t s_netif_index;           /* Ethernet netif, set by dj_link_open() */

static dj_link_config_t s_config;
static bool s_initialized;
static dj_link_table_t s_table;      /* owned by the dj_link task */
/* v298: one player per deck, sharing our MAC/IP (like an XDJ-XZ). Deck 1's
 * session is the library player (media queries, local track source); deck
 * 2's starts once deck 1 is active. Owned by the dj_link task. */
static dj_link_session_t s_session[2];
static uint8_t s_joined_number[2];   /* last numbers logged, dj_link task */
static uint32_t s_status_ms;         /* last CDJ status burst, dj_link task */
static uint32_t s_status_counter[2]; /* 0xc8 packet counters, dj_link task */
static dj_link_deck_report_t s_deck_report[2]; /* guarded by s_mux */
/* v301: absolute position 0x0b / beat 0x28 on port 50001, dj_link task. */
static uint32_t s_position_ms;       /* last position burst */
static dj_link_beat_tracker_t s_beat_tracker[2];
static uint32_t s_players_wait_ms = DJ_LINK_RX_WAIT_MS; /* next players pass */
/* Every packet we send is built here by the dj_link task, then copied into
 * a pbuf in the tcpip thread before esp_netif_tcpip_exec() returns. Sized
 * for the largest one: media response (0xc0) or CDJ status (0xd4). */
static uint8_t s_tx[DJLINK_STATUS_PACKET_LEN > DJLINK_MEDIA_RESP_PACKET_LEN
                        ? DJLINK_STATUS_PACKET_LEN : DJLINK_MEDIA_RESP_PACKET_LEN];
_Static_assert(sizeof(s_tx) >= DJLINK_BEAT_PACKET_LEN &&
               sizeof(s_tx) >= DJLINK_POSITION_PACKET_LEN, "v301 packets fit s_tx");
static atomic_uint s_local_track_count; /* written by the LVGL task */
/* v305: tempo master negotiation (dj_link_master.h), dj_link task only;
 * s_sync_control is the LINK SYNC switch, written by any task. */
static atomic_bool s_sync_control;
static dj_link_master_t s_master;
static dj_link_master_phase_t s_master_logged;

/* Network load-track handoff: the dj_link task validates a 0x19 and parks it
 * here; ui_update() takes it, runs the load and reports back; the dj_link
 * task then acks (accepted only) and frees the slot. One at a time. */
typedef enum {
    DJ_LINK_REMOTE_IDLE = 0,
    DJ_LINK_REMOTE_PENDING,   /* waiting for ui_update() */
    DJ_LINK_REMOTE_TAKEN,     /* ui_update() is deciding */
    DJ_LINK_REMOTE_ACCEPTED,
    DJ_LINK_REMOTE_REFUSED,
} dj_link_remote_state_t;
static dj_link_remote_state_t s_remote_state; /* guarded by s_mux */
static dj_link_load_request_t s_remote_req;   /* guarded by s_mux */
static uint32_t s_remote_ip;                  /* dj_link task only */
static int s_remote_deck;                     /* dj_link task only, acking session */
static uint32_t s_remote_since_ms;            /* dj_link task only */
static uint32_t s_remote_next_id;             /* dj_link task only */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_want;                  /* guarded by s_mux */
static bool s_running;               /* guarded by s_mux */
static dj_link_summary_t s_published;/* guarded by s_mux */

/* v248 peer library browse. One TCP connection at a time, to the dbserver of
 * the player the UI selected. Received bytes are copied out of the pbufs in
 * the tcpip thread into a SPSC byte ring, like the UDP datagrams; the ring is
 * larger than the TCP window, and a segment that does not fit is left to lwIP
 * (ERR_MEM) to redeliver. Ring, client and row cache share one PSRAM block,
 * allocated when a peer is selected and freed on deselect / OFF. */
/* > CONFIG_LWIP_TCP_WND_DEFAULT (65534 in sdkconfig.defaults): a full window
 * always fits, so ERR_MEM (redelivered only by lwIP's slow timer) stays a
 * safety net. Power of two for the free-running indices. */
#define DJ_LINK_TCP_RING_BYTES    65536u
#define DJ_LINK_TCP_EVT_CONNECTED 0x1u
#define DJ_LINK_TCP_EVT_CLOSED    0x2u
#define DJ_LINK_BROWSE_DETAIL_MAX 64u     /* rows per want_details window */

typedef struct {
    dj_link_db_t client;                                    /* dj_link task only */
    uint8_t tcp_ring[DJ_LINK_TCP_RING_BYTES];
    dj_link_peer_track_t tracks[DJ_LINK_BROWSE_MAX_TRACKS]; /* rows: s_mux */
} dj_link_browse_mem_t;

static dj_link_browse_mem_t *s_browse; /* written by the dj_link task under s_mux */
static struct tcp_pcb *s_tcp;          /* tcpip thread only */
static atomic_uint s_tcp_head;         /* written by the tcpip thread only */
static atomic_uint s_tcp_tail;         /* written by the dj_link task only */
static atomic_uint s_tcp_events;       /* DJ_LINK_TCP_EVT_*, set by the tcpip thread */
static uint32_t s_tcp_gen;             /* dj_link task: bumps on every close */
static uint8_t  s_browse_peer;         /* dj_link task: selection being served */
static bool     s_browse_started;      /* dj_link task: client started */
static uint32_t s_browse_req_done;     /* dj_link task: last request served */
static dj_link_browse_state_t s_browse_logged; /* dj_link task */
static uint8_t  s_browse_want;         /* guarded by s_mux */
static uint8_t  s_browse_sort_want;    /* v310, guarded by s_mux */
static bool     s_browse_desc_want;    /* v310, guarded by s_mux */
static uint8_t  s_browse_menu_want;    /* v311, dj_link_db_menu_t, guarded by s_mux */
static uint32_t s_browse_menu_id_want; /* v311, guarded by s_mux */
static bool     s_browse_desc;         /* v310, dj_link task: list being reversed */
static uint32_t s_browse_rows;         /* v310, dj_link task: rows of the list */
static uint32_t s_browse_req;          /* guarded by s_mux, bumps per select */
static dj_link_browse_status_t s_browse_status; /* guarded by s_mux */
static uint32_t s_detail_gen;          /* guarded by s_mux */
static uint32_t s_detail_first;        /* guarded by s_mux */
static uint32_t s_detail_count;        /* guarded by s_mux */
/* v307: a row being loaded wants its artist first (guarded by s_mux) */
static uint32_t s_detail_prio_gen;
static uint32_t s_detail_prio_index;
static uint32_t s_detail_prio_id;

static uint32_t dj_link_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* tcpip thread. Drop-to-null for anything oversized, short, without the
 * magic header, or arriving while the ring is full. */
static void dj_link_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    (void)port;
    if (!p) {
        return;
    }
    unsigned head = atomic_load_explicit(&s_ring_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&s_ring_tail, memory_order_acquire);
    bool queued = false;
    if (p->tot_len > DJLINK_MAGIC_LEN && p->tot_len <= DJLINK_MAX_PACKET &&
        head - tail < DJ_LINK_RING_SLOTS) {
        dj_link_rx_slot_t *slot = &s_ring[head % DJ_LINK_RING_SLOTS];
        uint16_t len = pbuf_copy_partial(p, slot->data, p->tot_len, 0);
        if (len == p->tot_len && djlink_packet_is_valid(slot->data, len)) {
            const ip4_addr_t *dest = ip4_current_dest_addr();
            slot->port = (uint16_t)(uintptr_t)arg;
            slot->len = len;
            slot->rx_ms = dj_link_now_ms();
            slot->src_ip = addr ? lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr))) : 0u;
            slot->unicast = !ip4_addr_isbroadcast(dest, ip_current_input_netif()) &&
                            !ip4_addr_ismulticast(dest);
            atomic_store_explicit(&s_ring_head, head + 1u, memory_order_release);
            queued = true;
        }
    }
    pbuf_free(p);
    if (queued) {
        xTaskNotifyGive(s_rx_task);
    } else {
        atomic_fetch_add_explicit(&s_ring_filtered, 1u, memory_order_relaxed);
    }
}

static esp_err_t dj_link_close_pcbs_tcpip(void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < DJ_LINK_PCB_COUNT; i++) {
        if (s_pcbs[i]) {
            udp_remove(s_pcbs[i]);
            s_pcbs[i] = NULL;
        }
    }
    return ESP_OK;
}

/* Bound to the Ethernet netif: a datagram arriving on any other interface
 * (Wi-Fi SoftAP included) never reaches the callback, and everything we send
 * leaves through Ethernet. */
static esp_err_t dj_link_open_pcbs_tcpip(void *ctx)
{
    struct netif *nif = netif_get_by_index((u8_t)(uintptr_t)ctx);
    if (!nif) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < DJ_LINK_PCB_COUNT; i++) {
        struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb) {
            dj_link_close_pcbs_tcpip(NULL);
            return ESP_ERR_NO_MEM;
        }
        if (udp_bind(pcb, IP4_ADDR_ANY, s_ports[i]) != ERR_OK) {
            udp_remove(pcb);
            dj_link_close_pcbs_tcpip(NULL);
            return ESP_FAIL;
        }
        udp_bind_netif(pcb, nif);
        ip_set_option(pcb, SOF_BROADCAST);
        udp_recv(pcb, dj_link_udp_recv, (void *)(uintptr_t)s_ports[i]);
        s_pcbs[i] = pcb;
    }
    return ESP_OK;
}

typedef struct {
    size_t pcb;
    uint32_t dst_ip;    /* host order; 0 = subnet broadcast */
    uint16_t port;
    uint16_t len;
} dj_link_tx_t;

/* tcpip thread: one pbuf per datagram, freed before returning. */
static esp_err_t dj_link_send_tcpip(void *ctx)
{
    const dj_link_tx_t *tx = ctx;
    struct udp_pcb *pcb = s_pcbs[tx->pcb];
    struct netif *nif = pcb ? netif_get_by_index(pcb->netif_idx) : NULL;
    if (!nif) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t dst = tx->dst_ip ? lwip_htonl(tx->dst_ip)
                              : (ip4_addr_get_u32(netif_ip4_addr(nif)) |
                                 ~ip4_addr_get_u32(netif_ip4_netmask(nif)));
    ip_addr_t addr = IPADDR4_INIT(dst);
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, tx->len, PBUF_RAM);
    if (!p) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(p->payload, s_tx, tx->len);
    err_t err = udp_sendto_if(pcb, p, &addr, tx->port, nif);
    pbuf_free(p);
    return err == ERR_OK ? ESP_OK : ESP_FAIL;
}

static void dj_link_send(size_t pcb, uint32_t dst_ip, uint16_t port, int len)
{
    if (len <= 0 || len > (int)sizeof(s_tx)) {
        return;
    }
    dj_link_tx_t tx = { .pcb = pcb, .dst_ip = dst_ip, .port = port, .len = (uint16_t)len };
    esp_err_t rc = esp_netif_tcpip_exec(dj_link_send_tcpip, &tx);
    if (rc != ESP_OK) {
        ESP_LOGD(TAG, "send type 0x%02x to port %u failed: %s",
                 (unsigned)s_tx[0x0a], (unsigned)port, esp_err_to_name(rc));
    }
}

static uint32_t dj_link_eth_ip(esp_netif_t *eth)
{
    esp_netif_ip_info_t info;
    if (!eth || esp_netif_get_ip_info(eth, &info) != ESP_OK) {
        return 0u;
    }
    return lwip_ntohl(info.ip.addr);
}

/* ---- v248: TCP transport for the dbserver client ------------------------ */

static void dj_link_tcp_signal(unsigned event)
{
    atomic_fetch_or_explicit(&s_tcp_events, event, memory_order_release);
    xTaskNotifyGive(s_rx_task);
}

/* tcpip thread. */
static err_t dj_link_tcp_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (!p) {
        dj_link_tcp_signal(DJ_LINK_TCP_EVT_CLOSED); /* FIN from the peer */
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return ERR_OK;
    }
    unsigned head = atomic_load_explicit(&s_tcp_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&s_tcp_tail, memory_order_acquire);
    if (p->tot_len > DJ_LINK_TCP_RING_BYTES - (head - tail)) {
        return ERR_MEM; /* not freed: lwIP keeps it and retries */
    }
    uint8_t *ring = s_browse->tcp_ring;
    for (struct pbuf *q = p; q; q = q->next) {
        size_t at = head % DJ_LINK_TCP_RING_BYTES;
        size_t first = DJ_LINK_TCP_RING_BYTES - at;
        if (first > q->len) {
            first = q->len;
        }
        memcpy(&ring[at], q->payload, first);
        memcpy(ring, (const uint8_t *)q->payload + first, q->len - first);
        head += q->len;
    }
    atomic_store_explicit(&s_tcp_head, head, memory_order_release);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    xTaskNotifyGive(s_rx_task);
    return ERR_OK;
}

/* tcpip thread. The PCB is already freed by lwIP. */
static void dj_link_tcp_err(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    s_tcp = NULL;
    dj_link_tcp_signal(DJ_LINK_TCP_EVT_CLOSED);
}

static err_t dj_link_tcp_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    (void)pcb;
    (void)err;
    dj_link_tcp_signal(DJ_LINK_TCP_EVT_CONNECTED);
    return ERR_OK;
}

/* Callbacks cleared first: after this returns nothing of the old
 * connection can reach the ring or the event flags. */
static esp_err_t dj_link_tcp_close_tcpip(void *ctx)
{
    (void)ctx;
    if (s_tcp) {
        tcp_arg(s_tcp, NULL);
        tcp_recv(s_tcp, NULL);
        tcp_err(s_tcp, NULL);
        if (tcp_close(s_tcp) != ERR_OK) {
            tcp_abort(s_tcp);
        }
        s_tcp = NULL;
    }
    return ESP_OK;
}

typedef struct {
    uint32_t ip;    /* host order */
    uint16_t port;
} dj_link_tcp_dst_t;

/* Bound to the Ethernet netif like the UDP PCBs. */
static esp_err_t dj_link_tcp_connect_tcpip(void *ctx)
{
    const dj_link_tcp_dst_t *dst = ctx;
    struct netif *nif = netif_get_by_index(s_netif_index);
    if (!nif || s_tcp) {
        return ESP_ERR_INVALID_STATE;
    }
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        return ESP_ERR_NO_MEM;
    }
    tcp_bind_netif(pcb, nif);
    tcp_recv(pcb, dj_link_tcp_recv);
    tcp_err(pcb, dj_link_tcp_err);
    s_tcp = pcb;
    ip_addr_t addr = IPADDR4_INIT(lwip_htonl(dst->ip));
    if (tcp_connect(pcb, &addr, dst->port, dj_link_tcp_connected) != ERR_OK) {
        dj_link_tcp_close_tcpip(NULL);
        return ESP_FAIL;
    }
    return ESP_OK;
}

typedef struct {
    const uint8_t *buf;
    size_t len;
} dj_link_tcp_tx_t;

static esp_err_t dj_link_tcp_send_tcpip(void *ctx)
{
    const dj_link_tcp_tx_t *tx = ctx;
    if (!s_tcp || tx->len > tcp_sndbuf(s_tcp) ||
        tcp_write(s_tcp, tx->buf, (u16_t)tx->len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return ESP_FAIL;
    }
    tcp_output(s_tcp);
    return ESP_OK;
}

static void dj_link_tcp_close(void)
{
    esp_netif_tcpip_exec(dj_link_tcp_close_tcpip, NULL);
    /* No producer is left: drop what the old connection left behind. */
    s_tcp_gen++;
    atomic_store(&s_tcp_events, 0u);
    atomic_store(&s_tcp_head, 0u);
    atomic_store(&s_tcp_tail, 0u);
}

/* ---- v248: dbserver client hooks (dj_link task) ------------------------- */

static int dj_link_db_io_connect(void *ctx, uint32_t ip, uint16_t port)
{
    (void)ctx;
    dj_link_tcp_close();
    dj_link_tcp_dst_t dst = { .ip = ip, .port = port };
    esp_err_t rc = esp_netif_tcpip_exec(dj_link_tcp_connect_tcpip, &dst);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "dbserver connect %u.%u.%u.%u:%u failed: %s",
                 (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xffu),
                 (unsigned)((ip >> 8) & 0xffu), (unsigned)(ip & 0xffu),
                 (unsigned)port, esp_err_to_name(rc));
    }
    return rc == ESP_OK ? 0 : -1;
}

static int dj_link_db_io_send(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    dj_link_tcp_tx_t tx = { .buf = buf, .len = len };
    return esp_netif_tcpip_exec(dj_link_tcp_send_tcpip, &tx) == ESP_OK ? 0 : -1;
}

static void dj_link_db_io_close(void *ctx)
{
    (void)ctx;
    dj_link_tcp_close();
}

static void dj_link_db_io_list_begin(void *ctx, uint32_t total)
{
    (void)ctx;
    s_browse_rows = total < DJ_LINK_BROWSE_MAX_TRACKS ? total : DJ_LINK_BROWSE_MAX_TRACKS;
    portENTER_CRITICAL(&s_mux);
    s_browse_status.total = total;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "browse #%u: %u rows (menu %u id %u)%s", (unsigned)s_browse_peer,
             (unsigned)total, (unsigned)s_browse->client.menu, (unsigned)s_browse->client.menu_id,
             total > DJ_LINK_BROWSE_MAX_TRACKS ? " (list capped)" : "");
}

static void dj_link_db_io_track(void *ctx, uint32_t index, const dj_link_peer_track_t *t,
                                bool detail)
{
    (void)ctx;
    if (index >= DJ_LINK_BROWSE_MAX_TRACKS || (s_browse_desc && index >= s_browse_rows)) {
        return;
    }
    /* v310: a descending list is stored reversed; its rows are published
     * all at once when it is complete (dj_link_browse_service). */
    index = dj_link_db_row_index(s_browse_rows, s_browse_desc, index);
    portENTER_CRITICAL(&s_mux);
    dj_link_peer_track_t *row = &s_browse->tracks[index];
    if (!detail) {
        *row = *t;
        if (!s_browse_desc && index >= s_browse_status.count) {
            s_browse_status.count = index + 1u;
        }
    } else if (index < s_browse_status.count && row->rekordbox_id == t->rekordbox_id) {
        row->duration_s = t->duration_s;
        row->bpm100 = t->bpm100;
        if (t->artwork_id) {
            row->artwork_id = t->artwork_id;
        }
        if (t->artist[0]) {
            /* The list's second label follows the player's sort; the
             * metadata artist is authoritative. */
            memcpy(row->artist, t->artist, sizeof(row->artist));
        }
        row->has_detail = true;
        s_browse_status.detail_seq++;
    }
    portEXIT_CRITICAL(&s_mux);
}

static bool dj_link_db_io_next_detail(void *ctx, uint32_t *index, uint32_t *rekordbox_id)
{
    (void)ctx;
    bool found = false;
    portENTER_CRITICAL(&s_mux);
    const uint32_t gen = s_browse_status.generation;
    uint32_t i;
    if (dj_link_db_pick_detail(s_browse->tracks, s_browse_status.count,
                               s_detail_gen == gen ? s_detail_first : 0u,
                               s_detail_gen == gen ? s_detail_count : 0u,
                               s_detail_prio_index,
                               s_detail_prio_gen == gen ? s_detail_prio_id : 0u, &i)) {
        *index = dj_link_db_row_index(s_browse_rows, s_browse_desc, i); /* client's order */
        *rekordbox_id = s_browse->tracks[i].rekordbox_id;
        found = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return found;
}

static void dj_link_fetch_on_path(uint32_t rekordbox_id, const char *path);

/* v297: the fetch job asked the browse session for a file path. */
static void dj_link_db_io_path(void *ctx, uint32_t rekordbox_id, const char *path)
{
    (void)ctx;
    dj_link_fetch_on_path(rekordbox_id, path);
}

static void dj_link_fetch_on_blob(uint32_t rekordbox_id, uint16_t request, size_t len,
                                  bool answered);
static void dj_link_db_io_blob(void *ctx, uint32_t rekordbox_id, uint16_t request, size_t len,
                               bool answered)
{
    (void)ctx;
    dj_link_fetch_on_blob(rekordbox_id, request, len, answered);
}

static const dj_link_peer_t *dj_link_find_peer(uint8_t number)
{
    for (size_t i = 0; i < DJ_LINK_MAX_PEERS; i++) {
        if (s_table.peers[i].in_use && s_table.peers[i].device_number == number) {
            return &s_table.peers[i];
        }
    }
    return NULL;
}

/* Close the connection; the cache and the selection stay. */
static void dj_link_browse_halt(void)
{
    if (s_browse) {
        dj_link_db_stop(&s_browse->client);
    }
    dj_link_tcp_close();
    s_browse_started = false;
}

static void dj_link_browse_free(void)
{
    dj_link_browse_halt();
    portENTER_CRITICAL(&s_mux);
    dj_link_browse_mem_t *mem = s_browse;
    s_browse = NULL;
    portEXIT_CRITICAL(&s_mux);
    heap_caps_free(mem);
}

/* Serve a selection: new generation, empty list, client not started yet. */
static void dj_link_browse_restart(uint8_t peer)
{
    dj_link_browse_halt();
    s_browse_peer = peer;
    s_browse_logged = DJ_LINK_BROWSE_OFF;
    if (peer == 0u) {
        dj_link_browse_free();
        portENTER_CRITICAL(&s_mux);
        uint32_t generation = s_browse_status.generation + 1u;
        memset(&s_browse_status, 0, sizeof(s_browse_status));
        s_browse_status.generation = generation;
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    dj_link_browse_mem_t *mem = s_browse;
    if (!mem) {
        mem = heap_caps_calloc(1, sizeof(*mem), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (mem) {
        static const dj_link_db_io_t io = {
            .connect = dj_link_db_io_connect,
            .send = dj_link_db_io_send,
            .close = dj_link_db_io_close,
            .list_begin = dj_link_db_io_list_begin,
            .track = dj_link_db_io_track,
            .next_detail = dj_link_db_io_next_detail,
            .path = dj_link_db_io_path,
            .blob = dj_link_db_io_blob,
        };
        dj_link_db_init(&mem->client, &io, DJ_LINK_BROWSE_MAX_TRACKS);
        /* v249: every row is loadable through the NFS fetch job. */
        mem->client.audio = DJ_LINK_PEER_AUDIO_NFS;
    }
    portENTER_CRITICAL(&s_mux);
    const uint8_t menu = s_browse_menu_want;
    const uint32_t menu_id = s_browse_menu_id_want;
    /* v311: folders and playlists keep the player's order. */
    const bool all = menu == DJ_LINK_DB_MENU_ALL_TRACKS;
    const uint8_t sort = all ? s_browse_sort_want : DJ_LINK_DB_SORT_DEFAULT;
    const bool desc = all && s_browse_desc_want;
    portEXIT_CRITICAL(&s_mux);
    if (mem) {
        dj_link_db_set_sort(&mem->client, sort);
        dj_link_db_set_menu(&mem->client, (dj_link_db_menu_t)menu, menu_id);
    }
    s_browse_desc = desc;
    s_browse_rows = 0u;
    const dj_link_peer_t *p = dj_link_find_peer(peer);
    portENTER_CRITICAL(&s_mux);
    s_browse = mem;
    uint32_t generation = s_browse_status.generation + 1u;
    memset(&s_browse_status, 0, sizeof(s_browse_status));
    s_browse_status.generation = generation;
    s_browse_status.peer = peer;
    s_browse_status.state = mem ? DJ_LINK_BROWSE_WAITING : DJ_LINK_BROWSE_FAILED;
    s_browse_status.sort = sort;
    s_browse_status.sort_desc = desc;
    s_browse_status.menu = menu;
    s_browse_status.menu_id = all ? 0u : menu_id;
    if (p) {
        memcpy(s_browse_status.peer_name, p->name, sizeof(s_browse_status.peer_name));
    }
    if (!mem) {
        snprintf(s_browse_status.error, sizeof(s_browse_status.error), "NO MEMORY");
    }
    portEXIT_CRITICAL(&s_mux);
    if (!mem) {
        ESP_LOGW(TAG, "browse #%u: no PSRAM for %u rows", (unsigned)peer,
                 (unsigned)DJ_LINK_BROWSE_MAX_TRACKS);
    }
}

/* Connection events and received bytes into the client. A close from inside
 * the client (it hops from the discovery port to the dbserver port) bumps
 * s_tcp_gen, and whatever was read before belongs to the old connection. */
static void dj_link_browse_pump(dj_link_db_t *c, uint32_t now_ms)
{
    uint32_t gen = s_tcp_gen;
    unsigned events = atomic_exchange_explicit(&s_tcp_events, 0u, memory_order_acquire);
    if (events & DJ_LINK_TCP_EVT_CONNECTED) {
        dj_link_db_on_connected(c, now_ms);
    }
    while (gen == s_tcp_gen) {
        unsigned tail = atomic_load_explicit(&s_tcp_tail, memory_order_relaxed);
        unsigned head = atomic_load_explicit(&s_tcp_head, memory_order_acquire);
        if (tail == head) {
            break;
        }
        size_t at = tail % DJ_LINK_TCP_RING_BYTES;
        size_t n = DJ_LINK_TCP_RING_BYTES - at;
        if (n > head - tail) {
            n = head - tail;
        }
        /* Never more than the client can buffer: a reply split across
         * chunks is fine; one larger than its buffer fails it cleanly. */
        size_t room = sizeof(c->rx) - c->rx_len;
        if (n > room) {
            n = room ? room : 1u;
        }
        dj_link_db_on_data(c, &s_browse->tcp_ring[at], n, now_ms);
        if (gen != s_tcp_gen) {
            break;
        }
        /* Released only now: the producer cannot overwrite bytes in use. */
        atomic_store_explicit(&s_tcp_tail, tail + (unsigned)n, memory_order_release);
    }
    if ((events & DJ_LINK_TCP_EVT_CLOSED) && gen == s_tcp_gen) {
        dj_link_tcp_close();
        dj_link_db_on_closed(c, now_ms);
    }
}

static void dj_link_browse_service(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t want = s_browse_want;
    uint32_t req = s_browse_req;
    portEXIT_CRITICAL(&s_mux);
    if (req != s_browse_req_done) {
        s_browse_req_done = req;
        dj_link_browse_restart(want);
    }
    if (!s_browse || s_browse_peer == 0u) {
        return;
    }
    dj_link_db_t *c = &s_browse->client;
    if (!s_browse_started) {
        /* The dbserver wants a real player number, and the peer's IP comes
         * from its keep-alive / beat / status packets. */
        const dj_link_peer_t *p = dj_link_find_peer(s_browse_peer);
        uint8_t ours = dj_link_session_number(&s_session[0]);
        if (!p || p->ip == 0u || ours == 0u) {
            return;
        }
        s_browse_started = true;
        portENTER_CRITICAL(&s_mux);
        memcpy(s_browse_status.peer_name, p->name, sizeof(s_browse_status.peer_name));
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "browse #%u %s: dbserver session as #%u", (unsigned)s_browse_peer,
                 p->name, (unsigned)ours);
        /* v297: rekordbox serves its collection (slot 4); its rows are
         * fetched over NFS too, from a path the dbserver gives (0x2102). */
        bool collection = p->device_type == DJ_LINK_DEVICE_TYPE_REKORDBOX;
        dj_link_db_start(c, p->ip, s_browse_peer,
                         collection ? DJLINK_SLOT_LAPTOP : DJLINK_SLOT_USB, ours, now_ms);
    }
    dj_link_browse_pump(c, now_ms);
    dj_link_db_poll(c, now_ms);

    dj_link_browse_state_t state = DJ_LINK_BROWSE_LOADING;
    if (dj_link_db_phase(c) == DJ_LINK_DB_FAILED) {
        state = DJ_LINK_BROWSE_FAILED;
    } else if (dj_link_db_list_done(c)) {
        state = DJ_LINK_BROWSE_LISTED;
    }
    portENTER_CRITICAL(&s_mux);
    s_browse_status.state = state;
    if (state == DJ_LINK_BROWSE_LISTED && s_browse_desc) {
        s_browse_status.count = s_browse_rows;
    }
    if (state == DJ_LINK_BROWSE_FAILED) {
        memcpy(s_browse_status.error, c->error, sizeof(s_browse_status.error));
    }
    uint32_t count = s_browse_status.count;
    portEXIT_CRITICAL(&s_mux);
    if (state != s_browse_logged) {
        s_browse_logged = state;
        if (state == DJ_LINK_BROWSE_FAILED) {
            ESP_LOGW(TAG, "browse #%u failed: %s (select it again to retry)",
                     (unsigned)s_browse_peer, dj_link_db_error(c));
        } else if (state == DJ_LINK_BROWSE_LISTED) {
            ESP_LOGW(TAG, "browse #%u: %u rows listed - LOAD downloads over NFS",
                     (unsigned)s_browse_peer, (unsigned)count);
        }
    }
}

/* ---- v249: peer track download (NFS -> SD cache) ------------------------ */

/* The dbserver lists a peer's tracks but never says where the audio lives:
 * the job first downloads the peer's export.pdb (once per browse selection)
 * and resolves the rekordbox id to a file path (dj_link_pdb), then downloads
 * that file. v297: a rekordbox source has no export.pdb; its dbserver gives
 * the path (track info 0x2102) through the browse session instead, and its
 * portmapper listens on Pioneer's unprivileged 50111. Both go through FETCH.TMP and are renamed when complete, so a
 * cancelled or failed fetch never leaves a truncated file under a real name.
 * Everything runs in the dj_link task: datagrams are copied out of the pbufs
 * in the tcpip thread like the other UDP ports, and the SD writes are bounded
 * DJ_LINK_FETCH_STAGE batches under sd_io_gate, as the recorder does. */
#define DJ_LINK_CACHE_DIR       "/sd/djlcache"
#define DJ_LINK_CACHE_PDB       DJ_LINK_CACHE_DIR "/PEER.PDB"
#define DJ_LINK_CACHE_TMP       DJ_LINK_CACHE_DIR "/FETCH.TMP"
#define DJ_LINK_PDB_PATH        "PIONEER/rekordbox/export.pdb"
#define DJ_LINK_PDB_PATH_HFS    ".PIONEER/rekordbox/export.pdb" /* HFS+ media */
/* No IP reassembly in our lwIP: every READ reply must fit one frame. */
#define DJ_LINK_FETCH_READ      DJLINK_NFS_READ_DEFAULT
#define DJ_LINK_FETCH_WINDOW    4u
#define DJ_LINK_FETCH_RX_SLOTS  8u      /* > window: a whole burst fits */
#define DJ_LINK_FETCH_RX_MAX    1536u   /* larger datagrams are dropped */
#define DJ_LINK_FETCH_STAGE     65536u  /* bytes staged (v318: 64 KB) */
#define DJ_LINK_FETCH_PDB_MAX   (64u * 1024u * 1024u)
#define DJ_LINK_FETCH_AUDIO_MAX (1024u * 1024u * 1024u)
#define DJ_LINK_FETCH_PRUNE_MAX 8u      /* files removed per directory pass */
#define DJ_LINK_FETCH_PATH_MS   10000u  /* v297: dbserver path answer, reconnect included */
#define DJ_LINK_FETCH_ANALYSIS_MS 10000u /* v300: all analysis answers */
#define DJ_LINK_FETCH_REFRESH_MS   4000u /* v303: same, the analysis already cached */
#define DJ_LINK_FETCH_CUES_MAX    (8u * 1024u) /* v303: 64 entries of 124 bytes */
#define DJ_LINK_FETCH_ART_MAX     (64u * 1024u) /* v300: = UI_ARTWORK_FILE_MAX */
#define DJ_LINK_REKORDBOX_PMAP  50111u

typedef struct {
    uint16_t len;
    uint8_t  data[DJ_LINK_FETCH_RX_MAX];
} dj_link_fetch_rx_t;

/* ~88 KB of PSRAM (v318: the 64 KB write stage is a separate aligned block,
 * s_fetch_stage), allocated per fetch and freed when it ends. */
typedef struct {
    djlink_nfs_t nfs;
    dj_link_pdb_track_t track;
    dj_link_fetch_rx_t rx[DJ_LINK_FETCH_RX_SLOTS];   /* written by the tcpip thread */
    uint8_t window_buf[DJ_LINK_FETCH_WINDOW * DJ_LINK_FETCH_READ];
    uint8_t page[DJ_LINK_PDB_PAGE_MAX];
} dj_link_fetch_mem_t;

static dj_link_fetch_mem_t *s_fetch;   /* dj_link task; outlives s_fetch_pcb */
/* v318: the stage every SD write of a fetch goes out of, in one fwrite:
 * PSRAM, 64-byte aligned (the P4 cache line), 64 KB, so FATFS hands each
 * write to the card as one multi-sector DMA straight from PSRAM
 * (SOC_SDMMC_PSRAM_DMA_CAPABLE). Unaligned (pre-v317), the IDF SD driver
 * bounced every write through internal RAM; v317's 4 KB internal buffer
 * avoided that but made 8x more writes, each followed by the driver's
 * busy-wait on the card (CMD13 polling, SDMMC interrupts on CPU0). Every
 * write but a file's last starts and ends on a sector. Allocated per fetch. */
static uint8_t *s_fetch_stage;         /* dj_link task */
static struct udp_pcb *s_fetch_pcb;    /* tcpip thread only */
static atomic_uint s_fetch_rx_head;    /* written by the tcpip thread only */
static atomic_uint s_fetch_rx_tail;    /* written by the dj_link task only */
static atomic_uint s_fetch_peer_ip;    /* replies from any other host are dropped */

/* The running job, dj_link task only. state is IDLE, PDB or AUDIO. */
static struct {
    dj_link_fetch_state_t state;
    uint32_t id;
    uint8_t  peer;
    uint32_t rekordbox_id;
    uint32_t ip;
    uint32_t key;
    uint32_t keep[2];
    uint32_t generation;               /* browse generation at start */
    bool     hfs;                      /* PDB path retried as .PIONEER */
    bool     collection;               /* v297: rekordbox source, path from dbserver */
    bool     path_wait;                /* v297: dbserver path asked, not answered */
    uint32_t path_deadline_ms;
    bool     tmp_used;                 /* FETCH.TMP may exist */
    FILE    *file;
    size_t   staged;
    char     path[DJ_LINK_FETCH_PATH_MAX];
    /* v300: the analysis is asked from the browse session once the audio
     * is cached; wave / grid are PSRAM buffers the dbserver client streams
     * into, freed by dj_link_fetch_end. */
    bool     analysis_wait;
    uint32_t analysis_deadline_ms;
    bool     cache_hit;
    uint8_t *wave;
    size_t   wave_len;
    uint8_t *grid;
    size_t   grid_len;
    uint32_t artwork_id;               /* 0 = the peer listed none */
    bool     want_art;                 /* v302: <key>.JPG missing */
    uint8_t *art;                      /* JPEG, allocated when asked */
    size_t   art_len;
    /* v303: the analysis is asked on every load (a cached DAT is only
     * replaced by a complete answer, dj_link_anlz_commit), the cue list
     * too; answered = the peer replied with the expected type. */
    bool     dat_cached;
    uint8_t *cues;
    size_t   cues_len;
    bool     wave_answered;
    bool     grid_answered;
    bool     cues_answered;
    /* v314: the PWV4 colour preview section (0x2c04), after the cues; a
     * peer without one (or not answering) leaves the EXT mono. */
    uint8_t *color;
    size_t   color_len;
    /* v303: export.pdb as the peer serves it now (NFS GETATTR); unchanged =
     * the read was refused at open because the cached copy is that file. */
    dj_link_pdb_stamp_t pdb_stamp;
    bool     pdb_unchanged;
} s_job;

/* v303: which export.pdb is cached (peer + NFS size/mtime), dj_link task.
 * Checked against the peer's attributes on every fetch, not the browse
 * generation, so a source edit under the same selection is fetched. */
static dj_link_pdb_stamp_t s_pdb_cache;

static dj_link_fetch_status_t s_fetch_status; /* guarded by s_mux */
static bool     s_fetch_pending;       /* guarded by s_mux: start not taken yet */
static uint32_t s_fetch_cancel_id;     /* guarded by s_mux */
static uint32_t s_fetch_next_id;       /* guarded by s_mux */
static uint32_t s_fetch_req_keep[2];   /* guarded by s_mux */
static uint32_t s_fetch_req_artwork;   /* guarded by s_mux */

/* tcpip thread. */
static void dj_link_fetch_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                   const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)port;
    if (!p) {
        return;
    }
    unsigned head = atomic_load_explicit(&s_fetch_rx_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&s_fetch_rx_tail, memory_order_acquire);
    uint32_t src = addr ? lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr))) : 0u;
    bool queued = false;
    if (src != 0u && src == atomic_load_explicit(&s_fetch_peer_ip, memory_order_relaxed) &&
        p->tot_len <= DJ_LINK_FETCH_RX_MAX && head - tail < DJ_LINK_FETCH_RX_SLOTS) {
        dj_link_fetch_rx_t *slot = &s_fetch->rx[head % DJ_LINK_FETCH_RX_SLOTS];
        slot->len = pbuf_copy_partial(p, slot->data, p->tot_len, 0);
        atomic_store_explicit(&s_fetch_rx_head, head + 1u, memory_order_release);
        queued = true;
    }
    pbuf_free(p);
    /* A dropped reply is a lost datagram: the client retransmits it. */
    if (queued) {
        xTaskNotifyGive(s_rx_task);
    }
}

static esp_err_t dj_link_fetch_close_tcpip(void *ctx)
{
    (void)ctx;
    if (s_fetch_pcb) {
        udp_remove(s_fetch_pcb);
        s_fetch_pcb = NULL;
    }
    return ESP_OK;
}

/* Ephemeral port, bound to the Ethernet netif like the other PCBs. */
static esp_err_t dj_link_fetch_open_tcpip(void *ctx)
{
    (void)ctx;
    struct netif *nif = netif_get_by_index(s_netif_index);
    if (!nif || s_fetch_pcb) {
        return ESP_ERR_INVALID_STATE;
    }
    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        return ESP_ERR_NO_MEM;
    }
    if (udp_bind(pcb, IP4_ADDR_ANY, 0) != ERR_OK) {
        udp_remove(pcb);
        return ESP_FAIL;
    }
    udp_bind_netif(pcb, nif);
    udp_recv(pcb, dj_link_fetch_udp_recv, NULL);
    s_fetch_pcb = pcb;
    return ESP_OK;
}

typedef struct {
    uint32_t ip;    /* host order */
    uint16_t port;
    const uint8_t *buf;
    size_t len;
} dj_link_fetch_tx_t;

static esp_err_t dj_link_fetch_send_tcpip(void *ctx)
{
    const dj_link_fetch_tx_t *tx = ctx;
    struct netif *nif = s_fetch_pcb ? netif_get_by_index(s_fetch_pcb->netif_idx) : NULL;
    if (!nif) {
        return ESP_ERR_INVALID_STATE;
    }
    ip_addr_t addr = IPADDR4_INIT(lwip_htonl(tx->ip));
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)tx->len, PBUF_RAM);
    if (!p) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(p->payload, tx->buf, tx->len);
    err_t err = udp_sendto_if(s_fetch_pcb, p, &addr, tx->port, nif);
    pbuf_free(p);
    return err == ERR_OK ? ESP_OK : ESP_FAIL;
}

static int dj_link_fetch_io_send(void *ctx, uint32_t ip, uint16_t port,
                                 const uint8_t *buf, size_t len)
{
    (void)ctx;
    if (len == 0u || len > UINT16_MAX) {
        return -1;
    }
    dj_link_fetch_tx_t tx = { .ip = ip, .port = port, .buf = buf, .len = len };
    return esp_netif_tcpip_exec(dj_link_fetch_send_tcpip, &tx) == ESP_OK ? 0 : -1;
}

static void dj_link_fetch_publish_progress(uint32_t done, uint32_t total)
{
    portENTER_CRITICAL(&s_mux);
    s_fetch_status.done = done;
    s_fetch_status.total = total;
    s_fetch_status.percent = total ? (uint8_t)(((uint64_t)done * 100u) / total) : 100u;
    portEXIT_CRITICAL(&s_mux);
}

/* v316 DIAGNOSTIC (mode A, HIL of the v314/v315 crackles): 1 = a peer
 * track's audio is received and staged as usual but never written to the
 * SD card (no open, no fwrite), and the fetch then ends FAILED ("DIAG: NO
 * SD WRITE"), so nothing reaches the cache or a deck. Crackles still there
 * = the network side; gone = the SD write. export.pdb and the analysis
 * files are still written. Back to 0 for any normal build. */
#define DJ_LINK_FETCH_DIAG_NO_SD_WRITE 0

static bool dj_link_fetch_diag_discard(void)
{
    return DJ_LINK_FETCH_DIAG_NO_SD_WRITE && s_job.state == DJ_LINK_FETCH_AUDIO;
}

/* v319 diagnostic: SD write timings of the running fetch (dj_link task).
 * v323: logged in diagnostics builds only (SD_IO_DIAG_ENABLED). */
static struct {
    uint32_t writes;
    uint64_t write_us;
    uint32_t write_max_us;
    uint32_t over_2ms;
    uint32_t gate_max_us;
    uint64_t bytes;
} s_fetch_wstat;

static void dj_link_fetch_write_stat(uint32_t gate_us, uint32_t write_us, size_t bytes)
{
    s_fetch_wstat.writes++;
    s_fetch_wstat.write_us += write_us;
    s_fetch_wstat.bytes += bytes;
    if (write_us > s_fetch_wstat.write_max_us) s_fetch_wstat.write_max_us = write_us;
    if (gate_us > s_fetch_wstat.gate_max_us) s_fetch_wstat.gate_max_us = gate_us;
    if (write_us > 2000u) s_fetch_wstat.over_2ms++;
}

static void dj_link_fetch_write_stat_log(const char *what)
{
    if (SD_IO_DIAG_ENABLED && s_fetch_wstat.writes) {
        ESP_LOGW(TAG, "fetch #%u id %u SD writes (%s): %u x avg %u B, avg %u us, max %u us, "
                      "%u over 2 ms, gate wait max %u us",
                 (unsigned)s_job.peer, (unsigned)s_job.rekordbox_id, what,
                 (unsigned)s_fetch_wstat.writes,
                 (unsigned)(s_fetch_wstat.bytes / s_fetch_wstat.writes),
                 (unsigned)(s_fetch_wstat.write_us / s_fetch_wstat.writes),
                 (unsigned)s_fetch_wstat.write_max_us, (unsigned)s_fetch_wstat.over_2ms,
                 (unsigned)s_fetch_wstat.gate_max_us);
    }
    memset(&s_fetch_wstat, 0, sizeof(s_fetch_wstat));
    /* v321: card-busy waits since the last line (whole system, not only
     * this fetch): one CMD13 per tick, so polls ~ wait ms. */
    sd_idle_wait_stats_t idle;
    sd_idle_wait_take_stats(&idle);
    if (SD_IO_DIAG_ENABLED && idle.waits) {
        ESP_LOGW(TAG, "SD card busy waits: %u, %u polls, max %u us, %u timeouts",
                 (unsigned)idle.waits, (unsigned)idle.polls, (unsigned)idle.max_wait_us,
                 (unsigned)idle.timeouts);
    }
}

/* v321: write(2) until done (a short write is retried, an error fails). */
static bool dj_link_fetch_write_all(int fd, const uint8_t *data, size_t len)
{
    while (fd >= 0 && len > 0u) {
        const ssize_t n = write(fd, data, len);
        if (n <= 0) {
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return fd >= 0;
}

static bool dj_link_fetch_flush(void)
{
    if (s_job.staged == 0u) {
        return true;
    }
    if (dj_link_fetch_diag_discard()) {
        s_job.staged = 0u;
        return true;
    }
    bool ok = s_job.file != NULL;
    if (ok) {
        /* v319 diagnostic: the gate wait and the write itself, timed; the
         * write is marked for the audio mix probe (sd_io_gate_diag_*).
         * v321: one write(2) of the whole stage on the file's descriptor (no
         * stdio buffering in between: FATFS gets the aligned 64 KB block and
         * sends its whole sectors by DMA from PSRAM). */
        const int64_t t0 = esp_timer_get_time();
        sd_io_gate_begin();
        const int64_t t1 = esp_timer_get_time();
        sd_io_gate_diag_begin(SD_IO_DIAG_WRITE);
        ok = dj_link_fetch_write_all(fileno(s_job.file), s_fetch_stage, s_job.staged);
        sd_io_gate_diag_end(SD_IO_DIAG_WRITE);
        const int64_t t2 = esp_timer_get_time();
        sd_io_gate_end();
        dj_link_fetch_write_stat((uint32_t)(t1 - t0), (uint32_t)(t2 - t1), s_job.staged);
        /* A tick for the decks' SD reads between two writes (dj_link task,
         * CPU1, prio 1: nothing taken from the audio). */
        vTaskDelay(1);
    }
    s_job.staged = 0u;
    return ok;
}

/* The temp file every fetch asset is staged into. v321: written only with
 * write(2) on its descriptor (dj_link_fetch_flush), never through stdio. */
static FILE *dj_link_fetch_open_tmp(void)
{
    return fopen(DJ_LINK_CACHE_TMP, "wb");
}

static bool dj_link_fetch_cached(const char *path);

static int dj_link_fetch_io_open(void *ctx, uint32_t size)
{
    (void)ctx;
    if (s_job.state == DJ_LINK_FETCH_PDB) {
        /* v303: the lookup and GETATTR are done, nothing read yet: keep the
         * cached copy if it is still this file (dj_link_fetch_failed). */
        const djlink_nfs_fattr_t *a = &s_fetch->nfs.attr;
        s_job.pdb_stamp = (dj_link_pdb_stamp_t){ true, s_job.ip, s_job.peer, size,
                                                 a->mtime_s, a->mtime_us };
        if (dj_link_pdb_stamp_matches(&s_pdb_cache, &s_job.pdb_stamp) &&
            dj_link_fetch_cached(DJ_LINK_CACHE_PDB)) {
            s_job.pdb_unchanged = true;
            return -1;
        }
    }
    s_job.staged = 0u;
    if (dj_link_fetch_diag_discard()) {
        ESP_LOGW(TAG, "fetch #%u id %u: DIAG mode A - %u bytes received, not written",
                 (unsigned)s_job.peer, (unsigned)s_job.rekordbox_id, (unsigned)size);
        dj_link_fetch_publish_progress(0u, size);
        return 0;
    }
    sd_io_gate_begin();
    s_job.tmp_used = true;
    s_job.file = dj_link_fetch_open_tmp();
    sd_io_gate_end();
    dj_link_fetch_publish_progress(0u, size);
    return s_job.file ? 0 : -1;
}

static int dj_link_fetch_io_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)offset;
    while (len > 0u) {
        size_t n = DJ_LINK_FETCH_STAGE - s_job.staged;
        if (n > len) {
            n = len;
        }
        memcpy(&s_fetch_stage[s_job.staged], data, n);
        s_job.staged += n;
        data += n;
        len -= n;
        if (s_job.staged == DJ_LINK_FETCH_STAGE && !dj_link_fetch_flush()) {
            return -1;
        }
    }
    return 0;
}

static void dj_link_fetch_io_progress(void *ctx, uint32_t done, uint32_t total)
{
    (void)ctx;
    dj_link_fetch_publish_progress(done, total);
}

/* Close the temp file; it is deleted unless it was just renamed. */
static void dj_link_fetch_close_tmp(void)
{
    if (!s_job.tmp_used) {
        return;
    }
    sd_io_gate_begin();
    if (s_job.file) {
        fclose(s_job.file);
        s_job.file = NULL;
    }
    unlink(DJ_LINK_CACHE_TMP);
    sd_io_gate_end();
    s_job.staged = 0u;
    s_job.tmp_used = false;
}

/* Finish the job: close the PCB (no callback in flight afterwards), then
 * free the memory and publish the outcome. */
static void dj_link_fetch_end(dj_link_fetch_state_t state, const char *error, bool cache_hit)
{
    esp_netif_tcpip_exec(dj_link_fetch_close_tcpip, NULL);
    atomic_store(&s_fetch_peer_ip, 0u);
    dj_link_fetch_close_tmp();
    /* The dbserver client must let go of the analysis buffers first. */
    if (s_browse && s_browse->client.blob_dst &&
        (s_browse->client.blob_dst == s_job.wave || s_browse->client.blob_dst == s_job.grid ||
         s_browse->client.blob_dst == s_job.cues || s_browse->client.blob_dst == s_job.art ||
         s_browse->client.blob_dst == s_job.color)) {
        dj_link_db_cancel_blob(&s_browse->client);
    }
    s_job.analysis_wait = false;
    heap_caps_free(s_job.wave);
    heap_caps_free(s_job.grid);
    heap_caps_free(s_job.cues);
    heap_caps_free(s_job.art);
    heap_caps_free(s_job.color);
    s_job.color = NULL;
    s_job.color_len = 0u;
    s_job.wave = NULL;
    s_job.grid = NULL;
    s_job.cues = NULL;
    s_job.art = NULL;
    heap_caps_free(s_fetch);
    s_fetch = NULL;
    heap_caps_free(s_fetch_stage);
    s_fetch_stage = NULL;
    portENTER_CRITICAL(&s_mux);
    if (s_fetch_status.id == s_job.id) {
        s_fetch_status.state = state;
        s_fetch_status.cache_hit = cache_hit;
        s_fetch_status.error[0] = '\0';
        if (state == DJ_LINK_FETCH_DONE) {
            s_fetch_status.track_key = s_job.key;
            memcpy(s_fetch_status.path, s_job.path, sizeof(s_fetch_status.path));
        } else if (error) {
            snprintf(s_fetch_status.error, sizeof(s_fetch_status.error), "%s", error);
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (state == DJ_LINK_FETCH_DONE) {
        ESP_LOGW(TAG, "fetch #%u id %u -> %s%s", (unsigned)s_job.peer,
                 (unsigned)s_job.rekordbox_id, s_job.path, cache_hit ? " (cached)" : "");
    } else {
        ESP_LOGW(TAG, "fetch #%u id %u %s: %s", (unsigned)s_job.peer,
                 (unsigned)s_job.rekordbox_id,
                 state == DJ_LINK_FETCH_CANCELLED ? "cancelled" : "failed",
                 error ? error : "?");
    }
    s_job.state = DJ_LINK_FETCH_IDLE;
}

static bool dj_link_fetch_nfs_start(const char *path, uint32_t max_size, uint32_t now_ms)
{
    static const djlink_nfs_io_t io = {
        .send = dj_link_fetch_io_send,
        .open = dj_link_fetch_io_open,
        .write = dj_link_fetch_io_write,
        .progress = dj_link_fetch_io_progress,
    };
    const djlink_nfs_fetch_cfg_t cfg = {
        .host_ip = s_job.ip,
        .export_path = DJLINK_NFS_EXPORT_USB,
        .path = path,
        .charset = DJLINK_NFS_NAMES_UTF16LE,
        .portmap_port = s_job.collection ? DJ_LINK_REKORDBOX_PMAP : 0u,
        .read_size = DJ_LINK_FETCH_READ,
        .window = DJ_LINK_FETCH_WINDOW,
        .window_buf = s_fetch->window_buf,
        .window_buf_len = sizeof(s_fetch->window_buf),
        .max_size = max_size,
        .xid_seed = (uint32_t)esp_timer_get_time(),
    };
    dj_link_fetch_publish_progress(0u, 0u);
    return djlink_nfs_fetch(&s_fetch->nfs, &cfg, &io, now_ms) == DJLINK_NFS_E_NONE;
}

static bool dj_link_fetch_pdb_read(void *ctx, uint32_t offset, uint8_t *dst, size_t len)
{
    FILE *f = ctx;
    sd_io_gate_begin();
    bool ok = fseek(f, (long)offset, SEEK_SET) == 0 && fread(dst, 1u, len, f) == len;
    sd_io_gate_end();
    return ok;
}

/* Walks the cached export.pdb page by page; each page read holds the gate
 * only for itself. */
static dj_link_pdb_result_t dj_link_fetch_resolve(void)
{
    struct stat st;
    sd_io_gate_begin();
    FILE *f = fopen(DJ_LINK_CACHE_PDB, "rb");
    bool ok = f && stat(DJ_LINK_CACHE_PDB, &st) == 0 && S_ISREG(st.st_mode);
    sd_io_gate_end();
    dj_link_pdb_result_t r = DJ_LINK_PDB_READ_ERROR;
    if (ok) {
        r = dj_link_pdb_find_track(dj_link_fetch_pdb_read, f, (uint32_t)st.st_size,
                                   s_job.rekordbox_id, s_fetch->page,
                                   sizeof(s_fetch->page), &s_fetch->track);
    }
    if (f) {
        sd_io_gate_begin();
        fclose(f);
        sd_io_gate_end();
    }
    return r;
}

static bool dj_link_fetch_cached(const char *path)
{
    struct stat st;
    sd_io_gate_begin();
    bool ok = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    sd_io_gate_end();
    return ok;
}

/* Keep export.pdb, the track being fetched and the keep keys (files the
 * decks have loaded); delete everything else in the cache directory. The
 * names are collected first so nothing is unlinked mid-readdir. */
static void dj_link_fetch_prune(void)
{
    char keep[3][9];
    const uint32_t keys[3] = { s_job.key, s_job.keep[0], s_job.keep[1] };
    for (size_t i = 0; i < 3u; i++) {
        keep[i][0] = '\0';
        if (keys[i]) {
            snprintf(keep[i], sizeof(keep[i]), "%08" PRIX32, keys[i]);
        }
    }
    char doomed[DJ_LINK_FETCH_PRUNE_MAX][32];
    char path[sizeof(DJ_LINK_CACHE_DIR) + 1u + sizeof(doomed[0]) + 8u];
    size_t count;
    do {
        count = 0u;
        bool more = false;
        sd_io_gate_begin();
        DIR *dir = opendir(DJ_LINK_CACHE_DIR);
        struct dirent *e;
        while (dir && (e = readdir(dir)) != NULL) {
            const char *name = e->d_name;
            size_t len = strlen(name);
            if (name[0] == '.' || len >= sizeof(doomed[0]) ||
                strcasecmp(name, "PEER.PDB") == 0) {
                continue;
            }
            bool kept = false;
            for (size_t i = 0; i < 3u && !kept; i++) {
                kept = keep[i][0] && len > 9u && name[8] == '.' &&
                       strncasecmp(name, keep[i], 8u) == 0;
            }
            if (kept) {
                continue;
            }
            if (count == DJ_LINK_FETCH_PRUNE_MAX) {
                more = true;
                break;
            }
            memcpy(doomed[count++], name, len + 1u);
        }
        if (dir) {
            closedir(dir);
        }
        for (size_t i = 0; i < count; i++) {
            snprintf(path, sizeof(path), "%s/%.31s", DJ_LINK_CACHE_DIR, doomed[i]);
            if (unlink(path) != 0) {
                more = false; /* stuck file: do not loop on it */
            }
        }
        sd_io_gate_end();
        if (!more) {
            break;
        }
    } while (count > 0u);
}

static void dj_link_fetch_file(uint32_t now_ms);
static void dj_link_fetch_analysis(bool cache_hit, uint32_t now_ms);

/* export.pdb is in the cache: find the file, then download it (or not). */
static void dj_link_fetch_audio(uint32_t now_ms)
{
    dj_link_pdb_result_t r = dj_link_fetch_resolve();
    if (r != DJ_LINK_PDB_FOUND) {
        if (r != DJ_LINK_PDB_NOT_FOUND) {
            s_pdb_cache.valid = false;
        }
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED,
                          r == DJ_LINK_PDB_NOT_FOUND    ? "TRACK NOT IN EXPORT.PDB" :
                          r == DJ_LINK_PDB_BAD_FILE     ? "BAD EXPORT.PDB" :
                          r == DJ_LINK_PDB_BUFFER_SMALL ? "EXPORT.PDB PAGE TOO BIG" :
                                                          "CACHE READ FAILED",
                          false);
        return;
    }
    dj_link_fetch_file(now_ms);
}

/* s_fetch->track.file_path is known: download it unless it is cached. */
static void dj_link_fetch_file(uint32_t now_ms)
{
    char ext[8];
    dj_link_pdb_extension(s_fetch->track.file_path, ext, sizeof(ext));
    if (!ext[0]) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "NO FILE EXTENSION", false);
        return;
    }
    snprintf(s_job.path, sizeof(s_job.path), "%s/%08" PRIX32 ".%s", DJ_LINK_CACHE_DIR,
             s_job.key, ext);
    if (dj_link_fetch_cached(s_job.path)) {
        dj_link_fetch_analysis(true, now_ms);
        return;
    }
    dj_link_fetch_prune();
    s_job.state = DJ_LINK_FETCH_AUDIO;
    portENTER_CRITICAL(&s_mux);
    s_fetch_status.state = DJ_LINK_FETCH_AUDIO;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "fetch #%u id %u: %s", (unsigned)s_job.peer,
             (unsigned)s_job.rekordbox_id, s_fetch->track.file_path);
    if (!dj_link_fetch_nfs_start(s_fetch->track.file_path, DJ_LINK_FETCH_AUDIO_MAX, now_ms)) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, djlink_nfs_error_text(&s_fetch->nfs), false);
    }
}

static bool dj_link_fetch_sd_ready(void)
{
    struct stat st;
    return stat("/sd", &st) == 0 && S_ISDIR(st.st_mode);
}

static void dj_link_fetch_begin(uint32_t id, uint8_t peer, uint32_t rekordbox_id,
                                uint32_t artwork_id, const uint32_t keep[2],
                                uint32_t generation, uint32_t now_ms)
{
    memset(&s_job, 0, sizeof(s_job));
    s_job.state = DJ_LINK_FETCH_PDB;
    s_job.id = id;
    s_job.peer = peer;
    s_job.rekordbox_id = rekordbox_id;
    s_job.artwork_id = artwork_id;
    s_job.keep[0] = keep[0];
    s_job.keep[1] = keep[1];
    s_job.generation = generation;

    const dj_link_peer_t *p = dj_link_find_peer(peer);
    if (!p || p->ip == 0u) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "PLAYER NOT ON THE NETWORK", false);
        return;
    }
    s_job.ip = p->ip;
    s_job.key = dj_link_peer_track_key(p->ip, peer, rekordbox_id);
    if (!dj_link_fetch_sd_ready()) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "NO SD CARD", false);
        return;
    }
    /* The master recording owns the card's bandwidth. */
    if (sd_io_gate_recorder_active()) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "RECORDING TO SD", false);
        return;
    }
    sd_io_gate_begin();
    bool dir_ok = mkdir(DJ_LINK_CACHE_DIR, 0775) == 0 || errno == EEXIST;
    sd_io_gate_end();
    if (!dir_ok) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "CACHE DIR FAILED", false);
        return;
    }
    s_fetch = heap_caps_calloc(1, sizeof(*s_fetch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fetch) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "NO MEMORY", false);
        return;
    }
    s_fetch_stage = heap_caps_aligned_alloc(64u, DJ_LINK_FETCH_STAGE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fetch_stage) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "NO MEMORY", false);
        return;
    }
    /* No PCB yet, so the rx ring has no producer: safe to rewind. */
    atomic_store(&s_fetch_rx_head, 0u);
    atomic_store(&s_fetch_rx_tail, 0u);
    atomic_store(&s_fetch_peer_ip, s_job.ip);
    if (esp_netif_tcpip_exec(dj_link_fetch_open_tcpip, NULL) != ESP_OK) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "UDP OPEN FAILED", false);
        return;
    }
    /* v297: a rekordbox source answers the path itself, every time (so a
     * cache hit is still checked against what the source serves now). */
    s_job.collection = p->device_type == DJ_LINK_DEVICE_TYPE_REKORDBOX;
    if (s_job.collection) {
        /* The answer arrives through the browse pump: dj_link_fetch_on_path. */
        s_job.path_wait = true;
        s_job.path_deadline_ms = now_ms + DJ_LINK_FETCH_PATH_MS;
        if (!s_browse || s_browse_peer != peer || !s_browse_started ||
            !dj_link_db_want_path(&s_browse->client, rekordbox_id, now_ms)) {
            dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "SOURCE DB NOT READY", false);
            return;
        }
        ESP_LOGW(TAG, "fetch #%u id %u: path from the dbserver", (unsigned)peer,
                 (unsigned)rekordbox_id);
        return;
    }
    /* A file cached from older media may belong to a track that reused the
     * rekordbox id: cache hits need the peer's current export.pdb. v303: it
     * is looked up every time; the open hook keeps the cached copy when the
     * peer's size and mtime still match it (dj_link_fetch_io_open). */
    ESP_LOGW(TAG, "fetch #%u id %u: export.pdb first", (unsigned)peer, (unsigned)rekordbox_id);
    if (!dj_link_fetch_nfs_start(DJ_LINK_PDB_PATH, DJ_LINK_FETCH_PDB_MAX, now_ms)) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, djlink_nfs_error_text(&s_fetch->nfs), false);
    }
}

/* The NFS transfer finished: commit FETCH.TMP under its real name. */
static void dj_link_fetch_commit(uint32_t now_ms)
{
    bool pdb = s_job.state == DJ_LINK_FETCH_PDB;
    bool ok = dj_link_fetch_flush();
    sd_io_gate_begin();
    if (!s_job.file || fclose(s_job.file) != 0) {
        ok = false;
    }
    s_job.file = NULL;
    const char *dest = pdb ? DJ_LINK_CACHE_PDB : s_job.path;
    if (pdb) {
        s_pdb_cache.valid = false; /* the old copy goes first */
    }
    if (ok) {
        unlink(dest); /* FAT rename does not replace */
        ok = rename(DJ_LINK_CACHE_TMP, dest) == 0;
    }
    sd_io_gate_end();
    if (!ok) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "CACHE WRITE FAILED", false);
        return;
    }
    s_job.tmp_used = false;
    if (!pdb) {
        dj_link_fetch_analysis(false, now_ms);
        return;
    }
    s_pdb_cache = s_job.pdb_stamp;
    dj_link_fetch_audio(now_ms);
}

static void dj_link_fetch_failed(uint32_t now_ms)
{
    const djlink_nfs_t *c = &s_fetch->nfs;
    dj_link_fetch_close_tmp();
    if (s_job.state == DJ_LINK_FETCH_PDB && s_job.pdb_unchanged) {
        ESP_LOGW(TAG, "fetch #%u: export.pdb unchanged, cached copy kept", (unsigned)s_job.peer);
        dj_link_fetch_audio(now_ms);
        return;
    }
    /* HFS+ media keep the rekordbox folder as ".PIONEER" (crate-digger). */
    if (s_job.state == DJ_LINK_FETCH_PDB && !s_job.hfs &&
        c->error == DJLINK_NFS_E_LOOKUP && c->status == DJLINK_NFSERR_NOENT) {
        s_job.hfs = true;
        if (dj_link_fetch_nfs_start(DJ_LINK_PDB_PATH_HFS, DJ_LINK_FETCH_PDB_MAX, now_ms)) {
            return;
        }
    }
    char error[sizeof(s_fetch_status.error)];
    snprintf(error, sizeof(error), "%s", djlink_nfs_error_text(c));
    dj_link_fetch_end(DJ_LINK_FETCH_FAILED, error, false);
}

/* v297: the browse session answered a path request (path "" = unknown).
 * dj_link task, from dj_link_browse_service. */
static void dj_link_fetch_on_path(uint32_t rekordbox_id, const char *path)
{
    if (s_job.state != DJ_LINK_FETCH_PDB || !s_job.collection || !s_fetch ||
        rekordbox_id != s_job.rekordbox_id) {
        return;
    }
    s_job.path_wait = false;
    if (!path || path[0] == '\0') {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "NO PATH FROM SOURCE", false);
        return;
    }
    if (strlen(path) >= sizeof(s_fetch->track.file_path)) {
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "PATH TOO LONG", false);
        return;
    }
    snprintf(s_fetch->track.file_path, sizeof(s_fetch->track.file_path), "%s", path);
    dj_link_fetch_file(dj_link_now_ms());
}

/* v300: <key>.DAT / <key>.EXT / <key>.JPG beside the cached audio
 * <key>.<ext>. */
static void dj_link_fetch_anlz_path(char *out, size_t cap, const char *ext)
{
    snprintf(out, cap, "%s/%08" PRIX32 ".%s", DJ_LINK_CACHE_DIR, s_job.key, ext);
}

static bool dj_link_fetch_anlz_sink(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    return dj_link_fetch_io_write(NULL, 0u, data, len) == 0;
}

typedef enum { DJ_LINK_ASSET_DAT, DJ_LINK_ASSET_EXT, DJ_LINK_ASSET_JPG } dj_link_asset_t;

/* One ANLZ or artwork file through FETCH.TMP and the stage buffer, renamed
 * when complete, like the downloads. */
static bool dj_link_fetch_write_anlz(dj_link_asset_t kind, bool cue_lists)
{
    static const char *const ext_of[] = { "DAT", "EXT", "JPG" };
    char dest[DJ_LINK_FETCH_PATH_MAX];
    dj_link_fetch_anlz_path(dest, sizeof(dest), ext_of[kind]);
    sd_io_gate_begin();
    s_job.tmp_used = true;
    s_job.file = dj_link_fetch_open_tmp();
    sd_io_gate_end();
    s_job.staged = 0u;
    bool ok = s_job.file != NULL;
    if (ok) {
        const uint8_t *color = NULL;
        size_t color_len = 0u;
        if (kind == DJ_LINK_ASSET_EXT &&
            !dj_link_anlz_color_entries(s_job.color, s_job.color_len, &color, &color_len)) {
            color = NULL;
            color_len = 0u;
        }
        ok = kind == DJ_LINK_ASSET_EXT
                 ? dj_link_anlz_write_ext(dj_link_fetch_anlz_sink, NULL,
                                          s_job.wave, s_job.wave_len, color, color_len)
             : kind == DJ_LINK_ASSET_JPG
                 ? dj_link_fetch_anlz_sink(NULL, s_job.art, s_job.art_len)
                 : dj_link_anlz_write_dat(dj_link_fetch_anlz_sink, NULL, s_job.path,
                                          s_job.grid, s_job.grid_len,
                                          s_job.wave, s_job.wave_len,
                                          cue_lists ? s_job.cues : NULL, s_job.cues_len);
        ok = dj_link_fetch_flush() && ok;
    }
    s_job.staged = 0u;
    sd_io_gate_begin();
    if (!s_job.file || fclose(s_job.file) != 0) {
        ok = false;
    }
    s_job.file = NULL;
    if (ok) {
        unlink(dest); /* FAT rename does not replace */
        ok = rename(DJ_LINK_CACHE_TMP, dest) == 0;
    }
    if (!ok) {
        unlink(DJ_LINK_CACHE_TMP);
    }
    sd_io_gate_end();
    s_job.tmp_used = false;
    return ok;
}

/* v300: write what the peer answered (EXT and JPG first: the DAT marks the
 * analysis as cached), then finish. A track without analysis still loads.
 * v303: a cached analysis is only replaced by a complete answer
 * (dj_link_anlz_commit); otherwise the deck keeps the cached files. */
static void dj_link_fetch_analysis_done(void)
{
    s_job.analysis_wait = false;
    memset(&s_fetch_wstat, 0, sizeof(s_fetch_wstat));
    const size_t beats = dj_link_anlz_grid_count(s_job.grid, s_job.grid_len);
    const size_t cues = dj_link_anlz_cue_count(s_job.cues, s_job.cues_len);
    const dj_link_anlz_answers_t answers = {
        .dat_cached = s_job.dat_cached,
        .wave_answered = s_job.wave_answered,
        .grid_answered = s_job.grid_answered,
        .cues_answered = s_job.cues_answered && s_job.cues != NULL,
        .wave_len = s_job.wave_len,
        .beats = beats,
        .cues = cues,
    };
    const dj_link_anlz_commit_t plan = dj_link_anlz_commit(&answers);
    bool jpg = s_job.art_len > 0u;
    if (plan.drop_ext) {
        char ext[DJ_LINK_FETCH_PATH_MAX];
        dj_link_fetch_anlz_path(ext, sizeof(ext), "EXT");
        sd_io_gate_begin();
        unlink(ext);
        sd_io_gate_end();
    }
    bool ok = (!plan.write_ext || dj_link_fetch_write_anlz(DJ_LINK_ASSET_EXT, false)) &&
              (!jpg || dj_link_fetch_write_anlz(DJ_LINK_ASSET_JPG, false)) &&
              (!plan.write_dat || dj_link_fetch_write_anlz(DJ_LINK_ASSET_DAT, plan.cue_lists));
    dj_link_fetch_write_stat_log("analysis");
    ESP_LOGW(TAG, "fetch #%u id %u analysis: wave %u B, colour %u B, %u beats, %u cues%s, "
                  "artwork %u B (id %u), %s%s",
             (unsigned)s_job.peer, (unsigned)s_job.rekordbox_id, (unsigned)s_job.wave_len,
             (unsigned)s_job.color_len,
             (unsigned)beats, (unsigned)cues, plan.cue_lists ? "" : " (no list)",
             (unsigned)s_job.art_len, (unsigned)s_job.artwork_id,
             plan.write_dat ? (s_job.dat_cached ? "refreshed" : "cached")
                            : (s_job.dat_cached ? "cache kept" : "none"),
             ok ? "" : " (ANLZ WRITE FAILED)");
    dj_link_fetch_end(DJ_LINK_FETCH_DONE, NULL, s_job.cache_hit);
}

/* v300: the artwork, once the cues are in (v303; or alone, v302). */
static bool dj_link_fetch_want_art(uint32_t now_ms)
{
    if (s_job.artwork_id == 0u) {
        return false;
    }
    if (!s_job.art) {
        s_job.art = heap_caps_malloc(DJ_LINK_FETCH_ART_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_job.art &&
           dj_link_db_want_blob(&s_browse->client, DJLINK_DB_TYPE_ARTWORK_REQUEST,
                                s_job.artwork_id, s_job.art, DJ_LINK_FETCH_ART_MAX, now_ms);
}

/* v300: the audio is cached; ask the browse session for the waveform, beat
 * grid, cue list (v303) and artwork. v303: the analysis is asked even when
 * <key>.DAT is cached - v302 kept it forever, so a beat grid or cue edit at
 * the source never reached the deck - with a shorter wait, the cached files
 * standing if the answer is late or partial. The artwork only when <key>.JPG
 * is missing (v302). Without a session for this peer the track loads as
 * before, with what is cached. All of it runs in the dj_link task, before
 * the deck is told the track is ready: nothing here touches the audio path. */
static void dj_link_fetch_analysis(bool cache_hit, uint32_t now_ms)
{
    char dat[DJ_LINK_FETCH_PATH_MAX];
    char jpg[DJ_LINK_FETCH_PATH_MAX];
    s_job.cache_hit = cache_hit;
    dj_link_fetch_anlz_path(dat, sizeof(dat), "DAT");
    dj_link_fetch_anlz_path(jpg, sizeof(jpg), "JPG");
    s_job.dat_cached = dj_link_fetch_cached(dat);
    s_job.want_art = s_job.artwork_id != 0u && !dj_link_fetch_cached(jpg);
    if (!s_browse || s_browse_peer != s_job.peer || !s_browse_started) {
        dj_link_fetch_end(DJ_LINK_FETCH_DONE, NULL, cache_hit);
        return;
    }
    s_job.wave_len = 0u;
    s_job.grid_len = 0u;
    s_job.cues_len = 0u;
    s_job.art_len = 0u;
    s_job.color_len = 0u;
    s_job.wave_answered = false;
    s_job.grid_answered = false;
    s_job.cues_answered = false;
    s_job.wave = heap_caps_malloc(DJ_LINK_ANLZ_WAVE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_job.grid = heap_caps_malloc(DJ_LINK_ANLZ_GRID_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_job.cues = heap_caps_malloc(DJ_LINK_FETCH_CUES_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool mem = s_job.wave && s_job.grid && s_job.cues;
    if (!mem || !dj_link_db_want_blob(&s_browse->client, DJLINK_DB_TYPE_WAVEFORM_REQUEST,
                                      s_job.rekordbox_id, s_job.wave, DJ_LINK_ANLZ_WAVE_MAX,
                                      now_ms)) {
        ESP_LOGW(TAG, "fetch #%u id %u: no analysis (%s)", (unsigned)s_job.peer,
                 (unsigned)s_job.rekordbox_id, mem ? "source db busy" : "no memory");
        dj_link_fetch_end(DJ_LINK_FETCH_DONE, NULL, cache_hit);
        return;
    }
    s_job.analysis_wait = true;
    s_job.analysis_deadline_ms =
        now_ms + (s_job.dat_cached ? DJ_LINK_FETCH_REFRESH_MS : DJ_LINK_FETCH_ANALYSIS_MS);
}

/* v300: the browse session answered an analysis or artwork request (len 0
 * = none); id is the artwork id for the artwork. Wave, grid, cues (v303),
 * then artwork. dj_link task, from dj_link_browse_service. */
static void dj_link_fetch_on_blob(uint32_t id, uint16_t request, size_t len, bool answered)
{
    bool art = request == DJLINK_DB_TYPE_ARTWORK_REQUEST;
    if (!s_job.analysis_wait || id != (art ? s_job.artwork_id : s_job.rekordbox_id)) {
        return;
    }
    const uint32_t now_ms = dj_link_now_ms();
    if (request == DJLINK_DB_TYPE_WAVEFORM_REQUEST) {
        s_job.wave_len = len;
        s_job.wave_answered = answered;
        if (dj_link_db_want_blob(&s_browse->client, DJLINK_DB_TYPE_BEATGRID_REQUEST,
                                 s_job.rekordbox_id, s_job.grid, DJ_LINK_ANLZ_GRID_MAX, now_ms)) {
            return;
        }
    } else if (request == DJLINK_DB_TYPE_BEATGRID_REQUEST) {
        s_job.grid_len = len;
        s_job.grid_answered = answered;
        if (dj_link_db_want_blob(&s_browse->client, DJLINK_DB_TYPE_CUES_EXT_REQUEST,
                                 s_job.rekordbox_id, s_job.cues, DJ_LINK_FETCH_CUES_MAX, now_ms)) {
            return;
        }
    } else if (request == DJLINK_DB_TYPE_CUES_EXT_REQUEST) {
        s_job.cues_len = len;
        s_job.cues_answered = answered;
        /* v314: the colour preview, only with a detail to go with it. */
        if (s_job.wave_len > 0u && !s_job.color) {
            s_job.color = heap_caps_malloc(DJ_LINK_ANLZ_COLOR_BLOB_MAX,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (s_job.wave_len > 0u && s_job.color &&
            dj_link_db_want_blob(&s_browse->client, DJ_LINK_DB_TYPE_ANLZ_TAG_REQUEST,
                                 s_job.rekordbox_id, s_job.color, DJ_LINK_ANLZ_COLOR_BLOB_MAX,
                                 now_ms)) {
            return;
        }
        if (s_job.want_art && dj_link_fetch_want_art(now_ms)) {
            return;
        }
    } else if (request == DJ_LINK_DB_TYPE_ANLZ_TAG_REQUEST) {
        s_job.color_len = answered ? len : 0u;
        if (s_job.want_art && dj_link_fetch_want_art(now_ms)) {
            return;
        }
    } else {
        s_job.art_len = len;
    }
    dj_link_fetch_analysis_done();
}

/* Stop a running job, e.g. when the PCBs close. */
static void dj_link_fetch_abort(const char *why)
{
    if (s_job.state == DJ_LINK_FETCH_PDB || s_job.state == DJ_LINK_FETCH_AUDIO) {
        if (s_fetch) {
            djlink_nfs_cancel(&s_fetch->nfs);
        }
        dj_link_fetch_end(DJ_LINK_FETCH_FAILED, why, false);
    }
}

static void dj_link_fetch_service(uint32_t now_ms)
{
    uint32_t keep[2];
    portENTER_CRITICAL(&s_mux);
    bool start = s_fetch_pending;
    s_fetch_pending = false;
    uint32_t id = s_fetch_status.id;
    uint8_t peer = s_fetch_status.peer;
    uint32_t rekordbox_id = s_fetch_status.rekordbox_id;
    uint32_t artwork_id = s_fetch_req_artwork;
    keep[0] = s_fetch_req_keep[0];
    keep[1] = s_fetch_req_keep[1];
    uint32_t generation = s_browse_status.generation;
    bool cancel = s_fetch_cancel_id != 0u && s_fetch_cancel_id == s_job.id;
    s_fetch_cancel_id = 0u;
    portEXIT_CRITICAL(&s_mux);

    if (start) {
        dj_link_fetch_begin(id, peer, rekordbox_id, artwork_id, keep, generation, now_ms);
    }
    if (s_job.state != DJ_LINK_FETCH_PDB && s_job.state != DJ_LINK_FETCH_AUDIO) {
        return;
    }
    djlink_nfs_t *c = &s_fetch->nfs;
    if (cancel) {
        djlink_nfs_cancel(c);
        dj_link_fetch_end(DJ_LINK_FETCH_CANCELLED, "CANCELLED", false);
        return;
    }
    if (s_job.path_wait) {
        /* v297: the browse session (served before us in the task loop)
         * answers through dj_link_fetch_on_path; give up if it cannot. */
        if (generation != s_job.generation) {
            dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "SOURCE CHANGED", false);
        } else if (!s_browse || dj_link_db_phase(&s_browse->client) == DJ_LINK_DB_FAILED) {
            dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "SOURCE DB FAILED", false);
        } else if ((int32_t)(now_ms - s_job.path_deadline_ms) >= 0) {
            dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "SOURCE PATH TIMEOUT", false);
        }
        return;
    }
    if (s_job.analysis_wait) {
        /* v300: the audio is already cached; keep what arrived and finish. */
        if (generation != s_job.generation || !s_browse || s_browse_peer != s_job.peer ||
            dj_link_db_phase(&s_browse->client) == DJ_LINK_DB_FAILED ||
            (int32_t)(now_ms - s_job.analysis_deadline_ms) >= 0) {
            ESP_LOGW(TAG, "fetch #%u id %u: analysis gave up", (unsigned)s_job.peer,
                     (unsigned)s_job.rekordbox_id);
            dj_link_fetch_analysis_done();
        }
        return;
    }
    unsigned tail = atomic_load_explicit(&s_fetch_rx_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&s_fetch_rx_head, memory_order_acquire);
    while (tail != head && djlink_nfs_state(c) == DJLINK_NFS_BUSY) {
        const dj_link_fetch_rx_t *slot = &s_fetch->rx[tail % DJ_LINK_FETCH_RX_SLOTS];
        djlink_nfs_on_datagram(c, slot->data, slot->len, now_ms);
        tail++;
        atomic_store_explicit(&s_fetch_rx_tail, tail, memory_order_release);
    }
    djlink_nfs_poll(c, now_ms);
    switch (djlink_nfs_state(c)) {
    case DJLINK_NFS_DONE:
        if (c->retransmits) {
            ESP_LOGW(TAG, "fetch #%u: %u bytes, %u retransmits", (unsigned)s_job.peer,
                     (unsigned)c->size, (unsigned)c->retransmits);
        }
        if (dj_link_fetch_diag_discard()) {
            /* v316 mode A: nothing was written; never commit an empty file. */
            ESP_LOGW(TAG, "fetch #%u: DIAG mode A - %u bytes received, not cached",
                     (unsigned)s_job.peer, (unsigned)c->size);
            dj_link_fetch_end(DJ_LINK_FETCH_FAILED, "DIAG: NO SD WRITE", false);
            break;
        }
        dj_link_fetch_write_stat_log("audio");
        dj_link_fetch_commit(now_ms);
        break;
    case DJLINK_NFS_FAILED:
    case DJLINK_NFS_CANCELLED:
        dj_link_fetch_failed(now_ms);
        break;
    default:
        break;
    }
}

static esp_err_t dj_link_open(void)
{
    esp_netif_t *eth = s_config.eth_netif ? s_config.eth_netif() : NULL;
    int index = esp_netif_get_netif_impl_index(eth);
    uint8_t mac[6];
    uint32_t ip = dj_link_eth_ip(eth);
    if (index <= 0 || index > UINT8_MAX || ip == 0u ||
        esp_netif_get_mac(eth, mac) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    /* No PCB exists, so the ring has no producer: safe to rewind. */
    atomic_store(&s_ring_head, 0u);
    atomic_store(&s_ring_tail, 0u);
    s_rx_task = xTaskGetCurrentTaskHandle();
    s_netif_index = (u8_t)index;
    esp_err_t rc = esp_netif_tcpip_exec(dj_link_open_pcbs_tcpip, (void *)(uintptr_t)index);
    if (rc == ESP_OK) {
        /* Deck 2 joins later, from dj_link_task(), once deck 1 holds a
         * number: one claim sequence at a time, no number clash. */
        dj_link_session_set_pair(&s_session[0], DJ_LINK_PAIR_LOW);
        dj_link_session_start(&s_session[0], DJ_LINK_DEVICE_NAME, mac, ip, dj_link_now_ms());
        s_joined_number[0] = 0u;
        s_joined_number[1] = 0u;
        s_status_counter[0] = 0u;
        s_status_counter[1] = 0u;
        memset(s_beat_tracker, 0, sizeof(s_beat_tracker));
        s_players_wait_ms = DJ_LINK_RX_WAIT_MS;
    }
    return rc;
}

static void dj_link_remote_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    s_remote_state = DJ_LINK_REMOTE_IDLE;
    portEXIT_CRITICAL(&s_mux);
}

static void dj_link_close(void)
{
    /* Before the TCP/UDP teardown: the fetch PCB needs the netif index. */
    dj_link_fetch_abort("DJ LINK STOPPED");
    /* A browse in progress starts over (new generation) once reopened. */
    if (s_browse_peer != 0u) {
        dj_link_browse_restart(s_browse_peer);
    } else {
        dj_link_tcp_close();
    }
    /* Runs in the tcpip thread, so no callback is in flight afterwards.
     * Players simply stop hearing our keep-alives and drop us. */
    esp_netif_tcpip_exec(dj_link_close_pcbs_tcpip, NULL);
    dj_link_session_reset(&s_session[0]);
    dj_link_session_reset(&s_session[1]);
    dj_link_remote_reset();
}

static void dj_link_on_load_track(const dj_link_rx_slot_t *slot)
{
    dj_link_load_cmd_t cmd;
    /* Both sessions share the IP, so the 0x19 names its deck at 0x40; one
     * naming neither leaves the choice to the UI. Of ours only deck 1's
     * library can be loaded; another player's track is downloaded. */
    uint8_t library = dj_link_session_number(&s_session[0]);
    dj_link_load_verdict_t verdict = dj_link_session_check_load(
        &s_session[0], library, slot->data, slot->len, slot->unicast, &cmd);
    const uint8_t numbers[2] = { library, dj_link_session_number(&s_session[1]) };
    int deck = verdict == DJ_LINK_LOAD_OK ? dj_link_load_target_deck(numbers, &cmd) : -1;
    bool queued = false;
    if (verdict == DJ_LINK_LOAD_OK) {
        portENTER_CRITICAL(&s_mux);
        if (s_remote_state == DJ_LINK_REMOTE_IDLE) {
            s_remote_req.id = ++s_remote_next_id;
            s_remote_req.rekordbox_id = cmd.rekordbox_id;
            s_remote_req.from_number = cmd.sender_number;
            s_remote_req.deck = (int8_t)deck;
            s_remote_req.source_number = cmd.source_device == library ? 0u : cmd.source_device;
            s_remote_req.source_slot = cmd.source_slot;
            s_remote_state = DJ_LINK_REMOTE_PENDING;
            queued = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (queued) {
            s_remote_ip = slot->src_ip;
            s_remote_deck = deck < 0 ? 0 : deck;
            s_remote_since_ms = slot->rx_ms;
        }
    }
    ESP_LOGW(TAG, "load-track from #%u: rekordbox id %u on #%u slot %u for deck %d -> %s",
             (unsigned)cmd.sender_number, (unsigned)cmd.rekordbox_id,
             (unsigned)cmd.source_device, (unsigned)cmd.source_slot, deck + 1,
             verdict != DJ_LINK_LOAD_OK ? dj_link_load_verdict_str(verdict)
                                        : (queued ? "queued for the UI" : "refused: busy"));
}

/* Ack an accepted load, expire one the UI never took. */
static void dj_link_service_remote(uint32_t now_ms)
{
    portENTER_CRITICAL(&s_mux);
    dj_link_remote_state_t state = s_remote_state;
    dj_link_load_request_t req = s_remote_req;
    bool expired = (state == DJ_LINK_REMOTE_PENDING || state == DJ_LINK_REMOTE_TAKEN) &&
                   (uint32_t)(now_ms - s_remote_since_ms) > DJ_LINK_LOAD_TIMEOUT_MS;
    if (state == DJ_LINK_REMOTE_ACCEPTED || state == DJ_LINK_REMOTE_REFUSED || expired) {
        s_remote_state = DJ_LINK_REMOTE_IDLE;
    }
    portEXIT_CRITICAL(&s_mux);
    if (state == DJ_LINK_REMOTE_ACCEPTED) {
        dj_link_send(DJ_LINK_PCB_STATUS, s_remote_ip, DJLINK_PORT_STATUS,
                     dj_link_session_load_ack(&s_session[s_remote_deck], s_tx, sizeof(s_tx)));
        ESP_LOGW(TAG, "load-track #%u id %u accepted - ack sent",
                 (unsigned)req.from_number, (unsigned)req.rekordbox_id);
    } else if (expired) {
        ESP_LOGW(TAG, "load-track #%u id %u not taken by the UI - dropped",
                 (unsigned)req.from_number, (unsigned)req.rekordbox_id);
    }
}

/* v305: our decks as the master negotiation sees them. */
static void dj_link_master_decks(dj_link_master_deck_t decks[2])
{
    portENTER_CRITICAL(&s_mux);
    for (int d = 0; d < 2; d++) {
        decks[d].loaded = s_deck_report[d].loaded;
        decks[d].playing = s_deck_report[d].playing;
        decks[d].want = s_deck_report[d].loaded && s_deck_report[d].master;
    }
    portEXIT_CRITICAL(&s_mux);
    for (int d = 0; d < 2; d++) {
        decks[d].number = dj_link_session_number(&s_session[d]);
    }
}

static void dj_link_deck_command(uint8_t deck, dj_link_deck_cmd_t cmd)
{
    if (cmd == DJ_LINK_DECK_CMD_NONE) {
        return;
    }
    ESP_LOGW(TAG, "deck %u: %s", (unsigned)deck + 1u, dj_link_deck_cmd_str(cmd));
    if (s_config.deck_command) {
        s_config.deck_command(deck, cmd);
    }
}

/* v305: sync control 0x2a, takeover request 0x26 and response 0x27 (port
 * 50001, unicast to our shared IP). */
static void dj_link_on_sync_packet(const dj_link_rx_slot_t *slot)
{
    const uint8_t type = slot->data[0x0a];
    const uint8_t from = slot->data[0x21];
    const bool on = atomic_load(&s_sync_control);
    dj_link_master_deck_t decks[2];
    dj_link_master_decks(decks);
    if (type == 0x2au) {
        djlink_sync_t sc;
        if (djlink_sync_parse(slot->data, slot->len, &sc) != DJLINK_OK) {
            return;
        }
        dj_link_deck_cmd_t cmd = dj_link_sync_action_cmd(sc.action);
        int deck = dj_link_sync_target_deck(decks);
        const char *verdict = !on ? "ignored: LINK SYNC off"
                            : cmd == DJ_LINK_DECK_CMD_NONE ? "ignored: unknown action"
                            : deck < 0 ? "refused: ambiguous deck"
                            : "applied";
        ESP_LOGW(TAG, "sync control 0x%02x from #%u -> deck %d %s", (unsigned)sc.action,
                 (unsigned)from, deck + 1, verdict);
        if (on && cmd != DJ_LINK_DECK_CMD_NONE && deck >= 0 &&
            !(cmd == DJ_LINK_DECK_CMD_MASTER_TAKE &&
              dj_link_master_asserts(&s_master, (uint8_t)deck))) {
            dj_link_deck_command((uint8_t)deck, cmd);
        }
        return;
    }
    if (type == 0x26u) {
        djlink_handoff_req_t req;
        if (djlink_handoff_req_parse(slot->data, slot->len, &req) != DJLINK_OK) {
            return;
        }
        uint8_t ours = on ? dj_link_master_on_request(&s_master, decks, req.requester_number,
                                                      slot->rx_ms)
                          : 0u;
        ESP_LOGW(TAG, "tempo master request from #%u -> %s", (unsigned)req.requester_number,
                 ours ? "yielding" : (on ? "not master" : "ignored: LINK SYNC off"));
        if (ours) {
            /* 0x21 and 0x27 carry the responder's own number (sync.html). */
            djlink_handoff_resp_t resp = { .name = DJ_LINK_DEVICE_NAME,
                                           .requester_number = ours };
            dj_link_send(DJ_LINK_PCB_BEAT, slot->src_ip, DJLINK_PORT_BEAT,
                         djlink_handoff_resp_build(&resp, s_tx, sizeof(s_tx)));
        }
        return;
    }
    if (type == 0x27u) {
        ESP_LOGW(TAG, "tempo master response from #%u%s", (unsigned)from,
                 dj_link_master_on_response(&s_master, from) ? " - waiting for its handoff"
                                                             : " (not asked)");
    }
}

static bool dj_link_handle(const dj_link_rx_slot_t *slot)
{
    const uint8_t type = slot->data[0x0a];
    if (slot->src_ip == s_session[0].ip) {
        /* v298: our two decks' claims, keep-alives and status. */
        return false;
    }
    if (slot->port == DJLINK_PORT_DISCOVERY) {
        for (int d = 0; d < 2; d++) {
            dj_link_session_t *s = &s_session[d];
            if (s->phase == DJ_LINK_CLAIM_IDLE) {
                /* Deck 2 before its claim: still learn the numbers in use. */
                if (type == 0x06u && slot->len >= DJLINK_KEEPALIVE_PACKET_LEN) {
                    dj_link_session_note_device(s, slot->data[0x24], slot->rx_ms);
                }
            } else if (dj_link_session_on_discovery(s, slot->data, slot->len, slot->src_ip,
                                                    slot->rx_ms) &&
                       !dj_link_session_active(s)) {
                ESP_LOGW(TAG, "deck %d player number taken - re-claiming as #%u", d + 1,
                         (unsigned)s->device_number);
            }
        }
        /* v297: keep-alives make peers too. A rekordbox source (vynull,
         * rekordbox) sends no beat or status at all. */
        return dj_link_table_ingest_from(&s_table, slot->port, slot->data, slot->len,
                                         slot->src_ip, slot->rx_ms) == DJ_LINK_RX_ACCEPTED;
    }
    if (slot->port == DJLINK_PORT_STATUS && type == 0x05u) {
        /* Only deck 1 has media; queries for deck 2 go unanswered. */
        int n = dj_link_session_media_reply(&s_session[0], slot->data, slot->len, slot->src_ip,
                                            atomic_load(&s_local_track_count), slot->rx_ms,
                                            s_tx, sizeof(s_tx));
        if (n > 0) {
            dj_link_send(DJ_LINK_PCB_STATUS, slot->src_ip, DJLINK_PORT_STATUS, n);
            ESP_LOGD(TAG, "media query from #%u answered", (unsigned)slot->data[0x21]);
        }
        return false;
    }
    if (slot->port == DJLINK_PORT_STATUS && type == 0x19u) {
        dj_link_on_load_track(slot);
        return false;
    }
    if (slot->port == DJLINK_PORT_BEAT && (type == 0x2au || type == 0x26u || type == 0x27u)) {
        dj_link_on_sync_packet(slot);
        return false;
    }
    if (slot->port == DJLINK_PORT_STATUS && type == 0x1au) {
        ESP_LOGW(TAG, "unexpected load ack from #%u (we send no loads)",
                 (unsigned)slot->data[0x21]);
        return false;
    }
    if (dj_link_table_ingest_from(&s_table, slot->port, slot->data, slot->len,
                                  slot->src_ip, slot->rx_ms) != DJ_LINK_RX_ACCEPTED) {
        return false;
    }
    /* Beat, position and status all carry the player number at 0x21. */
    dj_link_session_note_device(&s_session[0], slot->data[0x21], slot->rx_ms);
    dj_link_session_note_device(&s_session[1], slot->data[0x21], slot->rx_ms);
    return true;
}

static bool dj_link_drain(void)
{
    bool changed = false;
    unsigned tail = atomic_load_explicit(&s_ring_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&s_ring_head, memory_order_acquire);
    while (tail != head) {
        if (dj_link_handle(&s_ring[tail % DJ_LINK_RING_SLOTS])) {
            changed = true;
        }
        tail++;
        atomic_store_explicit(&s_ring_tail, tail, memory_order_release);
    }
    return changed;
}

/* v304: last clock handed to s_config.beat_clock. dj_link task only. */
static dj_link_beat_clock_t s_beat_clock;

static void dj_link_beat_clock_service(uint32_t now_ms)
{
    if (!s_config.beat_clock) {
        return;
    }
    dj_link_beat_clock_t c;
    dj_link_table_beat_clock(&s_table, s_beat_clock.valid ? s_beat_clock.player : 0u,
                             now_ms, &c);
    if (dj_link_master_asserts(&s_master, 0u) || dj_link_master_asserts(&s_master, 1u)) {
        /* v305: we are the master; the peers follow us, so SYNC on our other
         * deck follows ours (local beat sync), not a peer echoing it. */
        memset(&c, 0, sizeof(c));
    }
    if (c.valid == s_beat_clock.valid && c.player == s_beat_clock.player &&
        c.beat_in_bar == s_beat_clock.beat_in_bar && c.anchor_ms == s_beat_clock.anchor_ms &&
        c.period_us == s_beat_clock.period_us) {
        return;
    }
    if (c.valid != s_beat_clock.valid || c.player != s_beat_clock.player) {
        if (c.valid) {
            ESP_LOGI(TAG, "beat clock: player #%u, %.2f BPM", (unsigned)c.player,
                     (double)(60000000.0f / (float)c.period_us));
        } else {
            ESP_LOGI(TAG, "beat clock: none");
        }
    }
    s_beat_clock = c;
    s_config.beat_clock(&c);
}

static void dj_link_publish(dj_link_state_t state)
{
    dj_link_summary_t summary;
    dj_link_table_summarize(&s_table, state, &summary);
    summary.rx_dropped += atomic_load_explicit(&s_ring_filtered, memory_order_relaxed);
    summary.our_number = dj_link_session_number(&s_session[0]);
    summary.our_number_d2 = dj_link_session_number(&s_session[1]);
    portENTER_CRITICAL(&s_mux);
    s_published = summary;
    portEXIT_CRITICAL(&s_mux);
}

/* v305: with LINK SYNC on, the master flag and Mh our status shows come from
 * the negotiation instead of SYNC MASTER alone; a 0x26 goes unicast to the
 * master's port 50001. */
static void dj_link_master_service(dj_link_deck_report_t report[2], uint32_t now_ms)
{
    if (!atomic_load(&s_sync_control)) {
        if (s_master.phase != DJ_LINK_MASTER_IDLE) {
            ESP_LOGW(TAG, "tempo master negotiation off");
        }
        dj_link_master_reset(&s_master);
        s_master_logged = DJ_LINK_MASTER_IDLE;
        return;
    }
    dj_link_master_deck_t decks[2];
    for (int d = 0; d < 2; d++) {
        decks[d].number = dj_link_session_number(&s_session[d]);
        decks[d].loaded = report[d].loaded;
        decks[d].playing = report[d].playing;
        decks[d].want = report[d].loaded && report[d].master;
    }
    dj_link_master_out_t out;
    dj_link_master_update(&s_master, decks, &s_table, now_ms, &out);
    if (out.send_request) {
        djlink_handoff_req_t req = { .name = DJ_LINK_DEVICE_NAME,
                                     .requester_number = out.request_number };
        dj_link_send(DJ_LINK_PCB_BEAT, out.request_ip, DJLINK_PORT_BEAT,
                     djlink_handoff_req_build(&req, s_tx, sizeof(s_tx)));
    }
    for (uint8_t d = 0; d < 2u; d++) {
        dj_link_deck_command(d, out.cmd[d]);
        report[d].master = dj_link_master_asserts(&s_master, d);
        report[d].master_handoff = dj_link_master_handoff(&s_master, d);
    }
    if (s_master.phase != s_master_logged) {
        s_master_logged = s_master.phase;
        ESP_LOGW(TAG, "tempo master: deck %u %s (peer #%u)", (unsigned)s_master.deck + 1u,
                 dj_link_master_phase_str(s_master.phase), (unsigned)s_master.peer);
    }
}

/* v298: claim steps and keep-alives for both decks, then every
 * DJ_LINK_STATUS_MS a CDJ status per joined deck, all broadcast on the
 * Ethernet subnet. Each session treats the other's number as taken.
 * v301: per deck with a track, a beat 0x28 when its playhead crosses a grid
 * beat and every DJ_LINK_POSITION_MS an absolute position 0x0b, on port
 * 50001; s_players_wait_ms is when the next one is due. */
static bool dj_link_players_service(uint32_t now_ms)
{
    bool changed = false;
    if (dj_link_session_active(&s_session[0]) && s_session[1].phase == DJ_LINK_CLAIM_IDLE) {
        dj_link_session_set_pair(&s_session[1], DJ_LINK_PAIR_HIGH);
        dj_link_session_start(&s_session[1], DJ_LINK_DEVICE_NAME, s_session[0].mac,
                              s_session[0].ip, now_ms);
    }
    for (int d = 0; d < 2; d++) {
        dj_link_session_t *s = &s_session[d];
        dj_link_session_set_sibling(s, s_session[d ^ 1].device_number);
        int n;
        while ((n = dj_link_session_poll(s, now_ms, s_tx, sizeof(s_tx))) > 0) {
            dj_link_send(DJ_LINK_PCB_DISCOVERY, 0u, DJLINK_PORT_DISCOVERY, n);
        }
        uint8_t number = dj_link_session_number(s);
        if (number != s_joined_number[d]) {
            s_joined_number[d] = number;
            changed = true;
            if (number) {
                ESP_LOGW(TAG, "deck %d joined as player #%u", d + 1, (unsigned)number);
            }
        }
    }

    dj_link_deck_report_t report[2];
    portENTER_CRITICAL(&s_mux);
    memcpy(report, s_deck_report, sizeof(report));
    portEXIT_CRITICAL(&s_mux);
    dj_link_master_service(report, now_ms);

    if ((uint32_t)(now_ms - s_status_ms) >= DJ_LINK_STATUS_MS) {
        s_status_ms = now_ms;
        uint8_t library = dj_link_session_number(&s_session[0]);
        for (int d = 0; d < 2; d++) {
            int n = dj_link_session_status(&s_session[d], library, &report[d],
                                           s_status_counter[d] + 1u, s_tx, sizeof(s_tx));
            if (n > 0) {
                s_status_counter[d]++;
                dj_link_send(DJ_LINK_PCB_STATUS, 0u, DJLINK_PORT_STATUS, n);
            }
        }
    }

    for (int d = 0; d < 2; d++) {
        int n = dj_link_session_beat(&s_session[d], &report[d], &s_beat_tracker[d], now_ms,
                                     s_tx, sizeof(s_tx));
        if (n > 0) {
            dj_link_send(DJ_LINK_PCB_BEAT, 0u, DJLINK_PORT_BEAT, n);
        }
    }
    if ((uint32_t)(now_ms - s_position_ms) >= DJ_LINK_POSITION_MS) {
        s_position_ms = now_ms;
        for (int d = 0; d < 2; d++) {
            int n = dj_link_session_position(&s_session[d], &report[d], now_ms, s_tx,
                                             sizeof(s_tx));
            if (n > 0) {
                dj_link_send(DJ_LINK_PCB_BEAT, 0u, DJLINK_PORT_BEAT, n);
            }
        }
    }

    /* While a joined deck has a track, wake for its next position or beat;
     * the RX notification still wakes the task earlier. */
    uint32_t wait = DJ_LINK_RX_WAIT_MS;
    for (int d = 0; d < 2; d++) {
        if (!report[d].loaded || dj_link_session_number(&s_session[d]) == 0u) {
            continue;
        }
        uint32_t since = now_ms - s_position_ms;
        uint32_t position = since < DJ_LINK_POSITION_MS ? DJ_LINK_POSITION_MS - since : 1u;
        uint32_t beat = dj_link_deck_next_beat_in_ms(&report[d], now_ms);
        wait = position < wait ? position : wait;
        wait = beat < wait ? beat : wait;
    }
    s_players_wait_ms = wait > 0u ? wait : 1u;
    return changed;
}

static bool dj_link_wanted(void)
{
    portENTER_CRITICAL(&s_mux);
    bool want = s_want;
    portEXIT_CRITICAL(&s_mux);
    return want;
}

static void dj_link_task(void *arg)
{
    (void)arg;
    bool open = false;
    dj_link_state_t state = DJ_LINK_STATE_OFF;
    uint32_t last_publish_ms = 0;
    uint32_t last_ip_check_ms = 0;
    dj_link_table_reset(&s_table);
    dj_link_master_reset(&s_master);
    s_master_logged = DJ_LINK_MASTER_IDLE;
    atomic_store(&s_ring_filtered, 0u);
    portENTER_CRITICAL(&s_mux);
    s_browse_req_done = s_browse_req; /* selections made while OFF are void */
    portEXIT_CRITICAL(&s_mux);

    for (;;) {
        if (!dj_link_wanted()) {
            if (open) {
                dj_link_close();
                open = false;
            }
            dj_link_table_reset(&s_table);
            dj_link_master_reset(&s_master);
            dj_link_beat_clock_service(dj_link_now_ms());
            state = DJ_LINK_STATE_OFF;
            s_browse_peer = 0u;
            dj_link_browse_free();
            /* Freed before s_running drops, so a successor task can never
             * see (or lose) this ring. */
            heap_caps_free(s_ring);
            s_ring = NULL;
            portENTER_CRITICAL(&s_mux);
            if (!s_want) {
                /* Re-checked under the lock: a re-enable racing this exit
                 * keeps the task instead of spawning a second one. */
                memset(&s_published, 0, sizeof(s_published));
                s_published.state = DJ_LINK_STATE_OFF;
                s_browse_want = 0u;
                memset(&s_browse_status, 0, sizeof(s_browse_status));
                if (s_fetch_pending) {
                    /* dj_link_close() already ended a running fetch. */
                    s_fetch_pending = false;
                    s_fetch_status.state = DJ_LINK_FETCH_FAILED;
                    snprintf(s_fetch_status.error, sizeof(s_fetch_status.error),
                             "DJ LINK STOPPED");
                }
                s_running = false;
                portEXIT_CRITICAL(&s_mux);
                ESP_LOGW(TAG, "stopped");
                vTaskDeleteWithCaps(NULL);
                return;
            }
            portEXIT_CRITICAL(&s_mux);
            continue;
        }

        bool ip = s_config.ip_ready && s_config.ip_ready();
        if (open && !ip) {
            dj_link_close();
            open = false;
            dj_link_table_reset(&s_table);
            ESP_LOGW(TAG, "Ethernet IP lost - PCBs closed");
        }
        if (!open) {
            dj_link_state_t next = DJ_LINK_STATE_WAIT_IP;
            if (ip) {
                if (!s_ring) {
                    s_ring = heap_caps_calloc(DJ_LINK_RING_SLOTS, sizeof(dj_link_rx_slot_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                esp_err_t rc = s_ring ? dj_link_open() : ESP_ERR_NO_MEM;
                if (rc == ESP_OK) {
                    open = true;
                    next = DJ_LINK_STATE_LISTENING;
                    ESP_LOGW(TAG, "UDP %u/%u/%u open (Ethernet only) - joining as a player",
                             (unsigned)DJLINK_PORT_DISCOVERY, (unsigned)DJLINK_PORT_BEAT,
                             (unsigned)DJLINK_PORT_STATUS);
                } else {
                    next = DJ_LINK_STATE_ERROR;
                    if (state != DJ_LINK_STATE_ERROR) {
                        ESP_LOGW(TAG, "UDP open failed: %s", esp_err_to_name(rc));
                    }
                }
            }
            if (next != state) {
                state = next;
                dj_link_publish(state);
                last_publish_ms = dj_link_now_ms();
            }
            if (!open) {
                vTaskDelay(pdMS_TO_TICKS(DJ_LINK_IDLE_POLL_MS));
                continue;
            }
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_players_wait_ms));
        bool changed = dj_link_drain();
        uint32_t now_ms = dj_link_now_ms();
        changed |= dj_link_table_expire(&s_table, now_ms);
        dj_link_beat_clock_service(now_ms);
        dj_link_service_remote(now_ms);
        dj_link_browse_service(now_ms);
        dj_link_fetch_service(now_ms);

        changed |= dj_link_players_service(now_ms);

        /* A new DHCP lease invalidates the IP in our claims: rejoin. */
        if ((uint32_t)(now_ms - last_ip_check_ms) >= DJ_LINK_REFRESH_MS) {
            last_ip_check_ms = now_ms;
            if (dj_link_eth_ip(s_config.eth_netif()) != s_session[0].ip) {
                dj_link_close();
                open = false;
                dj_link_table_reset(&s_table);
                ESP_LOGW(TAG, "Ethernet IP changed - rejoining");
                continue;
            }
        }
        uint32_t since = now_ms - last_publish_ms;
        if ((changed && since >= DJ_LINK_PUBLISH_MS) || since >= DJ_LINK_REFRESH_MS) {
            dj_link_publish(state);
            last_publish_ms = now_ms;
        }
    }
}

void dj_link_browse_select(uint8_t peer)
{
    portENTER_CRITICAL(&s_mux);
    if (peer != s_browse_want) {
        s_browse_sort_want = DJ_LINK_DB_SORT_DEFAULT;   /* v310 */
        s_browse_desc_want = false;
        s_browse_menu_want = DJ_LINK_DB_MENU_ALL_TRACKS; /* v311 */
        s_browse_menu_id_want = 0u;
    }
    s_browse_want = peer;
    s_browse_req++;
    portEXIT_CRITICAL(&s_mux);
}

void dj_link_browse_open(dj_link_db_menu_t menu, uint32_t id)
{
    portENTER_CRITICAL(&s_mux);
    s_browse_menu_want = (uint8_t)menu;
    s_browse_menu_id_want = menu == DJ_LINK_DB_MENU_ALL_TRACKS ? 0u : id;
    s_browse_req++;     /* the selected player, listed again */
    portEXIT_CRITICAL(&s_mux);
}

void dj_link_browse_set_sort(uint8_t sort, bool descending)
{
    portENTER_CRITICAL(&s_mux);
    s_browse_sort_want = sort;
    s_browse_desc_want = descending;
    s_browse_req++;     /* the selected player, listed again */
    portEXIT_CRITICAL(&s_mux);
}

void dj_link_browse_get_status(dj_link_browse_status_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_browse_status;
    portEXIT_CRITICAL(&s_mux);
}

bool dj_link_browse_get_track(uint32_t generation, uint32_t index,
                              dj_link_peer_track_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (out && s_browse && generation == s_browse_status.generation &&
        index < s_browse_status.count) {
        *out = s_browse->tracks[index];
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void dj_link_browse_want_details(uint32_t generation, uint32_t first, uint32_t count)
{
    portENTER_CRITICAL(&s_mux);
    s_detail_gen = generation;
    s_detail_first = first;
    s_detail_count = count < DJ_LINK_BROWSE_DETAIL_MAX ? count : DJ_LINK_BROWSE_DETAIL_MAX;
    portEXIT_CRITICAL(&s_mux);
}

void dj_link_browse_want_detail_row(uint32_t generation, uint32_t index, uint32_t rekordbox_id)
{
    portENTER_CRITICAL(&s_mux);
    s_detail_prio_gen = generation;
    s_detail_prio_index = index;
    s_detail_prio_id = rekordbox_id;
    portEXIT_CRITICAL(&s_mux);
}

uint32_t dj_link_fetch_start(uint8_t peer, uint32_t rekordbox_id, uint32_t artwork_id,
                             const uint32_t *keep_keys)
{
    if (peer == 0u || rekordbox_id == 0u) {
        return 0u;
    }
    uint32_t id = 0u;
    portENTER_CRITICAL(&s_mux);
    bool busy = s_fetch_pending || s_fetch_status.state == DJ_LINK_FETCH_PDB ||
                s_fetch_status.state == DJ_LINK_FETCH_AUDIO;
    if (s_running && s_want && !busy) {
        id = ++s_fetch_next_id;
        if (id == 0u) {
            id = ++s_fetch_next_id;
        }
        memset(&s_fetch_status, 0, sizeof(s_fetch_status));
        s_fetch_status.id = id;
        s_fetch_status.state = DJ_LINK_FETCH_PDB;
        s_fetch_status.peer = peer;
        s_fetch_status.rekordbox_id = rekordbox_id;
        s_fetch_req_keep[0] = keep_keys ? keep_keys[0] : 0u;
        s_fetch_req_keep[1] = keep_keys ? keep_keys[1] : 0u;
        s_fetch_req_artwork = artwork_id;
        s_fetch_cancel_id = 0u;
        s_fetch_pending = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return id;
}

void dj_link_fetch_cancel(uint32_t id)
{
    portENTER_CRITICAL(&s_mux);
    if (id != 0u && s_fetch_status.id == id) {
        if (s_fetch_pending) {
            /* Not taken by the task yet: nothing to undo. */
            s_fetch_pending = false;
            s_fetch_status.state = DJ_LINK_FETCH_CANCELLED;
            snprintf(s_fetch_status.error, sizeof(s_fetch_status.error), "CANCELLED");
        } else if (s_fetch_status.state == DJ_LINK_FETCH_PDB ||
                   s_fetch_status.state == DJ_LINK_FETCH_AUDIO) {
            s_fetch_cancel_id = id;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

void dj_link_fetch_get_status(dj_link_fetch_status_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_fetch_status;
    portEXIT_CRITICAL(&s_mux);
}

void dj_link_set_sync_control(bool on)
{
    atomic_store(&s_sync_control, on);
}

void dj_link_set_local_track_count(uint32_t count)
{
    atomic_store(&s_local_track_count, count);
}

bool dj_link_take_load_request(dj_link_load_request_t *out)
{
    bool taken = false;
    portENTER_CRITICAL(&s_mux);
    if (s_remote_state == DJ_LINK_REMOTE_PENDING) {
        if (out) {
            *out = s_remote_req;
        }
        s_remote_state = DJ_LINK_REMOTE_TAKEN;
        taken = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return taken;
}

void dj_link_set_deck_report(uint8_t deck, const dj_link_deck_report_t *report)
{
    if (deck >= 2u || !report) {
        return;
    }
    uint32_t stamp_ms = dj_link_now_ms(); /* v301: the playhead's time */
    portENTER_CRITICAL(&s_mux);
    s_deck_report[deck] = *report;
    s_deck_report[deck].stamp_ms = stamp_ms;
    portEXIT_CRITICAL(&s_mux);
}

void dj_link_finish_load_request(uint32_t id, bool accepted)
{
    portENTER_CRITICAL(&s_mux);
    if (s_remote_state == DJ_LINK_REMOTE_TAKEN && s_remote_req.id == id) {
        s_remote_state = accepted ? DJ_LINK_REMOTE_ACCEPTED : DJ_LINK_REMOTE_REFUSED;
    }
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t dj_link_init(const dj_link_config_t *config)
{
    if (!config || !config->ip_ready || !config->eth_netif) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *config;
    s_published.state = DJ_LINK_STATE_OFF;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t dj_link_set_enabled(bool enable)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    bool create = false;
    portENTER_CRITICAL(&s_mux);
    s_want = enable;
    if (enable && !s_running) {
        s_running = true;
        create = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!create) {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCoreWithCaps(dj_link_task, "dj_link", DJ_LINK_TASK_STACK, NULL,
                                        DJ_LINK_TASK_PRIO, NULL, DJ_LINK_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_running = false;
        s_want = false;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "started");
    return ESP_OK;
}

void dj_link_get_summary(dj_link_summary_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_published;
    portEXIT_CRITICAL(&s_mux);
}
