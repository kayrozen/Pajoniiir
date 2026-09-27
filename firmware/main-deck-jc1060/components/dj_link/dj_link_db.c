#include "dj_link_db.h"

#include <stdio.h>
#include <string.h>

#include "djlink/dbserver.h"

#define DJ_LINK_DB_SORT_DEFAULT 0u
#define DJ_LINK_DB_NO_ITEMS     0xffffffffu

static const char *phase_name(dj_link_db_phase_t phase)
{
    switch (phase) {
    case DJ_LINK_DB_DISC_CONNECTING:    return "CONNECT";
    case DJ_LINK_DB_DISC_WAIT_PORT:     return "PORT";
    case DJ_LINK_DB_SRV_CONNECTING:     return "CONNECT DB";
    case DJ_LINK_DB_SRV_WAIT_GREETING:  return "GREETING";
    case DJ_LINK_DB_SRV_WAIT_SETUP:     return "SETUP";
    case DJ_LINK_DB_LIST_WAIT_AVAIL:    return "LIST";
    case DJ_LINK_DB_LIST_WAIT_RENDER:   return "RENDER";
    case DJ_LINK_DB_DETAIL_WAIT_AVAIL:  return "DETAIL";
    case DJ_LINK_DB_DETAIL_WAIT_RENDER: return "DETAIL RENDER";
    default:                            return "?";
    }
}

static bool has_connection(const dj_link_db_t *c)
{
    return c->phase != DJ_LINK_DB_IDLE && c->phase != DJ_LINK_DB_FAILED;
}

static void close_connection(dj_link_db_t *c)
{
    if (has_connection(c) && c->io.close) {
        c->io.close(c->io.ctx);
    }
    c->rx_len = 0;
}

static void fail(dj_link_db_t *c, const char *reason)
{
    close_connection(c);
    snprintf(c->error, sizeof(c->error), "%s", reason);
    c->phase = DJ_LINK_DB_FAILED;
}

static void wait_for(dj_link_db_t *c, dj_link_db_phase_t phase, uint32_t now_ms)
{
    c->phase = phase;
    c->deadline_ms = now_ms + c->step_timeout_ms;
}

static void connect_to(dj_link_db_t *c, uint16_t port, dj_link_db_phase_t phase,
                       uint32_t now_ms)
{
    if (!c->io.connect || c->io.connect(c->io.ctx, c->peer_ip, port) != 0) {
        c->phase = DJ_LINK_DB_IDLE; /* nothing to close */
        fail(c, "CONNECT FAILED");
        return;
    }
    wait_for(c, phase, now_ms);
}

/* Send what was just built into c->tx, then wait in `phase`. */
static void send_tx(dj_link_db_t *c, int n, dj_link_db_phase_t phase, uint32_t now_ms)
{
    if (n <= 0 || !c->io.send || c->io.send(c->io.ctx, c->tx, (size_t)n) != 0) {
        fail(c, "SEND FAILED");
        return;
    }
    wait_for(c, phase, now_ms);
}

static uint32_t dmst(const dj_link_db_t *c)
{
    return ((uint32_t)c->our_number << 24) | (0x01u << 16) |
           ((uint32_t)c->slot << 8) | 0x01u; /* main menu, rekordbox track */
}

static void request_render(dj_link_db_t *c, uint32_t offset, uint32_t count,
                           dj_link_db_phase_t phase, uint32_t now_ms)
{
    c->render_offset = offset;
    c->render_count = count;
    c->render_seen = 0;
    int n = djlink_db_render_request_build(++c->txid, c->our_number, c->slot,
                                           offset, count, c->tx, sizeof(c->tx));
    send_tx(c, n, phase, now_ms);
}

static void go_ready(dj_link_db_t *c, uint32_t now_ms)
{
    c->phase = DJ_LINK_DB_READY;
    c->idle_since_ms = now_ms;
}

/* READY: list first, then any metadata the owner asks for. */
static void start_next(dj_link_db_t *c, uint32_t now_ms)
{
    if (c->phase != DJ_LINK_DB_READY) {
        return;
    }
    if (!c->list_done) {
        djlink_db_arg_t args[2];
        memset(args, 0, sizeof(args));
        args[0].type = DJLINK_DB_FIELD_INT32;
        args[0].num = dmst(c);
        args[1].type = DJLINK_DB_FIELD_INT32;
        args[1].num = DJ_LINK_DB_SORT_DEFAULT;
        int n = djlink_db_msg_build(++c->txid, DJ_LINK_DB_TYPE_TRACK_MENU, args, 2,
                                    c->tx, sizeof(c->tx));
        send_tx(c, n, DJ_LINK_DB_LIST_WAIT_AVAIL, now_ms);
        return;
    }
    uint32_t index;
    uint32_t id;
    if (c->io.next_detail && c->io.next_detail(c->io.ctx, &index, &id)) {
        c->detail_index = index;
        memset(&c->detail, 0, sizeof(c->detail));
        c->detail.rekordbox_id = id;
        c->detail.audio = c->audio;
        int n = djlink_db_metadata_request_build(++c->txid, c->our_number, c->slot, id,
                                                 c->tx, sizeof(c->tx));
        send_tx(c, n, DJ_LINK_DB_DETAIL_WAIT_AVAIL, now_ms);
    }
}

void dj_link_db_utf16be_to_utf8(const uint8_t *in, size_t in_len, char *out, size_t cap)
{
    size_t o = 0;
    if (!out || cap == 0) {
        return;
    }
    for (size_t i = 0; in && i + 1 < in_len; i += 2) {
        uint32_t cp = ((uint32_t)in[i] << 8) | in[i + 1];
        if (cp == 0) {
            break;
        }
        if (cp >= 0xd800u && cp <= 0xdfffu) {
            if (cp < 0xdc00u && i + 3 < in_len) {
                i += 2; /* one '?' for the whole surrogate pair */
            }
            cp = '?';
        }
        size_t need = cp < 0x80u ? 1u : (cp < 0x800u ? 2u : 3u);
        if (o + need >= cap) {
            break;
        }
        if (need == 1) {
            out[o++] = (char)cp;
        } else if (need == 2) {
            out[o++] = (char)(0xc0u | (cp >> 6));
            out[o++] = (char)(0x80u | (cp & 0x3fu));
        } else {
            out[o++] = (char)(0xe0u | (cp >> 12));
            out[o++] = (char)(0x80u | ((cp >> 6) & 0x3fu));
            out[o++] = (char)(0x80u | (cp & 0x3fu));
        }
    }
    out[o] = '\0';
}

int dj_link_db_msg_size(const uint8_t *buf, size_t len)
{
    djlink_db_msg_t m;
    if (len == 0u) {
        return 0; /* the codec reports an empty buffer as bad magic */
    }
    djlink_err_t err = djlink_db_msg_parse(buf, len, &m);
    if (err == DJLINK_ERR_TRUNCATED) {
        return len >= DJ_LINK_DB_RX_MAX ? -1 : 0;
    }
    if (err != DJLINK_OK) {
        return -1;
    }
    /* magic 5 + txid 5 + type 3 + argc 2, then the tag blob field. */
    size_t pos = 20u + djlink_rd32(&buf[16]);
    for (uint8_t i = 0; i < m.arg_count; i++) {
        switch (m.args[i].type) {
        case DJLINK_DB_FIELD_INT8:  pos += 2u; break;
        case DJLINK_DB_FIELD_INT16: pos += 3u; break;
        case DJLINK_DB_FIELD_INT32: pos += 5u; break;
        default:                    pos += 5u + m.args[i].bin_len; break;
        }
    }
    return pos <= len && pos <= DJ_LINK_DB_RX_MAX ? (int)pos : -1;
}

/* Menu item: 0 parent, 1 id / number, 3 label 1, 5 label 2, 6 item type. */
static bool item_fields(const djlink_db_msg_t *m, uint32_t *num,
                        const djlink_db_arg_t **label1, const djlink_db_arg_t **label2,
                        uint32_t *item_type)
{
    if (m->arg_count < 7u ||
        m->args[1].type != DJLINK_DB_FIELD_INT32 ||
        m->args[3].type != DJLINK_DB_FIELD_STRING ||
        m->args[5].type != DJLINK_DB_FIELD_STRING ||
        m->args[6].type != DJLINK_DB_FIELD_INT32) {
        return false;
    }
    *num = m->args[1].num;
    *label1 = &m->args[3];
    *label2 = &m->args[5];
    *item_type = m->args[6].num & 0xffffu;
    return true;
}

static uint32_t menu_count(const djlink_db_msg_t *m)
{
    if (m->arg_count < 2u || m->args[1].num == DJ_LINK_DB_NO_ITEMS) {
        return 0;
    }
    return m->args[1].num;
}

static void list_item(dj_link_db_t *c, const djlink_db_msg_t *m)
{
    uint32_t id;
    uint32_t item_type;
    const djlink_db_arg_t *l1;
    const djlink_db_arg_t *l2;
    if (c->render_seen >= c->render_count || !item_fields(m, &id, &l1, &l2, &item_type)) {
        return;
    }
    dj_link_peer_track_t t;
    memset(&t, 0, sizeof(t));
    t.rekordbox_id = id;
    t.audio = c->audio;
    dj_link_db_utf16be_to_utf8(l1->bin, l1->bin_len, t.title, sizeof(t.title));
    /* The second label follows the player's sort; it is the artist only for
     * "title + artist" items. */
    if (item_type == DJ_LINK_DB_ITEM_TITLE_ARTIST) {
        dj_link_db_utf16be_to_utf8(l2->bin, l2->bin_len, t.artist, sizeof(t.artist));
    }
    if (c->io.track) {
        c->io.track(c->io.ctx, c->render_offset + c->render_seen, &t, false);
    }
    c->render_seen++;
}

static void detail_item(dj_link_db_t *c, const djlink_db_msg_t *m)
{
    uint32_t num;
    uint32_t item_type;
    const djlink_db_arg_t *l1;
    const djlink_db_arg_t *l2;
    if (!item_fields(m, &num, &l1, &l2, &item_type)) {
        return;
    }
    switch (item_type) {
    case DJ_LINK_DB_ITEM_TITLE:
        dj_link_db_utf16be_to_utf8(l1->bin, l1->bin_len, c->detail.title,
                                   sizeof(c->detail.title));
        break;
    case DJ_LINK_DB_ITEM_ARTIST:
        dj_link_db_utf16be_to_utf8(l1->bin, l1->bin_len, c->detail.artist,
                                   sizeof(c->detail.artist));
        break;
    case DJ_LINK_DB_ITEM_DURATION:
        c->detail.duration_s = num > 0xffffu ? 0xffffu : (uint16_t)num;
        break;
    case DJ_LINK_DB_ITEM_TEMPO:
        c->detail.bpm100 = num > 0xffffu ? 0u : (uint16_t)num;
        break;
    default:
        break;
    }
}

static void finish_detail(dj_link_db_t *c, uint32_t now_ms)
{
    c->detail.has_detail = true;
    if (c->io.track) {
        c->io.track(c->io.ctx, c->detail_index, &c->detail, true);
    }
    go_ready(c, now_ms);
    start_next(c, now_ms);
}

static void handle_msg(dj_link_db_t *c, const djlink_db_msg_t *m, uint32_t now_ms)
{
    if (c->phase == DJ_LINK_DB_SRV_WAIT_SETUP) {
        if (m->type != DJLINK_DB_TYPE_SUCCESS) {
            fail(c, "SETUP REFUSED");
            return;
        }
        go_ready(c, now_ms);
        start_next(c, now_ms);
        return;
    }
    if (m->txid != c->txid) {
        fail(c, "BAD TXID");
        return;
    }
    switch (c->phase) {
    case DJ_LINK_DB_LIST_WAIT_AVAIL:
        if (m->type != DJLINK_DB_TYPE_SUCCESS) {
            fail(c, "NO TRACK MENU");
            return;
        }
        c->list_total = menu_count(m);
        c->list_target = c->list_total < c->max_tracks ? c->list_total : c->max_tracks;
        if (c->io.list_begin) {
            c->io.list_begin(c->io.ctx, c->list_total);
        }
        if (c->list_target == 0) {
            c->list_done = true;
            go_ready(c, now_ms);
            start_next(c, now_ms);
            return;
        }
        request_render(c, 0, c->list_target < DJ_LINK_DB_RENDER_BATCH
                                 ? c->list_target : DJ_LINK_DB_RENDER_BATCH,
                       DJ_LINK_DB_LIST_WAIT_RENDER, now_ms);
        return;
    case DJ_LINK_DB_LIST_WAIT_RENDER:
        if (m->type == DJLINK_DB_TYPE_MENU_ITEM) {
            list_item(c, m);
        } else if (m->type == DJLINK_DB_TYPE_MENU_FOOTER) {
            uint32_t next = c->render_offset + c->render_count;
            if (next < c->list_target) {
                uint32_t left = c->list_target - next;
                request_render(c, next, left < DJ_LINK_DB_RENDER_BATCH
                                            ? left : DJ_LINK_DB_RENDER_BATCH,
                               DJ_LINK_DB_LIST_WAIT_RENDER, now_ms);
            } else {
                c->list_done = true;
                go_ready(c, now_ms);
                start_next(c, now_ms);
            }
        } else if (m->type != DJLINK_DB_TYPE_MENU_HEADER) {
            fail(c, "BAD RENDER REPLY");
        }
        return;
    case DJ_LINK_DB_DETAIL_WAIT_AVAIL:
        if (m->type != DJLINK_DB_TYPE_SUCCESS) {
            fail(c, "NO METADATA");
            return;
        }
        c->detail_count = menu_count(m);
        if (c->detail_count == 0) {
            finish_detail(c, now_ms);
            return;
        }
        request_render(c, 0, c->detail_count < DJ_LINK_DB_RENDER_BATCH
                                 ? c->detail_count : DJ_LINK_DB_RENDER_BATCH,
                       DJ_LINK_DB_DETAIL_WAIT_RENDER, now_ms);
        return;
    case DJ_LINK_DB_DETAIL_WAIT_RENDER:
        if (m->type == DJLINK_DB_TYPE_MENU_ITEM) {
            detail_item(c, m);
        } else if (m->type == DJLINK_DB_TYPE_MENU_FOOTER) {
            finish_detail(c, now_ms);
        } else if (m->type != DJLINK_DB_TYPE_MENU_HEADER) {
            fail(c, "BAD RENDER REPLY");
        }
        return;
    default:
        return;
    }
}

static void process(dj_link_db_t *c, uint32_t now_ms)
{
    for (;;) {
        switch (c->phase) {
        case DJ_LINK_DB_DISC_WAIT_PORT: {
            uint16_t port = 0;
            if (c->rx_len < 2u) {
                return;
            }
            (void)djlink_db_port_query_reply_parse(c->rx, c->rx_len, &port);
            close_connection(c); /* the discovery connection is one-shot */
            if (port == 0u || port == 0xffffu) {
                c->phase = DJ_LINK_DB_IDLE;
                fail(c, "NO DB SERVER");
                return;
            }
            connect_to(c, port, DJ_LINK_DB_SRV_CONNECTING, now_ms);
            return;
        }
        case DJ_LINK_DB_SRV_WAIT_GREETING: {
            uint8_t greeting[DJLINK_DB_SETUP_LEN];
            if (c->rx_len < DJLINK_DB_SETUP_LEN) {
                return;
            }
            (void)djlink_db_setup_build(greeting, sizeof(greeting));
            if (memcmp(c->rx, greeting, sizeof(greeting)) != 0) {
                fail(c, "BAD GREETING");
                return;
            }
            c->rx_len -= DJLINK_DB_SETUP_LEN;
            memmove(c->rx, &c->rx[DJLINK_DB_SETUP_LEN], c->rx_len);
            send_tx(c, djlink_db_context_setup_build(c->our_number, c->tx, sizeof(c->tx)),
                    DJ_LINK_DB_SRV_WAIT_SETUP, now_ms);
            continue;
        }
        case DJ_LINK_DB_SRV_WAIT_SETUP:
        case DJ_LINK_DB_LIST_WAIT_AVAIL:
        case DJ_LINK_DB_LIST_WAIT_RENDER:
        case DJ_LINK_DB_DETAIL_WAIT_AVAIL:
        case DJ_LINK_DB_DETAIL_WAIT_RENDER: {
            int n = dj_link_db_msg_size(c->rx, c->rx_len);
            if (n == 0) {
                return;
            }
            if (n < 0) {
                fail(c, "BAD REPLY");
                return;
            }
            djlink_db_msg_t m;
            (void)djlink_db_msg_parse(c->rx, (size_t)n, &m);
            handle_msg(c, &m, now_ms);
            if (!has_connection(c)) {
                return;
            }
            c->rx_len -= (size_t)n;
            memmove(c->rx, &c->rx[n], c->rx_len);
            /* Progress (a streamed item or a new request) restarts the clock. */
            c->deadline_ms = now_ms + c->step_timeout_ms;
            continue;
        }
        default:
            c->rx_len = 0; /* unsolicited bytes while idle */
            return;
        }
    }
}

void dj_link_db_init(dj_link_db_t *c, const dj_link_db_io_t *io, uint32_t max_tracks)
{
    memset(c, 0, sizeof(*c));
    if (io) {
        c->io = *io;
    }
    c->discovery_port = DJLINK_DBSERVER_DISCOVERY_PORT;
    c->step_timeout_ms = DJ_LINK_DB_STEP_TIMEOUT_MS;
    c->max_tracks = max_tracks;
}

void dj_link_db_start(dj_link_db_t *c, uint32_t peer_ip, uint8_t peer_number,
                      uint8_t slot, uint8_t our_number, uint32_t now_ms)
{
    close_connection(c);
    c->peer_ip = peer_ip;
    c->peer_number = peer_number;
    c->slot = slot;
    c->our_number = our_number;
    c->list_done = false;
    c->list_total = 0;
    c->list_target = 0;
    c->error[0] = '\0';
    c->phase = DJ_LINK_DB_IDLE;
    if (peer_ip == 0u || our_number == 0u || peer_number == 0u || peer_number == our_number) {
        fail(c, our_number == 0u ? "NOT JOINED" : "BAD PEER");
        return;
    }
    connect_to(c, c->discovery_port, DJ_LINK_DB_DISC_CONNECTING, now_ms);
}

void dj_link_db_stop(dj_link_db_t *c)
{
    close_connection(c);
    c->phase = DJ_LINK_DB_IDLE;
    c->peer_number = 0;
    c->list_done = false;
}

void dj_link_db_on_connected(dj_link_db_t *c, uint32_t now_ms)
{
    if (c->phase == DJ_LINK_DB_DISC_CONNECTING) {
        send_tx(c, djlink_db_port_query_build(c->tx, sizeof(c->tx)),
                DJ_LINK_DB_DISC_WAIT_PORT, now_ms);
    } else if (c->phase == DJ_LINK_DB_SRV_CONNECTING) {
        send_tx(c, djlink_db_setup_build(c->tx, sizeof(c->tx)),
                DJ_LINK_DB_SRV_WAIT_GREETING, now_ms);
    }
}

void dj_link_db_on_data(dj_link_db_t *c, const uint8_t *buf, size_t len, uint32_t now_ms)
{
    if (!has_connection(c) || !buf || len == 0) {
        return;
    }
    if (len > sizeof(c->rx) - c->rx_len) {
        fail(c, "REPLY TOO LARGE");
        return;
    }
    memcpy(&c->rx[c->rx_len], buf, len);
    c->rx_len += len;
    process(c, now_ms);
}

void dj_link_db_on_closed(dj_link_db_t *c, uint32_t now_ms)
{
    (void)now_ms;
    if (c->phase == DJ_LINK_DB_READY) {
        c->phase = DJ_LINK_DB_IDLE; /* idle session dropped: reconnect on demand */
        c->rx_len = 0;
    } else if (has_connection(c)) {
        fail(c, "CONNECTION LOST");
    }
}

void dj_link_db_poll(dj_link_db_t *c, uint32_t now_ms)
{
    uint32_t index;
    uint32_t id;
    switch (c->phase) {
    case DJ_LINK_DB_IDLE:
        /* Reconnect only when the owner wants metadata for a listed row. */
        if (c->peer_number != 0u && c->list_done && c->io.next_detail &&
            c->io.next_detail(c->io.ctx, &index, &id)) {
            connect_to(c, c->discovery_port, DJ_LINK_DB_DISC_CONNECTING, now_ms);
        }
        return;
    case DJ_LINK_DB_FAILED:
        return;
    case DJ_LINK_DB_READY:
        start_next(c, now_ms);
        if (c->phase == DJ_LINK_DB_READY &&
            (uint32_t)(now_ms - c->idle_since_ms) >= DJ_LINK_DB_IDLE_CLOSE_MS) {
            close_connection(c);
            c->phase = DJ_LINK_DB_IDLE;
        }
        return;
    default:
        if ((int32_t)(now_ms - c->deadline_ms) >= 0) {
            char reason[sizeof(c->error)];
            snprintf(reason, sizeof(reason), "TIMEOUT %s", phase_name(c->phase));
            fail(c, reason);
        }
        return;
    }
}

dj_link_db_phase_t dj_link_db_phase(const dj_link_db_t *c)
{
    return c->phase;
}

bool dj_link_db_list_done(const dj_link_db_t *c)
{
    return c->list_done;
}

const char *dj_link_db_error(const dj_link_db_t *c)
{
    return c->error;
}

dj_link_peer_load_t dj_link_peer_load_check(const dj_link_peer_track_t *t,
                                            char *reason, size_t cap)
{
    dj_link_peer_load_t verdict = DJ_LINK_PEER_LOAD_METADATA_ONLY;
    const char *text = "metadata only: peer audio not reachable";
    if (!t || t->rekordbox_id == 0u) {
        verdict = DJ_LINK_PEER_LOAD_NO_TRACK;
        text = "no track";
    } else if (t->audio == DJ_LINK_PEER_AUDIO_NFS) {
        verdict = DJ_LINK_PEER_LOAD_OK;
        text = "NFS download to the SD cache";
    }
    if (reason && cap) {
        snprintf(reason, cap, "%s", text);
    }
    return verdict;
}

uint32_t dj_link_peer_track_key(uint32_t peer_ip, uint8_t peer_number, uint32_t rekordbox_id)
{
    uint8_t bytes[9];
    uint32_t h = 2166136261u;
    bytes[0] = (uint8_t)(peer_ip >> 24);
    bytes[1] = (uint8_t)(peer_ip >> 16);
    bytes[2] = (uint8_t)(peer_ip >> 8);
    bytes[3] = (uint8_t)peer_ip;
    bytes[4] = peer_number;
    bytes[5] = (uint8_t)(rekordbox_id >> 24);
    bytes[6] = (uint8_t)(rekordbox_id >> 16);
    bytes[7] = (uint8_t)(rekordbox_id >> 8);
    bytes[8] = (uint8_t)rekordbox_id;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h != 0u ? h : 1u;
}
