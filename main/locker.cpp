#include "as608.h"
#include "cyd.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/pkcs5.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifdef LOCKER_MATTER
void startMatter();
void reportMatter(bool unlocked);
#endif

namespace {
constexpr auto relay = GPIO_NUM_26;
constexpr uint16_t bg = 0x0863, key = 0x21a9, white = 0xffff, accent = 0x47fa;
constexpr uint16_t slots = 10;
Cyd screen;
As608 fingerprint;
nvs_handle_t storage;
SemaphoreHandle_t relayMutex;
int64_t unlockUntil;
std::atomic<bool> configured{false};

struct PinRecord { uint8_t salt[16]; uint8_t hash[32]; };
PinRecord pin{};
uint16_t enrolled;
char input[13]{};
char candidate[13]{};
unsigned digits, failures;
int64_t blockedUntil, adminUntil, enrollmentUntil;
bool held, waitFingerRemoval;
enum class View { First, Confirm, Access, AdminPin, Settings, NewPin, NewConfirm, FirstFinger, RemoveFinger, RepeatFinger };
View view;
const char *message = "";
uint16_t newSlot;

struct Calibration { float ax, bx, cx, ay, by, cy; };
Calibration calibration{};

bool load(const char *name, void *data, size_t size) {
    const size_t expected = size;
    return nvs_get_blob(storage, name, data, &size) == ESP_OK && size == expected;
}

bool save(const char *name, const void *data, size_t size) {
    return nvs_set_blob(storage, name, data, size) == ESP_OK && nvs_commit(storage) == ESP_OK;
}

bool hashPin(const char *value, const PinRecord &record, uint8_t *hash) {
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const uint8_t *>(value),
        strlen(value), record.salt, sizeof(record.salt), 20000, sizeof(record.hash), hash) == 0;
}

bool savePin() {
    PinRecord next{};
    esp_fill_random(next.salt, sizeof(next.salt));
    if (!hashPin(candidate, next, next.hash) || !save("pin", &next, sizeof(next))) return false;
    pin = next;
    configured = true;
    return true;
}

bool checkPin() {
    uint8_t hash[sizeof(pin.hash)];
    if (!hashPin(input, pin, hash)) return false;
    unsigned difference = 0;
    for (unsigned i = 0; i < sizeof(hash); ++i) difference |= hash[i] ^ pin.hash[i];
    return difference == 0;
}

void drawButton(int x, int y, int w, int h, const char *title) {
    screen.fill(x, y, w, h, key);
    screen.text(x + (w - strlen(title) * 12) / 2, y + (h - 16) / 2, title, 2, white, key);
}

void draw() {
    screen.fill(0, 0, 320, 240, bg);
    const char *title = "FECHADURA";
    switch (view) {
        case View::First: title = "CRIE O PIN"; break;
        case View::Confirm: case View::NewConfirm: title = "CONFIRME O PIN"; break;
        case View::AdminPin: title = "PIN ADMIN"; break;
        case View::Settings: title = "CONFIGURACAO"; break;
        case View::NewPin: title = "NOVO PIN"; break;
        case View::FirstFinger: title = "ENCOSTE O DEDO"; break;
        case View::RemoveFinger: title = "RETIRE O DEDO"; break;
        case View::RepeatFinger: title = "REPITA O DEDO"; break;
        default: break;
    }
    screen.text(8, 7, title, 2, white, bg);
    screen.text(8, 29, message, 2, accent, bg);
    if (view == View::Settings) {
        drawButton(10, 57, 300, 36, "ALTERAR PIN");
        drawButton(10, 101, 300, 36, "CADASTRAR DIGITAL");
        drawButton(10, 145, 300, 36, "REMOVER TODAS DIGITAIS");
        drawButton(10, 189, 300, 36, "VOLTAR");
    } else if (view == View::FirstFinger || view == View::RemoveFinger || view == View::RepeatFinger) {
        drawButton(10, 189, 300, 36, "CANCELAR");
    } else {
        constexpr const char *keys[] = {"1","2","3","4","5","6","7","8","9","<","0","OK"};
        for (int i = 0; i < 12; ++i) drawButton(10 + i % 3 * 103, 76 + i / 3 * 40, 94, 35, keys[i]);
        if (view == View::Access) drawButton(210, 46, 100, 25, "AJUSTES");
        else if (view == View::AdminPin || view == View::NewPin || view == View::NewConfirm)
            drawButton(210, 46, 100, 25, "VOLTAR");
    }
}

void show(View next, const char *status = "") {
    view = next;
    message = status;
    digits = 0;
    memset(input, 0, sizeof(input));
    draw();
}

void mask() {
    screen.fill(8, 50, 194, 20, bg);
    char stars[sizeof(input)]{};
    memset(stars, '*', digits);
    screen.text(8, 52, stars, 2, white, bg);
}

void cross(int x, int y) {
    screen.fill(x - 8, y - 1, 17, 3, accent);
    screen.fill(x - 1, y - 8, 3, 17, accent);
}

void calibrate() {
    constexpr int targetsX[] = {20, 300, 20, 160};
    constexpr int targetsY[] = {20, 20, 220, 120};
    for (;;) {
        int x[4], y[4];
        for (int i = 0; i < 4; ++i) {
            screen.fill(0, 0, 320, 240, bg);
            screen.text(64, 63, "TOQUE NA CRUZ", 2, white, bg);
            cross(targetsX[i], targetsY[i]);
            while (!screen.touch(x[i], y[i])) vTaskDelay(pdMS_TO_TICKS(20));
            int idle = 0;
            while (idle < 5) {
                int a, b;
                idle = screen.touch(a, b) ? 0 : idle + 1;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        const float dx1 = x[1] - x[0], dy1 = y[1] - y[0];
        const float dx2 = x[2] - x[0], dy2 = y[2] - y[0];
        const float det = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(det) < 100000) continue;
        Calibration result{280 * dy2 / det, -280 * dx2 / det, 0, -200 * dy1 / det, 200 * dx1 / det, 0};
        result.cx = 20 - result.ax * x[0] - result.bx * y[0];
        result.cy = 20 - result.ay * x[0] - result.by * y[0];
        if (std::fabs(result.ax*x[3] + result.bx*y[3] + result.cx - 160) > 15 ||
            std::fabs(result.ay*x[3] + result.by*y[3] + result.cy - 120) > 15) continue;
        calibration = result;
        save("touch", &calibration, sizeof(calibration));
        return;
    }
}

void relock(void *) {
    bool changed = false;
    xSemaphoreTake(relayMutex, portMAX_DELAY);
    if (unlockUntil && esp_timer_get_time() >= unlockUntil) {
        gpio_set_level(relay, 0);
        unlockUntil = 0;
        changed = true;
    }
    xSemaphoreGive(relayMutex);
#ifdef LOCKER_MATTER
    if (changed) reportMatter(false);
#else
    (void)changed;
#endif
}
}

bool setLocker(bool unlock) {
    if (!configured) return false;
    xSemaphoreTake(relayMutex, portMAX_DELAY);
    const bool ok = gpio_set_level(relay, unlock) == ESP_OK;
    if (ok) unlockUntil = unlock ? esp_timer_get_time() + 5000000 : 0;
    xSemaphoreGive(relayMutex);
    return ok;
}

bool lockerUnlocked() {
    xSemaphoreTake(relayMutex, portMAX_DELAY);
    const bool unlocked = unlockUntil != 0;
    xSemaphoreGive(relayMutex);
    return unlocked;
}

namespace {
void unlockLocal() {
    const bool opened = setLocker(true);
#ifdef LOCKER_MATTER
    if (opened) reportMatter(true);
#endif
    show(View::Access, opened ? "LIBERADO POR 5 S" : "FALHA NA SAIDA");
}

void submit() {
    if (view == View::Access || view == View::AdminPin) {
        if (esp_timer_get_time() < blockedUntil) {
            show(view, "AGUARDE 30 S");
            return;
        }
        if (digits < 6 || !checkPin()) {
            if (++failures >= 5) {
                blockedUntil = esp_timer_get_time() + 30000000;
                failures = 0;
            }
            show(view, blockedUntil > esp_timer_get_time() ? "AGUARDE 30 S" : "PIN INCORRETO");
            return;
        }
        failures = 0;
        if (view == View::Access) unlockLocal();
        else {
            adminUntil = esp_timer_get_time() + 60000000;
            show(View::Settings);
        }
        return;
    }
    if (digits < 6) {
        show(view, "MINIMO DE 6 DIGITOS");
        return;
    }
    if (view == View::First || view == View::NewPin) {
        memcpy(candidate, input, sizeof(input));
        show(view == View::First ? View::Confirm : View::NewConfirm);
        return;
    }
    const bool first = view == View::Confirm;
    const bool equal = strcmp(candidate, input) == 0;
    const bool saved = equal && savePin();
    memset(candidate, 0, sizeof(candidate));
    show(saved ? (first ? View::Access : View::Settings) : (first ? View::First : View::NewPin),
         saved ? "PIN SALVO" : equal ? "FALHA AO SALVAR" : "PINS DIFERENTES");
}

bool enrollment() {
    return view == View::FirstFinger || view == View::RemoveFinger || view == View::RepeatFinger;
}

void handleTouch(int x, int y) {
    if (view == View::Settings) {
        if (x < 10 || x >= 310) return;
        if (y >= 57 && y < 93) show(View::NewPin);
        if (y >= 101 && y < 137) {
            newSlot = 1;
            while (newSlot <= slots && (enrolled & (1u << (newSlot - 1)))) ++newSlot;
            if (newSlot > slots) show(View::Settings, "MEMORIA CHEIA");
            else {
                enrollmentUntil = esp_timer_get_time() + 30000000;
                show(View::FirstFinger);
            }
        }
        if (y >= 145 && y < 181) {
            uint16_t empty = 0;
            if (!save("fingers", &empty, sizeof(empty))) show(View::Settings, "FALHA AO SALVAR");
            else {
                enrolled = empty;
                show(View::Settings, fingerprint.clear() == As608::Ok ? "DIGITAIS REMOVIDAS" : "DIGITAIS DESATIVADAS");
            }
        }
        if (y >= 189 && y < 225) show(View::Access);
        return;
    }
    if (enrollment()) {
        if (x >= 10 && x < 310 && y >= 189 && y < 225) show(View::Settings);
        return;
    }
    if (x >= 210 && x < 310 && y >= 46 && y < 71) {
        memset(candidate, 0, sizeof(candidate));
        if (view == View::Access) show(View::AdminPin);
        else if (view == View::AdminPin) show(View::Access);
        else if (view == View::NewPin || view == View::NewConfirm) show(View::Settings);
        return;
    }
    if (x < 10 || y < 76) return;
    const int col = (x - 10) / 103, row = (y - 76) / 40;
    if (col > 2 || row > 3 || (x - 10) % 103 >= 94 || (y - 76) % 40 >= 35) return;
    const int index = row * 3 + col;
    if (index == 11) submit();
    else if (index == 9) {
        if (digits) input[--digits] = 0;
        mask();
    } else if (digits < sizeof(input) - 1) {
        input[digits++] = index == 10 ? '0' : '1' + index;
        mask();
    }
}

void scan() {
    if (!enrollment() && (view != View::Access || !enrolled)) return;
    const int status = fingerprint.image();
    if (status == As608::NoFinger) {
        waitFingerRemoval = false;
        if (view == View::RemoveFinger) show(View::RepeatFinger);
        return;
    }
    if (status == As608::LinkError && enrollment()) {
        show(View::Settings, "LEITOR SEM RESPOSTA");
        return;
    }
    if (status != As608::Ok || waitFingerRemoval) return;
    waitFingerRemoval = true;
    if (view == View::FirstFinger) {
        if (fingerprint.convert(1) == As608::Ok) show(View::RemoveFinger);
        else show(View::FirstFinger, "TENTE NOVAMENTE");
    } else if (view == View::RepeatFinger) {
        if (fingerprint.convert(2) != As608::Ok || fingerprint.createModel() != As608::Ok ||
            fingerprint.store(newSlot) != As608::Ok) {
            show(View::Settings, "FALHA NO CADASTRO");
            return;
        }
        uint16_t next = enrolled | (1u << (newSlot - 1));
        if (save("fingers", &next, sizeof(next))) {
            enrolled = next;
            show(View::Settings, "DIGITAL CADASTRADA");
        } else show(View::Settings, "FALHA AO SALVAR");
    } else if (view == View::Access) {
        uint16_t slot{};
        if (fingerprint.convert(1) == As608::Ok && fingerprint.search(slot) == As608::Ok &&
            slot >= 1 && slot <= slots && (enrolled & (1u << (slot - 1)))) unlockLocal();
        else show(View::Access, "DIGITAL NAO ACEITA");
    }
}
}

extern "C" void app_main() {
    ESP_ERROR_CHECK(gpio_set_level(relay, 0));
    ESP_ERROR_CHECK(gpio_set_direction(relay, GPIO_MODE_OUTPUT));
    relayMutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(relayMutex ? ESP_OK : ESP_ERR_NO_MEM);
    esp_timer_create_args_t timerConfig{};
    timerConfig.callback = relock;
    timerConfig.name = "relock";
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timerConfig, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 50000));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("locker", NVS_READWRITE, &storage));
    configured = load("pin", &pin, sizeof(pin));
    if (configured) load("fingers", &enrolled, sizeof(enrolled));
    fingerprint.begin();
    screen.begin();
    if (!load("touch", &calibration, sizeof(calibration))) calibrate();
    show(configured ? View::Access : View::First);

    int64_t nextScan = 0;
    unsigned released = 0;
    bool wasUnlocked = lockerUnlocked();
#ifdef LOCKER_MATTER
    bool triedMatter = false;
#endif
    for (;;) {
#ifdef LOCKER_MATTER
        if (configured && !triedMatter) {
            triedMatter = true;
            startMatter();
        }
#endif
        const auto now = esp_timer_get_time();
        if ((view == View::Settings || view == View::NewPin || view == View::NewConfirm || enrollment()) &&
            now >= adminUntil) {
            memset(candidate, 0, sizeof(candidate));
            show(View::Access);
        } else if (enrollment() && now >= enrollmentUntil) show(View::Settings, "TEMPO ESGOTADO");
        const bool unlocked = lockerUnlocked();
        if (unlocked != wasUnlocked && view == View::Access) show(View::Access, unlocked ? "LIBERADO" : "TRAVADO");
        wasUnlocked = unlocked;
        int rawX, rawY;
        if (screen.touch(rawX, rawY)) {
            released = 0;
            if (!held) {
                held = true;
                handleTouch(int(calibration.ax*rawX + calibration.bx*rawY + calibration.cx),
                            int(calibration.ay*rawX + calibration.by*rawY + calibration.cy));
            }
        } else if (held && ++released >= 3) held = false;
        if (now >= nextScan) {
            scan();
            nextScan = esp_timer_get_time() + 160000;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
