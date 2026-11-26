#pragma once
#include <cstdint>

class As608 {
public:
    enum : int { Ok = 0, NoFinger = 2, LinkError = -1, NoReply = -2 };
    void begin();
    bool probe();
    int image();
    int convert(uint8_t buffer);
    int createModel();
    int store(uint16_t slot);
    int search(uint16_t &slot);
    int clear();
private:
    int command(const uint8_t *payload, unsigned count, uint16_t *slot = nullptr, int64_t timeoutUs = 1100000);
};
