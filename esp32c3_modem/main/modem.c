/*
 * PictoChat radio modem for the ESP32-C3 (USB-C = USB Serial/JTAG).
 *
 * The PC runs the protocol (pictochat.py host --serial /dev/ttyACM0); this firmware only moves raw 802.11
 * frames between the USB link and the radio:
 *   - frames from the PC are sent with esp_wifi_80211_tx at a fixed rate (default 2 Mbit/s, long preamble;
 *     the DS ignores multiplay polls at 1 Mbit/s),
 *   - frames from the air (promiscuous mode) are passed to the PC with RSSI and rate,
 *   - the station MAC is the host MAC, so the hardware ACKs the DS's auth/assoc requests.
 *
 * USB framing, both directions: AA 55 | type u8 | len u16 LE | payload | xor8(type, len bytes, payload)
 *   PC -> ESP  'C' config: channel u8, mac[6], rate u8 (wifi_phy_rate_t), all u8 (1 = forward every frame)
 *              'T' transmit: 802.11 frame without FCS
 *              'P' ping
 *   ESP -> PC  'R' received: rssi i8, rate u8 (wifi_phy_rate_t), timestamp u32 LE (us), 802.11 frame without FCS
 *              'S' status text
 *              'E' transmit error: esp_err_t i32 LE, frame control u16 LE
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
#include "nvs_flash.h"
#include "driver/usb_serial_jtag.h"

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

/* ---------------------------------------------------------------- USB link */

static void usb_send(uint8_t type, const void *a, size_t alen, const void *b, size_t blen)
{
    uint8_t hdr[5] = { 0xAA, 0x55, type, (uint8_t)((alen + blen) & 0xFF), (uint8_t)((alen + blen) >> 8) };
    uint8_t x = hdr[2] ^ hdr[3] ^ hdr[4];
    for (size_t i = 0; i < alen; i++) x ^= ((const uint8_t *)a)[i];
    for (size_t i = 0; i < blen; i++) x ^= ((const uint8_t *)b)[i];
    xSemaphoreTake(usb_lock, portMAX_DELAY);
    usb_serial_jtag_write_bytes(hdr, 5, pdMS_TO_TICKS(50));
    if (alen) usb_serial_jtag_write_bytes(a, alen, pdMS_TO_TICKS(50));
    if (blen) usb_serial_jtag_write_bytes(b, blen, pdMS_TO_TICKS(50));
    usb_serial_jtag_write_bytes(&x, 1, pdMS_TO_TICKS(50));
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

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    int len = p->rx_ctrl.sig_len - 4;                                    /* strip the FCS */
    if (len < 10 || !forward(p->payload, len)) return;
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
    status("ready ch=%d mac=%02x:%02x:%02x:%02x:%02x:%02x rate=%d all=%d", cfg.channel,
           m[0], m[1], m[2], m[3], m[4], m[5], cfg.rate, cfg.all);
    return ESP_OK;
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
        status("pong rx=%lu rxdrop=%lu tx=%lu txerr=%lu", (unsigned long)n_rx, (unsigned long)n_rx_drop,
               (unsigned long)n_tx, (unsigned long)n_tx_err);
    }
}

/* ---------------------------------------------------------------- tasks */

static void usb_rx_task(void *arg)
{
    static uint8_t frame[2048];
    uint8_t buf[256];
    int state = 0, type = 0;
    uint16_t len = 0, got = 0;
    uint8_t x = 0;
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(20));
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
    for (;;) {
        if (xQueueReceive(rxq, &it, portMAX_DELAY) == pdTRUE) {
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
    usb_serial_jtag_driver_config_t ucfg = { .tx_buffer_size = 8192, .rx_buffer_size = 8192 };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_apply();

    xTaskCreate(usb_tx_task, "usb_tx", 4096, NULL, 10, NULL);
    xTaskCreate(usb_rx_task, "usb_rx", 4096, NULL, 11, NULL);
}
