#include "cyd.h"
#include <algorithm>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "font.h"

namespace {
constexpr auto dc = GPIO_NUM_2;
constexpr auto backlight = GPIO_NUM_21;
constexpr auto irq = GPIO_NUM_36;
DMA_ATTR uint16_t pixels[320 * 4];

uint16_t wire(uint16_t color) { return __builtin_bswap16(color); }
}

void Cyd::send(bool data, const void *bytes, unsigned length) {
    gpio_set_level(dc, data);
    spi_transaction_t transaction{};
    transaction.length = 8 * length;
    transaction.tx_buffer = bytes;
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd, &transaction));
}

void Cyd::command(uint8_t code, const uint8_t *data, unsigned length) {
    send(false, &code, 1);
    if (length) send(true, data, length);
}

void Cyd::begin() {
    ESP_ERROR_CHECK(gpio_set_direction(dc, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_direction(backlight, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_direction(irq, GPIO_MODE_INPUT));
    spi_bus_config_t tft{};
    tft.sclk_io_num = 14;
    tft.mosi_io_num = 13;
    tft.miso_io_num = -1;
    tft.quadwp_io_num = tft.quadhd_io_num = -1;
    tft.max_transfer_sz = sizeof(pixels);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &tft, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t device{};
    device.clock_speed_hz = 40000000;
    device.spics_io_num = 15;
    device.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &lcd));

    spi_bus_config_t xpt{};
    xpt.sclk_io_num = 25;
    xpt.mosi_io_num = 32;
    xpt.miso_io_num = 39;
    xpt.quadwp_io_num = xpt.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &xpt, SPI_DMA_DISABLED));
    device.clock_speed_hz = 1000000;
    device.spics_io_num = 33;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &device, &digitizer));

    command(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));
    const uint8_t format = 0x55;
    // Landscape with the panel row direction reversed to correct horizontal mirroring.
    const uint8_t rotation = 0xa8;
    command(0x3a, &format, 1);
    command(0x36, &rotation, 1);
    command(0x11);
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x29);
    gpio_set_level(backlight, 1);
}

int Cyd::sample(uint8_t code) {
    spi_transaction_t t{};
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 24;
    t.tx_data[0] = code;
    ESP_ERROR_CHECK(spi_device_polling_transmit(digitizer, &t));
    return ((t.rx_data[1] << 8) | t.rx_data[2]) >> 3 & 4095;
}

bool Cyd::touch(int &x, int &y) {
    if (gpio_get_level(irq)) return false;
    int xs[5], ys[5];
    for (int i = 0; i < 5; ++i) {
        xs[i] = sample(0x90);
        ys[i] = sample(0xd0);
    }
    std::sort(xs, xs + 5);
    std::sort(ys, ys + 5);
    x = xs[2];
    y = ys[2];
    return !gpio_get_level(irq) && x > 20 && x < 4075 && y > 20 && y < 4075;
}

void Cyd::window(int x, int y, int w, int h) {
    const int right = x + w - 1, bottom = y + h - 1;
    const uint8_t col[] = {uint8_t(x >> 8), uint8_t(x), uint8_t(right >> 8), uint8_t(right)};
    const uint8_t row[] = {uint8_t(y >> 8), uint8_t(y), uint8_t(bottom >> 8), uint8_t(bottom)};
    command(0x2a, col, sizeof(col));
    command(0x2b, row, sizeof(row));
    command(0x2c);
}

void Cyd::fill(int x, int y, int w, int h, uint16_t color) {
    window(x, y, w, h);
    std::fill_n(pixels, sizeof(pixels) / 2, wire(color));
    for (int remaining = w * h; remaining > 0;) {
        const int count = std::min(remaining, int(sizeof(pixels) / 2));
        send(true, pixels, count * 2);
        remaining -= count;
    }
}

void Cyd::text(int x, int y, const char *label, int scale, uint16_t color, uint16_t background) {
    const int w = 6 * scale, h = 8 * scale;
    for (; *label && x + w <= 320; ++label, x += w) {
        const auto &glyph = font[static_cast<unsigned char>(*label) - 32];
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) {
                pixels[row * w + col] = wire(glyph[row / scale] & (1 << (col / scale)) ? color : background);
            }
        }
        window(x, y, w, h);
        send(true, pixels, w * h * 2);
    }
}
