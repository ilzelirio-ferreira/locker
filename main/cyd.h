#pragma once
#include <cstdint>
#include <driver/spi_master.h>

class Cyd {
public:
    void begin();
    bool touch(int &x, int &y);
    void fill(int x, int y, int w, int h, uint16_t color);
    void text(int x, int y, const char *label, int scale, uint16_t color, uint16_t background);
private:
    spi_device_handle_t lcd{};
    spi_device_handle_t digitizer{};
    void send(bool data, const void *bytes, unsigned length);
    void command(uint8_t code, const uint8_t *data = nullptr, unsigned length = 0);
    void window(int x, int y, int w, int h);
    int sample(uint8_t command);
};
