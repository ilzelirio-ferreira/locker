#include "as608.h"
#include <driver/uart.h>
#include <esp_timer.h>

namespace {
constexpr auto port = UART_NUM_2;

bool readBefore(uint8_t *bytes, unsigned length, int64_t deadline) {
    while (length) {
        const auto remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return false;
        const int count = uart_read_bytes(port, bytes, length, pdMS_TO_TICKS((remaining + 999) / 1000) + 1);
        if (count <= 0) return false;
        bytes += count;
        length -= count;
    }
    return true;
}
}

void As608::begin() {
    uart_config_t config{};
    config.baud_rate = 57600;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(port, &config));
    ESP_ERROR_CHECK(uart_set_pin(port, 22, 27, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(port, 256, 0, 0, nullptr, 0));
}

int As608::command(const uint8_t *payload, unsigned count, uint16_t *slot) {
    uint8_t packet[18] = {0xef, 1, 0xff, 0xff, 0xff, 0xff, 1, 0, static_cast<uint8_t>(count + 2)};
    uint16_t sum = packet[6] + packet[8];
    for (unsigned i = 0; i < count; ++i) {
        packet[9 + i] = payload[i];
        sum += payload[i];
    }
    packet[9 + count] = sum >> 8;
    packet[10 + count] = sum;
    uart_flush_input(port);
    if (uart_write_bytes(port, packet, count + 11) != static_cast<int>(count + 11)) return LinkError;

    const int64_t deadline = esp_timer_get_time() + 350000;
    uint8_t header[9];
    if (!readBefore(header, sizeof(header), deadline) ||
        header[0] != 0xef || header[1] != 1 || header[2] != 0xff || header[3] != 0xff ||
        header[4] != 0xff || header[5] != 0xff || header[6] != 7 || header[7] != 0) return LinkError;
    uint8_t body[16];
    const unsigned length = header[8];
    if (length < 3 || length > sizeof(body) || !readBefore(body, length, deadline)) return LinkError;
    sum = header[6] + header[8];
    for (unsigned i = 0; i < length - 2; ++i) sum += body[i];
    if (sum != ((body[length - 2] << 8) | body[length - 1])) return LinkError;
    if (body[0] == Ok && slot) {
        if (length != 7) return LinkError;
        *slot = (body[1] << 8) | body[2];
    }
    return body[0];
}

int As608::image() { const uint8_t p[] = {1}; return command(p, sizeof(p)); }
int As608::convert(uint8_t buffer) { const uint8_t p[] = {2, buffer}; return command(p, sizeof(p)); }
int As608::createModel() { const uint8_t p[] = {5}; return command(p, sizeof(p)); }
int As608::store(uint16_t slot) {
    const uint8_t p[] = {6, 1, static_cast<uint8_t>(slot >> 8), static_cast<uint8_t>(slot)};
    return command(p, sizeof(p));
}
int As608::search(uint16_t &slot) {
    const uint8_t p[] = {4, 1, 0, 1, 0, 10};
    return command(p, sizeof(p), &slot);
}
int As608::clear() { const uint8_t p[] = {13}; return command(p, sizeof(p)); }
