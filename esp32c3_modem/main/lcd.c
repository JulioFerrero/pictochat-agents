/*
 * Optional SPI screen on the modem board (e.g. Waveshare ESP32-S3-LCD-2: ST7789 240x320), driven by the PC.
 *
 * Nothing is touched until the PC sends 'D', so boards without a screen are unaffected. The PC draws everything
 * (display/room_display.py --modem-screen, or the agents' dashboard) and sends the bands that changed, through the
 * host (pictochat.py's "display" command), in the modem's USB framing (see modem.c, which calls lcd_handle()):
 *   'D' init: sclk, mosi, miso, cs, dc, rst, bl, bl_on (u8 each, 255 = not connected), width u16, height u16,
 *       swap_xy, mirror_x, mirror_y, invert, bgr, spi_mode, pclk_mhz (u8 each)
 *   'B' blit: x u16, y u16, w u16, h u16, then runs of (count u8 1..255, RGB565 as sent on the wire: high byte
 *       first), covering w*h pixels, w*h <= LCD_MAX_BLIT
 * Nothing is answered for a blit; errors and the init result go back as 'S' status lines.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"

#define LCD_MAX_BLIT (320 * 8)          /* pixels per blit: room_display.py's ModemScreen sends 320 x 8 bands */
#define NOT_CONNECTED 255

void modem_status(const char *format, ...); /* in modem.c: an 'S' status line to the PC */

static esp_lcd_panel_handle_t panel;    /* NULL until a 'D' succeeds */
static esp_lcd_panel_io_handle_t panel_io;
static SemaphoreHandle_t color_done;    /* given when the panel has taken a blit's pixels */
static uint16_t *blit_pixels;           /* DMA buffer for one blit */
static int lcd_width, lcd_height;

static bool on_color_done(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_io_event_data_t *event_data,
                          void *user_ctx)
{
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(color_done, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

/* Frees the panel, its IO and the SPI bus, so that a new 'D' can set them up with other settings */
static bool spi_bus_ready;

/* Frees whatever the last set-up got as far as, so that a failed or changed set-up can be tried again. */
static void lcd_release(void)
{
    if (panel) { esp_lcd_panel_del(panel); panel = NULL; }
    if (panel_io) { esp_lcd_panel_io_del(panel_io); panel_io = NULL; }
    if (spi_bus_ready) { spi_bus_free(SPI2_HOST); spi_bus_ready = false; }
}

/* 'D': sets the screen up (again) */
static void lcd_init(const uint8_t *payload, uint16_t length)
{
    if (length < 19) { modem_status("lcd: short init"); return; }
    lcd_release();
    int pin_sclk = payload[0], pin_mosi = payload[1], pin_miso = payload[2], pin_cs = payload[3];
    int pin_dc = payload[4], pin_reset = payload[5], pin_backlight = payload[6], backlight_on = payload[7];
    lcd_width = payload[8] | payload[9] << 8;
    lcd_height = payload[10] | payload[11] << 8;
    int swap_xy = payload[12], mirror_x = payload[13], mirror_y = payload[14], invert = payload[15];
    int bgr = payload[16], spi_mode = payload[17], pclk_mhz = payload[18];

    if (!color_done) color_done = xSemaphoreCreateBinary();
    if (!blit_pixels) blit_pixels = heap_caps_malloc(LCD_MAX_BLIT * 2, MALLOC_CAP_DMA);
    spi_bus_config_t bus_config = { .sclk_io_num = pin_sclk, .mosi_io_num = pin_mosi,
                                    .miso_io_num = pin_miso == NOT_CONNECTED ? -1 : pin_miso,
                                    .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_MAX_BLIT * 2 };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    spi_bus_ready = err == ESP_OK;
    if (err != ESP_OK || !blit_pixels || !color_done) {
        modem_status("lcd: spi init failed %d", err);
        lcd_release();
        return;
    }
    esp_lcd_panel_io_spi_config_t io_config = { .dc_gpio_num = pin_dc, .cs_gpio_num = pin_cs,
                                                .pclk_hz = pclk_mhz * 1000000, .lcd_cmd_bits = 8,
                                                .lcd_param_bits = 8, .spi_mode = spi_mode, .trans_queue_depth = 4,
                                                .on_color_trans_done = on_color_done };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io);
    if (err != ESP_OK) { modem_status("lcd: io failed %d", err); panel_io = NULL; lcd_release(); return; }
    esp_lcd_panel_dev_config_t panel_config = { .reset_gpio_num = pin_reset == NOT_CONNECTED ? -1 : pin_reset,
                                                .bits_per_pixel = 16,
                                                .rgb_ele_order = bgr ? LCD_RGB_ELEMENT_ORDER_BGR
                                                                     : LCD_RGB_ELEMENT_ORDER_RGB };
    err = esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel);
    if (err != ESP_OK) { modem_status("lcd: panel failed %d", err); panel = NULL; lcd_release(); return; }

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_swap_xy(panel, swap_xy);
    esp_lcd_panel_mirror(panel, mirror_x, mirror_y);
    esp_lcd_panel_invert_color(panel, invert);
    esp_lcd_panel_disp_on_off(panel, true);
    if (pin_backlight != NOT_CONNECTED) {
        gpio_config_t backlight_config = { .pin_bit_mask = 1ULL << pin_backlight, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&backlight_config);
        gpio_set_level(pin_backlight, backlight_on);
    }
    modem_status("lcd: on %dx%d spi mode %d at %d MHz, swap %d mirror %d,%d invert %d", lcd_width, lcd_height,
                 spi_mode, pclk_mhz, swap_xy, mirror_x, mirror_y, invert);
}

/* 'B': unpacks the runs into blit_pixels and draws them */
static void lcd_blit(const uint8_t *payload, uint16_t length)
{
    if (!panel) { modem_status("lcd: blit before init"); return; }
    if (length < 8) return;
    int x = payload[0] | payload[1] << 8, y = payload[2] | payload[3] << 8;
    int width = payload[4] | payload[5] << 8, height = payload[6] | payload[7] << 8;
    int pixel_count = width * height, filled = 0;
    if (width <= 0 || height <= 0 || pixel_count > LCD_MAX_BLIT || x + width > lcd_width || y + height > lcd_height) {
        modem_status("lcd: bad rect %d,%d %dx%d", x, y, width, height);
        return;
    }
    for (int i = 8; i + 2 < length && filled < pixel_count; i += 3) {
        /* the panel takes the high byte first; in little-endian memory that is the low byte of the uint16 */
        uint16_t colour = (uint16_t)(payload[i + 2] << 8 | payload[i + 1]);
        for (int repeat = 0; repeat < payload[i] && filled < pixel_count; repeat++) blit_pixels[filled++] = colour;
    }
    if (filled != pixel_count) { modem_status("lcd: %d of %d pixels", filled, pixel_count); return; }
    esp_lcd_panel_draw_bitmap(panel, x, y, x + width, y + height, blit_pixels);
    if (xSemaphoreTake(color_done, pdMS_TO_TICKS(200)) != pdTRUE) {
        modem_status("lcd: blit %d,%d took over 200 ms", x, y);   /* the next blit may overwrite this one */
    }
}

/* Called by modem.c for every frame from the PC: true when it was a screen frame ('D' or 'B') */
bool lcd_handle(uint8_t type, const uint8_t *payload, uint16_t length)
{
    if (type == 'D') { lcd_init(payload, length); return true; }
    if (type == 'B') { lcd_blit(payload, length); return true; }
    return false;
}
