// XIAO ESP32-C3 remote firmware: a thin wiring layer around the natively
// tested remote_core (spec section 11). All scoring/pairing/retry logic
// lives in components/remote_core; this file provides:
//
//   - ESP-NOW transport (broadcast until the court is learned, then unicast)
//   - GPIO button (raw level; remote_core debounces)
//   - LED feedback patterns (centralized table lives in remote_core)
//   - NVS persistence for RemoteSettings + court MAC (spec 11.5)
//
// Power behavior follows the spec 11.4 ordering: always-awake first; the
// sleep hook below is a stub until packet/ACK reliability is proven on
// hardware (M6).

#include <cinttypes>
#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "padel/protocol/packets.hpp"
#include "padel/remote/remote_core.hpp"

namespace {

const char* TAG = "remote";

constexpr uint8_t kBroadcastMac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr gpio_num_t kButtonGpio = static_cast<gpio_num_t>(CONFIG_PADEL_REMOTE_BUTTON_GPIO);
constexpr gpio_num_t kLedGpio = static_cast<gpio_num_t>(CONFIG_PADEL_REMOTE_LED_GPIO);

struct RxFrame {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    int len;
    uint8_t data[48];
};

QueueHandle_t s_rx_queue = nullptr;

void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len <= 0 || len > static_cast<int>(sizeof(RxFrame::data))) {
        return;
    }
    RxFrame frame{};
    memcpy(frame.mac, info->src_addr, ESP_NOW_ETH_ALEN);
    frame.len = len;
    memcpy(frame.data, data, static_cast<size_t>(len));
    xQueueSend(s_rx_queue, &frame, 0);  // drop on overflow; retries cover it
}

void ensure_peer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) {
        return;
    }
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    peer.channel = CONFIG_PADEL_REMOTE_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;  // encrypted peers land with key provisioning (M5)
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

// --- Platform adapters ---------------------------------------------------------

class EspClock : public padel::remote::IClock {
public:
    std::uint64_t now_ms() const override {
        return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    }
};

// NVS persistence: RemoteSettings plus the learned court MAC.
class NvsStore : public padel::remote::ISettingsStore {
public:
    struct Blob {
        padel::remote::RemoteSettings settings{};
        uint8_t court_mac[ESP_NOW_ETH_ALEN] = {};
        bool court_mac_valid = false;
    };

    bool open() { return nvs_open("padel_rmt", NVS_READWRITE, &handle_) == ESP_OK; }

    std::optional<padel::remote::RemoteSettings> load() override {
        Blob blob{};
        size_t size = sizeof(blob);
        if (nvs_get_blob(handle_, "cfg", &blob, &size) != ESP_OK || size != sizeof(blob)) {
            return std::nullopt;
        }
        cached_ = blob;
        return blob.settings;
    }

    bool save(const padel::remote::RemoteSettings& settings) override {
        cached_.settings = settings;
        return persist();
    }

    void save_court_mac(const uint8_t* mac) {
        memcpy(cached_.court_mac, mac, ESP_NOW_ETH_ALEN);
        cached_.court_mac_valid = true;
        persist();
    }

    const Blob& cached() const { return cached_; }

private:
    bool persist() {
        const esp_err_t err = nvs_set_blob(handle_, "cfg", &cached_, sizeof(cached_));
        return err == ESP_OK && nvs_commit(handle_) == ESP_OK;
    }

    nvs_handle_t handle_ = 0;
    Blob cached_{};
};

class EspNowRadio : public padel::remote::IRadio {
public:
    explicit EspNowRadio(NvsStore& store) : store_(store) {}

    void send_intent(const padel::protocol::PointIntentPacket& packet) override {
        const auto bytes = padel::protocol::serialize(packet);
        esp_now_send(target(), bytes.data(), bytes.size());
    }

    void send_pair_request(const padel::protocol::PairRequestPacket& packet) override {
        const auto bytes = padel::protocol::serialize(packet);
        esp_now_send(kBroadcastMac, bytes.data(), bytes.size());
    }

    void learn_court(const uint8_t* mac) {
        if (store_.cached().court_mac_valid &&
            memcmp(store_.cached().court_mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            return;
        }
        ensure_peer(mac);
        store_.save_court_mac(mac);
        ESP_LOGI(TAG, "learned court MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

private:
    const uint8_t* target() const {
        return store_.cached().court_mac_valid ? store_.cached().court_mac : kBroadcastMac;
    }

    NvsStore& store_;
};

// LED feedback. Blocking blink patterns are fine here: presses are minutes
// apart and the core is idle while feedback plays. Haptics hook in behind
// the same interface later (spec 11.4 ordering).
class LedFeedback : public padel::remote::IFeedback {
public:
    void init() {
        gpio_config_t config{};
        config.pin_bit_mask = 1ULL << kLedGpio;
        config.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&config));
        gpio_set_level(kLedGpio, 0);
    }

    void play(padel::remote::FeedbackPattern pattern) override {
        using P = padel::remote::FeedbackPattern;
        switch (pattern) {
            case P::PressRegistered:
                blink(1, 20, 0);
                break;
            case P::Accepted:
                blink(1, 120, 0);
                break;
            case P::RejectedConflict:
                blink(2, 80, 80);
                break;
            case P::RejectedOther:
                blink(2, 180, 120);
                break;
            case P::CommFailed:
                blink(3, 250, 120);
                break;
            case P::PairingRequired:
                blink(4, 60, 60);
                break;
            case P::PairingSuccess:
                blink(3, 100, 80);
                break;
        }
    }

private:
    void blink(int times, int on_ms, int off_ms) {
        for (int i = 0; i < times; ++i) {
            gpio_set_level(kLedGpio, 1);
            vTaskDelay(pdMS_TO_TICKS(on_ms));
            gpio_set_level(kLedGpio, 0);
            if (off_ms > 0 && i + 1 < times) {
                vTaskDelay(pdMS_TO_TICKS(off_ms));
            }
        }
    }
};

// --- Init -----------------------------------------------------------------------

void init_radio() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(
        esp_wifi_set_channel(CONFIG_PADEL_REMOTE_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    s_rx_queue = xQueueCreate(16, sizeof(RxFrame));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ensure_peer(kBroadcastMac);
}

void init_button() {
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << kButtonGpio;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
}

// Sleep hook, deliberately inert until M6 (spec 11.4: never optimize sleep
// before packet/ACK reliability is proven on hardware).
void power_inactivity_hook(std::uint64_t /*idle_ms*/) {}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    init_radio();
    init_button();

    static EspClock clock;
    static NvsStore store;
    if (!store.open()) {
        ESP_LOGE(TAG, "NVS open failed");
    }
    static EspNowRadio radio(store);
    static LedFeedback feedback;
    feedback.init();

    // Logical remote id from the station MAC (stable across reboots).
    uint8_t mac[ESP_NOW_ETH_ALEN];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    const uint32_t device_id = (static_cast<uint32_t>(mac[2]) << 24) |
                               (static_cast<uint32_t>(mac[3]) << 16) |
                               (static_cast<uint32_t>(mac[4]) << 8) | mac[5];

    static padel::remote::RemoteCore core(padel::remote::RemoteCoreConfig{}, clock, radio,
                                          feedback, store);
    core.begin(esp_random(), device_id);

    // Re-add the persisted court peer, if any.
    if (store.cached().court_mac_valid) {
        ensure_peer(store.cached().court_mac);
    }

    ESP_LOGI(TAG,
             "remote up: id=0x%08" PRIX32 " paired=%d team=%c court=%u channel=%d",
             core.settings().remote_id, core.settings().paired ? 1 : 0,
             core.settings().team == padel::TeamId::A ? 'A' : 'B',
             static_cast<unsigned>(core.settings().court_id),
             CONFIG_PADEL_REMOTE_WIFI_CHANNEL);

    std::uint64_t last_activity_ms = clock.now_ms();
    RxFrame frame;
    while (true) {
        // Raw button level; remote_core debounces (active low).
        core.set_button_level(gpio_get_level(kButtonGpio) == 0);

        // Drain the radio queue (callback-enqueue pattern, spec 23.3).
        while (xQueueReceive(s_rx_queue, &frame, 0) == pdTRUE) {
            const auto type = padel::protocol::peek_message_type(
                frame.data, static_cast<size_t>(frame.len));
            if (!type) {
                continue;
            }
            if (type.value() == padel::protocol::MessageType::Ack) {
                const auto ack = padel::protocol::parse_ack(frame.data,
                                                            static_cast<size_t>(frame.len));
                if (ack) {
                    radio.learn_court(frame.mac);
                    core.on_ack(ack.value());
                    last_activity_ms = clock.now_ms();
                }
            } else if (type.value() == padel::protocol::MessageType::PairAssign) {
                const auto assign = padel::protocol::parse_pair_assign(
                    frame.data, static_cast<size_t>(frame.len));
                if (assign) {
                    radio.learn_court(frame.mac);
                    core.on_pair_assign(assign.value());
                    last_activity_ms = clock.now_ms();
                }
            }
        }

        core.poll();
        if (core.state() != padel::remote::RemoteState::Ready) {
            last_activity_ms = clock.now_ms();
        }
        power_inactivity_hook(clock.now_ms() - last_activity_ms);

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
