/*
 * PictoChat radio modem for the ESP32-C3 and ESP32-S3 (native USB = USB Serial/JTAG) and for the classic ESP32,
 * whose only USB link is a USB-to-UART chip (e.g. the CH340 on an ESP32-2432S028): there it talks on UART0 at
 * MODEM_UART_BAUD. The link_* functions below hide which of the two it is.
 * An optional screen on the modem board is driven with 'D' and 'B' frames from the PC: see lcd.c.
 *
 * The PC runs the protocol (pictochat.py host --serial /dev/ttyACM0); this firmware only moves raw 802.11
 * frames between the USB link and the radio:
 *   - frames from the PC are sent with esp_wifi_80211_tx at a fixed rate (default 2 Mbit/s, long preamble;
 *     the DS ignores multiplay polls at 1 Mbit/s),
 *   - frames from the air (promiscuous mode) are passed to the PC with RSSI and rate,
 *   - the station MAC is the host MAC, so the hardware ACKs the DS's auth/assoc requests.
 *
 * Multiplay offload (optional, enabled with 'm'): the poll / reply / ACK cycle runs here instead of on the PC, so
 * polls go out every few ms without a USB round trip per poll. The PC keeps the PictoChat logic: it pushes the
 * packets the polls should carry ('Q'), tells us which DSes are associated ('A') and gets the DS replies ('R').
 *
 * USB framing, both directions: AA 55 | type u8 | len u16 LE | payload | xor8(type, len bytes, payload)
 *   PC -> ESP  'C' config: channel u8, mac[6], rate u8 (wifi_phy_rate_t), all u8 (1 = forward every frame)
 *              'T' transmit: 802.11 frame without FCS
 *              'P' ping
 *              'm' multiplay settings: on u8, clienttime u16, cmd_word u16, cmdcount u16, period_us u32,
 *                  reply_wait_us u32, cycle u8 (address the clients on every Nth poll)
 *              'A' associated DSes: n u8, then n x (aid u8, mac[6]), optional u8 1 = open with an empty poll again
 *              'Q' queue a packet for the polls: port u8, min_sent u8, max_sent u8, payload
 *              'X' clear the packet queue: epoch u8 (echoed in 'M', so the PC can tell old reports from new ones)
 *   ESP -> PC  'R' received: rssi i8, rate u8 (wifi_phy_rate_t), timestamp u32 LE (us), 802.11 frame without FCS
 *              'S' status text
 *              'E' transmit error: esp_err_t i32 LE, frame control u16 LE
 *              'M' multiplay report: epoch u8, qlen u16, queued u16 (packets received since the last 'X'),
 *                  polls u32, replies u32, acks u32, done u32, txerr u32, answered u16 (bit per AID that replied
 *                  since the previous report: lets the PC notice a DS that left without a deauth)
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
/* UART0 goes through the board's USB-to-UART chip; the host opens the port at the same rate */
#define MODEM_UART_BAUD 921600
#endif

/* The Wi-Fi library refuses auth/assoc responses in esp_wifi_80211_tx. 0 = frame allowed (the caller branches
 * to its error path on non-zero). Linked with -zmuldefs, this definition replaces the library's. */
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    return 0;
}

#define MAX_FRAME 600
#define RXQ_LEN 48

typedef struct {
    uint8_t channel;
    uint8_t mac[6];
    uint8_t rate;
    uint8_t all;
} modem_cfg_t;

typedef struct {
    uint16_t len;
    int8_t rssi;
    uint8_t rate;
    uint32_t ts;
    uint8_t data[MAX_FRAME];
} rx_item_t;

static modem_cfg_t cfg = { 1, {0x00, 0x1b, 0x7a, 0x00, 0x00, 0x01}, WIFI_PHY_RATE_2M_L, 0 };
static QueueHandle_t rxq;
static SemaphoreHandle_t usb_lock;
static volatile uint32_t n_rx, n_rx_drop, n_tx, n_tx_err;

/* ---------------------------------------------------------------- multiplay state */

#define MP_MAX_PL 256
#define MP_QLEN 128
#define MP_MAX_CLIENTS 15
#define BIT_TIMER 1u
#define BIT_REPLY 2u
#define BIT_KICK 4u

typedef struct {
    uint8_t port, min_sent, max_sent, sent;
    uint16_t len;
    uint8_t data[MP_MAX_PL];
} mp_item_t;

typedef struct {
    uint8_t on;
    uint16_t clienttime, cmd_word, cmdcount;
    uint32_t period_us, reply_wait_us;
    uint8_t cycle;
} mp_cfg_t;

static mp_cfg_t mpc = { 0, 0x03E6, 0, 0x54, 2500, 4000, 1 };
static portMUX_TYPE mp_mux = portMUX_INITIALIZER_UNLOCKED;
static mp_item_t mpq[MP_QLEN];              /* ring buffer, guarded by mp_mux */
static uint16_t mpq_head, mpq_len, mpq_queued;
static uint8_t mp_epoch;
static uint32_t mpq_gen;                    /* changes whenever the head item changes (pop or clear) */
static uint8_t cl_n, cl_aid[MP_MAX_CLIENTS], cl_mac[MP_MAX_CLIENTS][6];
static uint16_t cl_full;                    /* bit per associated AID */
static volatile uint16_t mp_pending;        /* clients that have not answered the current poll yet */
static volatile bool mp_first;              /* next poll is the opening empty one (after a new DS associated) */
static volatile uint32_t mp_polls, mp_replies, mp_acks, mp_done, mp_txerr;
static volatile uint16_t mp_answered;       /* AIDs that replied since the last 'M' report */
static TaskHandle_t mp_task_h;
static esp_timer_handle_t mp_timer;
/* last non-empty reply payload per AID, to pass each DS packet to the PC once instead of on every poll */
static uint32_t last_hash[16];
static int64_t last_fwd[16];

/* ---------------------------------------------------------------- USB link */

/* link_read() returns the number of bytes read (0 after 20 ms without data) */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
static void link_init(void)
{
    usb_serial_jtag_driver_config_t usb_config = { .tx_buffer_size = 8192, .rx_buffer_size = 8192 };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
}
static void link_write(const void *data, size_t length)
{
    usb_serial_jtag_write_bytes(data, length, pdMS_TO_TICKS(50));
}
static int link_read(uint8_t *buffer, size_t size)
{
    return usb_serial_jtag_read_bytes(buffer, size, pdMS_TO_TICKS(20));
}
#else
static void link_init(void)
{
    uart_config_t uart_config = { .baud_rate = MODEM_UART_BAUD, .data_bits = UART_DATA_8_BITS,
                                  .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
                                  .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 8192, 8192, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
}
static void link_write(const void *data, size_t length)
{
    uart_write_bytes(UART_NUM_0, data, length);
}
static int link_read(uint8_t *buffer, size_t size)
{
    return uart_read_bytes(UART_NUM_0, buffer, size, pdMS_TO_TICKS(20));
}
#endif

static void usb_send(uint8_t type, const void *a, size_t alen, const void *b, size_t blen)
{
    uint8_t hdr[5] = { 0xAA, 0x55, type, (uint8_t)((alen + blen) & 0xFF), (uint8_t)((alen + blen) >> 8) };
    uint8_t x = hdr[2] ^ hdr[3] ^ hdr[4];
    for (size_t i = 0; i < alen; i++) x ^= ((const uint8_t *)a)[i];
    for (size_t i = 0; i < blen; i++) x ^= ((const uint8_t *)b)[i];
    xSemaphoreTake(usb_lock, portMAX_DELAY);
    link_write(hdr, 5);
    if (alen) link_write(a, alen);
    if (blen) link_write(b, blen);
    link_write(&x, 1);
    xSemaphoreGive(usb_lock);
}

static void status(const char *fmt, ...)
{
    char s[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    usb_send('S', s, n < (int)sizeof s ? n : (int)sizeof s - 1, NULL, 0);
}

/* lcd.c, the optional screen on the modem board: lcd_handle() takes the screen frames ('D', 'B') and returns true
 * for them; lcd.c reports through modem_status(), an 'S' status line like status() */
bool lcd_handle(uint8_t type, const uint8_t *payload, uint16_t length);
void modem_status(const char *format, ...)
{
    char text[160];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length < 0) return;
    usb_send('S', text, length < (int)sizeof text ? length : (int)sizeof text - 1, NULL, 0);
}

/* ---------------------------------------------------------------- radio */

static bool forward(const uint8_t *f, int len)
{
    if (cfg.all) return true;
    if (len < 16) return false;
    const uint8_t *a1 = f + 4;
    if (memcmp(a1, cfg.mac, 6) == 0) return true;                        /* to us: auth, assoc, replies, deauth */
    if (a1[0] == 0x03 && a1[1] == 0x09 && a1[2] == 0xBF) return true;    /* Nintendo multiplay multicast */
    if (f[0] == 0x80 && len > 36) {                                      /* beacon with the Nintendo element */
        for (int i = 36; i + 6 <= len; i += 2 + f[i + 1]) {
            if (f[i] == 0xDD && f[i + 2] == 0x00 && f[i + 3] == 0x09 && f[i + 4] == 0xBF) return true;
        }
    }
    return false;
}

static const uint8_t MP_CMD_ADDR[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x00};
static const uint8_t MP_ACK_ADDR[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x03};
static const uint8_t MP_REPLY_ADDR[6] = {0x03, 0x09, 0xBF, 0x00, 0x00, 0x10};

/* A DS reply to our poll (ToDS data frame, a1 = us, a2 = DS, a3 = 03:09:BF:00:00:10). With the offload on: mark the
 * DS as answered, wake the poll task, and return whether the PC needs the frame (a new non-empty packet). */
static bool mp_reply(const uint8_t *f, int len)
{
    int aid = -1;
    portENTER_CRITICAL(&mp_mux);
    for (int i = 0; i < cl_n; i++) {
        if (memcmp(cl_mac[i], f + 10, 6) == 0) { aid = cl_aid[i]; break; }
    }
    portEXIT_CRITICAL(&mp_mux);
    if (aid < 0) return true;                                            /* not one of ours: let the PC log it */
    mp_replies++;
    mp_answered |= 1u << aid;
    if (mp_pending & (1u << aid)) {
        mp_pending &= ~(1u << aid);
        if (mp_task_h) xTaskNotify(mp_task_h, BIT_REPLY, eSetBits);
    }
    if (len < 26) return false;
    const uint8_t *body = f + 24;
    int plen = (body[0] & 0xFF) * 2;                                     /* WM word: payload length in halfwords */
    if (plen == 0) return false;                                         /* idle reply */
    if (plen + 2 > len - 24) plen = len - 26;
    uint32_t h = 2166136261u;                                            /* FNV-1a of word + payload */
    for (int i = 0; i < plen + 2; i++) h = (h ^ body[i]) * 16777619u;
    int64_t now = esp_timer_get_time();
    if (h == last_hash[aid] && now - last_fwd[aid] < 500000) return false;  /* the DS repeats a packet on every poll */
    last_hash[aid] = h;
    last_fwd[aid] = now;
    return true;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    int len = p->rx_ctrl.sig_len - 4;                                    /* strip the FCS */
    if (len < 10) return;
    if (mpc.on && type == WIFI_PKT_DATA && len >= 24 && memcmp(p->payload + 16, MP_REPLY_ADDR, 6) == 0) {
        if (!mp_reply(p->payload, len)) return;
    } else if (!forward(p->payload, len)) {
        return;
    }
    static rx_item_t it;                                                 /* the Wi-Fi task calls us one at a time */
    it.len = len > MAX_FRAME ? MAX_FRAME : len;
    it.rssi = p->rx_ctrl.rssi;
    it.rate = p->rx_ctrl.rate;
    it.ts = p->rx_ctrl.timestamp;
    memcpy(it.data, p->payload, it.len);
    if (xQueueSend(rxq, &it, 0) == pdTRUE) n_rx++;
    else n_rx_drop++;
}

static esp_err_t wifi_apply(void)
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B));
    esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, cfg.mac);
    if (e != ESP_OK) status("set_mac failed: %d", e);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_promiscuous_filter_t flt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&flt);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(cfg.channel, WIFI_SECOND_CHAN_NONE));
    e = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, (wifi_phy_rate_t)cfg.rate);
    if (e != ESP_OK) status("config_80211_tx_rate failed: %d", e);
    uint8_t m[6];
    esp_wifi_get_mac(WIFI_IF_STA, m);
    status("ready ch=%d mac=%02x:%02x:%02x:%02x:%02x:%02x rate=%d all=%d mp=1", cfg.channel,
           m[0], m[1], m[2], m[3], m[4], m[5], cfg.rate, cfg.all);
    return ESP_OK;
}

/* ---------------------------------------------------------------- multiplay poll loop */

static uint16_t mp_seq;

static int dot11_hdr(uint8_t *b, uint8_t fc0, uint8_t fc1, const uint8_t *a1)
{
    b[0] = fc0; b[1] = fc1; b[2] = 0; b[3] = 0;
    memcpy(b + 4, a1, 6);
    memcpy(b + 10, cfg.mac, 6);
    memcpy(b + 16, cfg.mac, 6);
    uint16_t s = (uint16_t)((mp_seq++ & 0xFFF) << 4);
    b[22] = (uint8_t)s; b[23] = (uint8_t)(s >> 8);
    return 24;
}

static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }

static void mp_tx(const uint8_t *f, int len)
{
    if (esp_wifi_80211_tx(WIFI_IF_STA, f, len, false) != ESP_OK) mp_txerr++;
}

static void mp_timer_cb(void *arg)
{
    if (mp_task_h) xTaskNotify(mp_task_h, BIT_TIMER, eSetBits);
}

/* Sleep until `us` from now (esp_timer: microsecond resolution, unlike the 1 ms FreeRTOS tick). */
static void mp_sleep_us(int64_t us)
{
    if (us < 30) return;
    uint32_t bits = 0;
    ulTaskNotifyValueClear(NULL, BIT_TIMER);
    esp_timer_stop(mp_timer);
    esp_timer_start_once(mp_timer, (uint64_t)us);
    do {
        if (xTaskNotifyWait(0, BIT_TIMER, &bits, pdMS_TO_TICKS(100)) == pdFALSE) break;
    } while (!(bits & BIT_TIMER));
}

/* The idle packet: port 0xC type 5 with a random value (renewed every 5 idle polls) and the roster: the host's and
 * each client's MAC, 16-bit pairs swapped, in AID order (MultiplayHost.roster / _type in pictochat.py). */
static int mp_idle(uint8_t *pl, uint32_t val)
{
    memset(pl, 0, 8 + 0x60);
    put16(pl, 5); put16(pl + 2, 0x68);
    pl[4] = (uint8_t)val; pl[5] = (uint8_t)(val >> 8); pl[6] = (uint8_t)(val >> 16); pl[7] = (uint8_t)(val >> 24);
    uint8_t macs[1 + MP_MAX_CLIENTS][6];
    int n = 0;
    memcpy(macs[n++], cfg.mac, 6);
    portENTER_CRITICAL(&mp_mux);
    for (int aid = 1; aid < 16; aid++) {
        for (int i = 0; i < cl_n; i++) {
            if (cl_aid[i] == aid) memcpy(macs[n++], cl_mac[i], 6);
        }
    }
    portEXIT_CRITICAL(&mp_mux);
    for (int k = 0; k < n && k < 16; k++) {
        for (int j = 0; j < 6; j++) pl[8 + k * 6 + j] = macs[k][j ^ 1];
    }
    return 8 + 0x60;
}

static void mp_task(void *arg)
{
    static uint8_t fr[24 + 6 + MP_MAX_PL + 4];
    static uint8_t pl[MP_MAX_PL];
    uint16_t ctr[16] = {0};
    int cyc = -1;
    uint32_t idle_n = 0, idle_val = 0, gen_sent = 0;
    int64_t next_keepalive = 0;
    for (;;) {
        if (!mpc.on || cl_full == 0) {
            uint32_t bits;
            xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(50));
            continue;
        }
        int64_t start = esp_timer_get_time();
        uint16_t full = cl_full, mask = 0, clienttime = mpc.clienttime, word = mpc.cmd_word;
        int dlen = 0;
        bool carried = false;
        uint8_t cycle = mpc.cycle ? mpc.cycle : 1;
        if (mp_first) {                                  /* the real host opens with an empty, unaddressed poll */
            mp_first = false;
            next_keepalive = start + 1000000;
            cyc = -1;
            word = 0;
        } else if (start >= next_keepalive) {            /* once a second: empty poll to everyone, clienttime 0xD6 */
            next_keepalive = start + 1000000;
            mask = full;
            clienttime = 0x00D6;
        } else {
            int plen = -1;
            uint8_t port = 0xC;
            portENTER_CRITICAL(&mp_mux);
            if (mpq_len) {
                mp_item_t *it = &mpq[mpq_head];
                cyc = (cyc + 1) % cycle;
                if (it->sent == 0) cyc = 0;              /* a new packet always starts on an addressed poll */
                port = it->port;
                plen = it->len;
                memcpy(pl, it->data, plen);
                it->sent++;
                if (it->sent >= it->max_sent) {          /* sent the maximum number of times: done */
                    mpq_head = (mpq_head + 1) % MP_QLEN;
                    mpq_len--;
                    mpq_gen++;
                    mp_done++;
                } else {
                    carried = true;
                    gen_sent = mpq_gen;
                }
            }
            portEXIT_CRITICAL(&mp_mux);
            if (plen < 0) {
                cyc = (cyc + 1) % cycle;
                if (idle_n++ % 5 == 0) idle_val = esp_random();
                plen = mp_idle(pl, idle_val);
            }
            mask = cyc == 0 ? full : 0;
            ctr[port & 15] = (uint16_t)(ctr[port & 15] % 0xFFFF + 1);
            word = (uint16_t)(0x1000 | (port & 15) << 8 | (plen / 2));
            memcpy(fr + 30, pl, plen);
            put16(fr + 30 + plen, ctr[port & 15]);
            put16(fr + 32 + plen, mask);
            dlen = plen + 4;
        }
        dot11_hdr(fr, 0x28, 0x02, MP_CMD_ADDR);
        put16(fr + 24, clienttime);
        put16(fr + 26, mask);
        put16(fr + 28, word);
        ulTaskNotifyValueClear(NULL, UINT32_MAX);
        mp_pending = mask;
        mp_tx(fr, 30 + dlen);
        mp_polls++;
        if (mask) {                                      /* wait for every addressed DS, at most reply_wait */
            uint32_t bits = 0;
            esp_timer_stop(mp_timer);
            esp_timer_start_once(mp_timer, mpc.reply_wait_us);
            while (mp_pending) {
                if (xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(100)) == pdFALSE) break;
                if (bits & BIT_TIMER) break;
            }
            esp_timer_stop(mp_timer);
        }
        uint16_t fail = mp_pending;
        mp_pending = 0;
        uint8_t ack[28];
        dot11_hdr(ack, 0x18, 0x02, MP_ACK_ADDR);
        put16(ack + 24, mpc.cmdcount);
        put16(ack + 26, fail);
        mp_tx(ack, 28);
        mp_acks++;
        if (carried && mask && fail == 0) {              /* every addressed DS has the packet: move on */
            portENTER_CRITICAL(&mp_mux);
            if (mpq_len && mpq_gen == gen_sent && mpq[mpq_head].sent >= mpq[mpq_head].min_sent) {
                mpq_head = (mpq_head + 1) % MP_QLEN;
                mpq_len--;
                mpq_gen++;
                mp_done++;
            }
            portEXIT_CRITICAL(&mp_mux);
        }
        mp_sleep_us(start + mpc.period_us - esp_timer_get_time());
    }
}

static void mp_handle(uint8_t type, const uint8_t *p, uint16_t len)
{
    if (type == 'm' && len >= 16) {
        mpc.on = p[0];
        mpc.clienttime = (uint16_t)(p[1] | p[2] << 8);
        mpc.cmd_word = (uint16_t)(p[3] | p[4] << 8);
        mpc.cmdcount = (uint16_t)(p[5] | p[6] << 8);
        mpc.period_us = (uint32_t)p[7] | (uint32_t)p[8] << 8 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 24;
        mpc.reply_wait_us = (uint32_t)p[11] | (uint32_t)p[12] << 8 | (uint32_t)p[13] << 16 | (uint32_t)p[14] << 24;
        mpc.cycle = p[15];
        if (mpc.period_us < 500) mpc.period_us = 500;
        if (mpc.reply_wait_us < 200) mpc.reply_wait_us = 200;
        status("mp %s period=%luus reply_wait=%luus clienttime=0x%x cycle=%d", mpc.on ? "on" : "off",
               (unsigned long)mpc.period_us, (unsigned long)mpc.reply_wait_us, mpc.clienttime, mpc.cycle);
        xTaskNotify(mp_task_h, BIT_KICK, eSetBits);
    } else if (type == 'A' && len >= 1) {
        int n = p[0];
        if (n > MP_MAX_CLIENTS || len < 1 + 7 * n) return;
        uint16_t old = cl_full, full = 0;
        portENTER_CRITICAL(&mp_mux);
        cl_n = 0;
        for (int i = 0; i < n; i++) {
            uint8_t aid = p[1 + 7 * i];
            if (aid < 1 || aid > 15) continue;
            cl_aid[cl_n] = aid;
            memcpy(cl_mac[cl_n], p + 2 + 7 * i, 6);
            cl_n++;
            full |= 1u << aid;
        }
        cl_full = full;
        portEXIT_CRITICAL(&mp_mux);
        uint16_t added = full & ~old;
        for (int aid = 1; aid < 16; aid++) {
            if (added & (1u << aid)) last_hash[aid] = 0;
        }
        if (added || (len > 1 + 7 * n && p[1 + 7 * n])) mp_first = true;   /* new DS, or the PC asks (re-assoc) */
        xTaskNotify(mp_task_h, BIT_KICK, eSetBits);
    } else if (type == 'Q' && len >= 3) {
        uint16_t plen = len - 3;
        bool ok = false;
        if (plen <= MP_MAX_PL) {
            portENTER_CRITICAL(&mp_mux);
            if (mpq_len < MP_QLEN) {
                mp_item_t *it = &mpq[(mpq_head + mpq_len) % MP_QLEN];
                it->port = p[0];
                it->min_sent = p[1];
                it->max_sent = p[2] ? p[2] : 1;
                it->sent = 0;
                it->len = plen;
                memcpy(it->data, p + 3, plen);
                mpq_len++;
                ok = true;
            }
            mpq_queued++;
            portEXIT_CRITICAL(&mp_mux);
        }
        if (!ok) status("mp: packet dropped (queue full or %u bytes too long)", plen);
    } else if (type == 'X' && len >= 1) {
        portENTER_CRITICAL(&mp_mux);
        mpq_head = 0;
        mpq_len = 0;
        mpq_queued = 0;
        mpq_gen++;
        mp_epoch = p[0];
        portEXIT_CRITICAL(&mp_mux);
    }
}

static void mp_report(void)
{
    uint8_t r[27];
    portENTER_CRITICAL(&mp_mux);
    r[0] = mp_epoch;
    put16(r + 1, mpq_len);
    put16(r + 3, mpq_queued);
    put16(r + 25, mp_answered);
    mp_answered = 0;
    portEXIT_CRITICAL(&mp_mux);
    uint32_t v[5] = { mp_polls, mp_replies, mp_acks, mp_done, mp_txerr };
    memcpy(r + 5, v, sizeof v);                          /* little-endian RISC-V */
    usb_send('M', r, sizeof r, NULL, 0);
}

static void handle(uint8_t type, const uint8_t *p, uint16_t len)
{
    if (type == 'T') {
        esp_err_t e = esp_wifi_80211_tx(WIFI_IF_STA, p, len, false);
        if (e == ESP_OK) {
            n_tx++;
        } else {
            n_tx_err++;
            int32_t ee = e;
            uint16_t fc = len >= 2 ? (uint16_t)(p[0] | p[1] << 8) : 0;
            usb_send('E', &ee, 4, &fc, 2);
        }
    } else if (type == 'C' && len >= 8) {
        cfg.channel = p[0];
        memcpy(cfg.mac, p + 1, 6);
        cfg.rate = p[7];
        cfg.all = len >= 9 ? p[8] : 0;
        wifi_apply();
    } else if (type == 'P') {
        status("pong rx=%lu rxdrop=%lu tx=%lu txerr=%lu mp_polls=%lu mp_replies=%lu mp_txerr=%lu",
               (unsigned long)n_rx, (unsigned long)n_rx_drop, (unsigned long)n_tx, (unsigned long)n_tx_err,
               (unsigned long)mp_polls, (unsigned long)mp_replies, (unsigned long)mp_txerr);
    } else if (!lcd_handle(type, p, len)) {
        mp_handle(type, p, len);
    }
}

/* ---------------------------------------------------------------- tasks */

static void usb_rx_task(void *arg)
{
    static uint8_t frame[8192];                    /* a screen band ('B', lcd.c) takes up to 8 + 3 x 320 x 8 bytes */
    uint8_t buf[256];
    int state = 0, type = 0;
    uint16_t len = 0, got = 0;
    uint8_t x = 0;
    for (;;) {
        int n = link_read(buf, sizeof buf);
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            switch (state) {
            case 0: state = c == 0xAA ? 1 : 0; break;
            case 1: state = c == 0x55 ? 2 : (c == 0xAA ? 1 : 0); break;
            case 2: type = c; x = c; state = 3; break;
            case 3: len = c; x ^= c; state = 4; break;
            case 4:
                len |= c << 8; x ^= c; got = 0;
                state = len > sizeof frame ? 0 : (len ? 5 : 6);
                break;
            case 5:
                frame[got++] = c; x ^= c;
                if (got == len) state = 6;
                break;
            case 6:
                if (c == x) handle(type, frame, len);
                state = 0;
                break;
            }
        }
    }
}

static void usb_tx_task(void *arg)
{
    static rx_item_t it;
    int64_t last_report = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (mpc.on && now - last_report >= 50000) {     /* multiplay report every 50 ms while the offload is on */
            last_report = now;
            mp_report();
        }
        if (xQueueReceive(rxq, &it, pdMS_TO_TICKS(20)) == pdTRUE) {
            uint8_t hdr[6] = { (uint8_t)it.rssi, it.rate,
                               (uint8_t)it.ts, (uint8_t)(it.ts >> 8), (uint8_t)(it.ts >> 16), (uint8_t)(it.ts >> 24) };
            usb_send('R', hdr, 6, it.data, it.len);
        }
    }
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    usb_lock = xSemaphoreCreateMutex();
    rxq = xQueueCreate(RXQ_LEN, sizeof(rx_item_t));
    link_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_apply();

    esp_timer_create_args_t targs = { .callback = mp_timer_cb, .name = "mp" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &mp_timer));
    xTaskCreate(mp_task, "mp", 4096, NULL, 15, &mp_task_h);
    xTaskCreate(usb_tx_task, "usb_tx", 4096, NULL, 10, NULL);
    xTaskCreate(usb_rx_task, "usb_rx", 4096, NULL, 11, NULL);
}
