#include <atomic>
#include <cstring>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_providers.h>
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <crypto/CHIPCryptoPAL.h>
#include <platform/CommissionableDataProvider.h>
#include <setup_payload/SetupPayload.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>

bool setLocker(bool unlock);
bool lockerUnlocked();
void reportMatter(bool);

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters::DoorLock;

namespace {
constexpr const char *tag = "locker-matter";
EndpointId endpointId;
std::atomic<bool> ready{false};

class PairingData final : public DeviceLayer::CommissionableDataProvider {
    uint32_t passcode{};
    uint16_t discriminator{};
    uint8_t salt[16]{};
public:
    CHIP_ERROR generate() {
        do {
            ReturnErrorOnFailure(Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&passcode), sizeof(passcode)));
            passcode = passcode % kSetupPINCodeMaximumValue + 1;
        } while (!SetupPayload::IsValidSetupPIN(passcode));
        ReturnErrorOnFailure(Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&discriminator), sizeof(discriminator)));
        discriminator &= 0xfff;
        return Crypto::DRBG_get_bytes(salt, sizeof(salt));
    }

    void printCode() const {
        SetupPayload payload;
        payload.discriminator.SetLongValue(discriminator);
        payload.setUpPINCode = passcode;
        char code[22]{};
        MutableCharSpan output(code);
        if (ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(output) == CHIP_NO_ERROR)
            ESP_LOGI(tag, "Codigo de pareamento Matter: %s", code);
    }

    CHIP_ERROR GetSetupDiscriminator(uint16_t &value) override { value = discriminator; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSetupPasscode(uint32_t &value) override { value = passcode; return CHIP_NO_ERROR; }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t &value) override { value = 10000; return CHIP_NO_ERROR; }
    CHIP_ERROR GetSpake2pSalt(MutableByteSpan &value) override {
        return CopySpanToMutableSpan(ByteSpan(salt), value);
    }
    CHIP_ERROR GetSpake2pVerifier(MutableByteSpan &value, size_t &length) override {
        Crypto::Spake2pVerifier verifier;
        ReturnErrorOnFailure(verifier.Generate(10000, ByteSpan(salt), passcode));
        ReturnErrorOnFailure(verifier.Serialize(value));
        length = value.size();
        return CHIP_NO_ERROR;
    }
} pairing;

void synchronize(OperationSourceEnum source) {
    const auto state = lockerUnlocked() ? DlLockState::kUnlocked : DlLockState::kLocked;
    if (!DoorLockServer::Instance().SetLockState(endpointId, state, source)) {
        ESP_LOGW(tag, "Falha ao atualizar LockState");
    }
}

void publish(intptr_t) { synchronize(OperationSourceEnum::kUnspecified); }

void event(const DeviceLayer::ChipDeviceEvent *event, intptr_t) {
    if (event->Type == DeviceLayer::DeviceEventType::kCommissioningWindowOpened) pairing.printCode();
    if (event->Type == DeviceLayer::DeviceEventType::kCommissioningComplete)
        ESP_LOGI(tag, "Pareamento Matter concluido");
}
}

void startMatter() {
    if (pairing.generate() != CHIP_NO_ERROR) {
        ESP_LOGE(tag, "Falha ao gerar pareamento");
        return;
    }
    esp_matter::set_custom_commissionable_data_provider(&pairing);
    esp_matter::node::config_t root{};
    auto *node = esp_matter::node::create(&root, nullptr, nullptr);
    if (!node) return;
    esp_matter::endpoint::door_lock::config_t config{};
    config.door_lock.lock_state = static_cast<uint8_t>(DlLockState::kLocked);
    config.door_lock.actuator_enabled = true;
    auto *endpoint = esp_matter::endpoint::door_lock::create(node, &config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (!endpoint) return;
    endpointId = esp_matter::endpoint::get_id(endpoint);
    if (esp_matter::start(event) != ESP_OK) {
        ESP_LOGE(tag, "Falha ao iniciar Matter");
        return;
    }
    pairing.printCode();
    ready = true;
    reportMatter(false);
}

void reportMatter(bool) {
    if (ready && DeviceLayer::PlatformMgr().ScheduleWork(publish) != CHIP_NO_ERROR)
        ESP_LOGW(tag, "Falha ao agendar LockState");
}

bool emberAfPluginDoorLockOnDoorLockCommand(EndpointId endpoint, const DataModel::Nullable<FabricIndex> &,
                                           const DataModel::Nullable<NodeId> &, const Optional<ByteSpan> &,
                                           OperationErrorEnum &) {
    if (endpoint != endpointId || !setLocker(false)) return false;
    synchronize(OperationSourceEnum::kRemote);
    return true;
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(EndpointId endpoint, const DataModel::Nullable<FabricIndex> &,
                                             const DataModel::Nullable<NodeId> &, const Optional<ByteSpan> &,
                                             OperationErrorEnum &) {
    if (endpoint != endpointId || !setLocker(true)) return false;
    synchronize(OperationSourceEnum::kRemote);
    return true;
}

extern "C" void locker_door_lock_callbacks() {}
