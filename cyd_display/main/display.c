/*
 * Room display for the ESP32-2432S028 ("Cheap Yellow Display": classic ESP32, CH340 USB-UART, ILI9341 320x240).
 *
 * The Mac draws every screen (display/room_display.py --serial) and sends only the parts that changed; this firmware
 * puts them on the panel. Link: UART0 through the CH340 at 921600 baud, the same framing as the radio modem:
 *   AA 55 | type u8 | len u16 LE | payload | xor8(type, len bytes, payload)
 *   Mac -> CYD  'P' ping                       -> 'S' "cyd ready ..."
 *               'O' orientation: swap_xy u8, mirror_x u8, mirror_y u8, invert u8
 *               'B' blit: x u16, y u16, w u16, h u16 (LE), then runs of (count u8 1..255, RGB565 big-endian u16)
 *                   covering w*h pixels, w*h <= MAX_BLIT                                -> 'A' when drawn
 *               'L' RGB LED: r u8, g u8, b u8 (0 = off, 1 = on)
 *   CYD -> Mac  'S' status text, 'A' blit done, 'E' error text
 * A frame with a bad checksum is answered with 'E' "checksum"; a frame longer than MAX_FRAME is ignored.
 */
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_heap_caps.h"

#define BAUD 921600
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define BAND_ROWS 16
#define MAX_BLIT (LCD_WIDTH * BAND_ROWS) /* pixels per blit: one 16-row band of the full width */
#define MAX_FRAME (8 + MAX_BLIT * 3)     /* worst case RLE: one run per pixel */

/* ESP32-2432S028R pins */
#define PIN_SCLK 14
#define PIN_MOSI 13
#define PIN_MISO 12
#define PIN_CS 15
#define PIN_DC 2
#define PIN_BACKLIGHT 21
#define PIN_LED_RED 4
#define PIN_LED_GREEN 16
#define PIN_LED_BLUE 17

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t color_done;     /* given when the panel has taken a blit's pixels */
static uint16_t *blit_pixels;            /* DMA buffer for one blit */

static void send_frame(uint8_t type, const void *payload, size_t length)
{
    uint8_t header[5] = { 0xAA, 0x55, type, (uint8_t)(length & 0xFF), (uint8_t)(length >> 8) };
    uint8_t checksum = header[2] ^ header[3] ^ header[4];
    for (size_t i = 0; i < length; i++) checksum ^= ((const uint8_t *)payload)[i];
    uart_write_bytes(UART_NUM_0, header, 5);
    if (length) uart_write_bytes(UART_NUM_0, payload, length);
    uart_write_bytes(UART_NUM_0, &checksum, 1);
}

/* A text frame ('S' status or 'E' error), printf style, cut at 119 characters */
static void send_text(uint8_t type, const char *format, ...)
{
    char text[120];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    send_frame(type, text, length < 0 ? 0 : (length >= (int)sizeof text ? sizeof text - 1 : length));
}

static bool on_color_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *event_data,
                          void *user_ctx)
{
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(color_done, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

static void uart_init(void)
{
    uart_config_t uart_config = { .baud_rate = BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                                  .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                  .source_clk = UART_SCLK_DEFAULT };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 32768, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
}

static void lcd_init(void)
{
    gpio_config_t outputs = { .pin_bit_mask = (1ULL << PIN_BACKLIGHT) | (1ULL << PIN_LED_RED) |
                                              (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_BLUE),
                              .mode = GPIO_MODE_OUTPUT };
    gpio_config(&outputs);
    gpio_set_level(PIN_LED_RED, 1);      /* the RGB LED is active low: off */
    gpio_set_level(PIN_LED_GREEN, 1);
    gpio_set_level(PIN_LED_BLUE, 1);

    spi_bus_config_t bus_config = { .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO,
                                    .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = MAX_BLIT * 2 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_io_spi_config_t io_config = { .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS,
                                                .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8,
                                                .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 4,
                                                .on_color_trans_done = on_color_done };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io));
    esp_lcd_panel_dev_config_t panel_config = { .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
                                                .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_swap_xy(panel, true);  /* landscape; the Mac can change it with 'O' */
    esp_lcd_panel_mirror(panel, true, false);
    esp_lcd_panel_disp_on_off(panel, true);

    /* start dark grey instead of the panel's random contents */
    /* dark grey: RGB565 0x2104, high byte first in memory as the panel takes it */
    for (int i = 0; i < MAX_BLIT; i++) blit_pixels[i] = 0x0421;
    for (int y = 0; y < LCD_HEIGHT; y += BAND_ROWS) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_WIDTH, y + BAND_ROWS, blit_pixels);
        xSemaphoreTake(color_done, portMAX_DELAY);
    }
    gpio_set_level(PIN_BACKLIGHT, 1);
}

/* 'B': unpacks the runs into blit_pixels, draws them and answers 'A' (or 'E' when the blit is malformed) */
static void blit(const uint8_t *payload, size_t length)
{
    if (length < 8) { send_text('E', "blit: short"); return; }
    int x = payload[0] | payload[1] << 8, y = payload[2] | payload[3] << 8;
    int width = payload[4] | payload[5] << 8, height = payload[6] | payload[7] << 8;
    int pixel_count = width * height;
    if (width <= 0 || height <= 0 || pixel_count > MAX_BLIT || x + width > LCD_WIDTH || y + height > LCD_HEIGHT) {
        send_text('E', "blit: bad rect %d,%d %dx%d", x, y, width, height);
        return;
    }
    int filled = 0;
    for (size_t i = 8; i + 2 < length && filled < pixel_count; i += 3) {
        int run_length = payload[i];
        /* high byte first in memory, as the panel takes it */
        uint16_t colour = (uint16_t)(payload[i + 2] << 8 | payload[i + 1]);
        for (int repeat = 0; repeat < run_length && filled < pixel_count; repeat++) blit_pixels[filled++] = colour;
    }
    if (filled != pixel_count) { send_text('E', "blit: %d of %d pixels", filled, pixel_count); return; }
    esp_lcd_panel_draw_bitmap(panel, x, y, x + width, y + height, blit_pixels);
    xSemaphoreTake(color_done, portMAX_DELAY);
    send_frame('A', NULL, 0);
}

static void handle_frame(uint8_t type, const uint8_t *payload, size_t length)
{
    switch (type) {
    case 'P':
        send_text('S', "cyd ready %dx%d max_blit=%d", LCD_WIDTH, LCD_HEIGHT, MAX_BLIT);
        break;
    case 'O':
        if (length >= 4) {
            esp_lcd_panel_swap_xy(panel, payload[0]);
            esp_lcd_panel_mirror(panel, payload[1], payload[2]);
            esp_lcd_panel_invert_color(panel, payload[3]);
            send_text('S', "orientation swap=%d mirror=%d,%d invert=%d", payload[0], payload[1], payload[2],
                      payload[3]);
        }
        break;
    case 'B':
        blit(payload, length);
        break;
    case 'L':
        if (length >= 3) {               /* active low */
            gpio_set_level(PIN_LED_RED, !payload[0]);
            gpio_set_level(PIN_LED_GREEN, !payload[1]);
            gpio_set_level(PIN_LED_BLUE, !payload[2]);
        }
        break;
    default:
        send_text('E', "unknown type 0x%02x", type);
    }
}

/* Reads UART0 forever and hands every complete frame with a good checksum to handle_frame() */
static void read_frames(void)
{
    enum { WAIT_AA, WAIT_55, READ_TYPE, READ_LENGTH_LOW, READ_LENGTH_HIGH, READ_PAYLOAD, READ_CHECKSUM };
    static uint8_t frame[MAX_FRAME];
    uint8_t chunk[512];
    int state = WAIT_AA, type = 0;
    size_t length = 0, received = 0;
    uint8_t checksum = 0;
    for (;;) {
        int count = uart_read_bytes(UART_NUM_0, chunk, sizeof chunk, pdMS_TO_TICKS(20));
        for (int i = 0; i < count; i++) {
            uint8_t byte = chunk[i];
            switch (state) {
            case WAIT_AA: state = byte == 0xAA ? WAIT_55 : WAIT_AA; break;
            case WAIT_55: state = byte == 0x55 ? READ_TYPE : (byte == 0xAA ? WAIT_55 : WAIT_AA); break;
            case READ_TYPE: type = byte; checksum = byte; state = READ_LENGTH_LOW; break;
            case READ_LENGTH_LOW: length = byte; checksum ^= byte; state = READ_LENGTH_HIGH; break;
            case READ_LENGTH_HIGH:
                length |= byte << 8; checksum ^= byte; received = 0;
                state = length > sizeof frame ? WAIT_AA : (length ? READ_PAYLOAD : READ_CHECKSUM);
                break;
            case READ_PAYLOAD:
                frame[received++] = byte; checksum ^= byte;
                if (received == length) state = READ_CHECKSUM;
                break;
            case READ_CHECKSUM:
                if (byte == checksum) handle_frame(type, frame, length);
                else send_text('E', "checksum");
                state = WAIT_AA;
                break;
            }
        }
    }
}

void app_main(void)
{
    color_done = xSemaphoreCreateBinary();
    blit_pixels = heap_caps_malloc(MAX_BLIT * 2, MALLOC_CAP_DMA);
    uart_init();
    lcd_init();
    send_text('S', "cyd ready %dx%d max_blit=%d", LCD_WIDTH, LCD_HEIGHT, MAX_BLIT);
    read_frames();
}
