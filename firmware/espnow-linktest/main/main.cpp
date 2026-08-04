// ESP-NOW link proof for the two ESP32-S3 DevKitC-1 boards (spec M4
// rehearsal). One binary, role chosen via menuconfig:
//
//   Sender   - plays the remote: builds PointIntentPacket, follows the retry
//              policy from docs/RADIO_PROTOCOL.md (450 ms ACK timeout, 5
//              attempts, backoff 0/80/180/350/650 ms), logs round-trip
//              latency. Runs a scripted burst first, then the BOOT button
//              sends real debounced presses.
//   Receiver - plays the court: parses, deduplicates, replies with an ACK
//              (optionally dropping a configured percentage to force
//              retries), and counts points applied exactly once.
//
// Discovery is zero-config: the sender broadcasts until the first ACK
// arrives, then locks onto that MAC as a unicast peer. No encryption; this
// is a bench link test only (production pairing is spec section 10.8).
//
// Radio callbacks only enqueue into a FreeRTOS queue; all protocol work
// happens in the main task (the callback-enqueue pattern, spec 23.3).

#include <cinttypes>
#include <cstring>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "padel/protocol/dedup.hpp"
#include "padel/protocol/packets.hpp"

namespace {

const char* TAG = "linktest";

constexpr uint8_t kBroadcastMac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr gpio_num_t kBootButton = GPIO_NUM_0;

constexpr uint32_t kAckTimeoutMs = 450;
constexpr int kMaxAttempts = 5;
constexpr uint32_t kBackoffMs[kMaxAttempts] = {0, 80, 180, 350, 650};

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
    peer.channel = CONFIG_LINKTEST_WIFI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

void init_radio() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_LINKTEST_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    s_rx_queue = xQueueCreate(16, sizeof(RxFrame));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    uint8_t mac[ESP_NOW_ETH_ALEN];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    ESP_LOGI(TAG, "own MAC %02X:%02X:%02X:%02X:%02X:%02X, channel %d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], CONFIG_LINKTEST_WIFI_CHANNEL);
}

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

struct SenderStats {
    uint32_t presses = 0;
    uint32_t accepted = 0;
    uint32_t duplicate_accepted = 0;
    uint32_t failed = 0;
    uint32_t total_attempts = 0;
    int64_t latency_min_us = INT64_MAX;
    int64_t latency_max_us = 0;
    int64_t latency_sum_us = 0;
    uint32_t latency_samples = 0;

    void record_latency(int64_t us) {
        if (us < latency_min_us) latency_min_us = us;
        if (us > latency_max_us) latency_max_us = us;
        latency_sum_us += us;
        ++latency_samples;
    }

    void log_summary(const char* label) const {
        ESP_LOGI(TAG,
                 "[%s] presses=%" PRIu32 " accepted=%" PRIu32 " dup_accepted=%" PRIu32
                 " failed=%" PRIu32 " attempts=%" PRIu32,
                 label, presses, accepted, duplicate_accepted, failed, total_attempts);
        if (latency_samples > 0) {
            ESP_LOGI(TAG, "[%s] press->ACK latency ms: min=%.1f avg=%.1f max=%.1f (n=%" PRIu32 ")",
                     label, latency_min_us / 1000.0,
                     latency_sum_us / 1000.0 / latency_samples,
                     latency_max_us / 1000.0, latency_samples);
        }
    }
};

struct Sender {
    uint32_t boot_id = esp_random();
    uint32_t sequence = 0;
    bool have_peer = false;
    uint8_t peer_mac[ESP_NOW_ETH_ALEN] = {};
    SenderStats stats{};

    // One press: same intent identity across all retries.
    void press() {
        ++stats.presses;
        padel::protocol::PointIntentPacket packet{};
        packet.court_id = CONFIG_LINKTEST_COURT_ID;
        packet.identity = padel::protocol::IntentIdentity{
            CONFIG_LINKTEST_REMOTE_ID, boot_id, ++sequence};
        packet.team = static_cast<padel::TeamId>(CONFIG_LINKTEST_TEAM);

        const int64_t t0 = esp_timer_get_time();
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (kBackoffMs[attempt] > 0) {
                vTaskDelay(pdMS_TO_TICKS(kBackoffMs[attempt]));
            }
            ++stats.total_attempts;
            packet.monotonic_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            const auto bytes = padel::protocol::serialize(packet);
            esp_now_send(have_peer ? peer_mac : kBroadcastMac, bytes.data(), bytes.size());

            if (wait_for_ack(packet.identity, t0)) {
                return;
            }
        }
        ++stats.failed;
        ESP_LOGW(TAG, "press seq=%" PRIu32 " FAILED after %d attempts", sequence, kMaxAttempts);
    }

    bool wait_for_ack(const padel::protocol::IntentIdentity& identity, int64_t t0) {
        const int64_t deadline = esp_timer_get_time() + kAckTimeoutMs * 1000;
        RxFrame frame;
        int64_t remaining_us;
        while ((remaining_us = deadline - esp_timer_get_time()) > 0) {
            if (xQueueReceive(s_rx_queue, &frame,
                              pdMS_TO_TICKS(remaining_us / 1000 + 1)) != pdTRUE) {
                continue;
            }
            const auto parsed = padel::protocol::parse_ack(frame.data,
                                                           static_cast<size_t>(frame.len));
            if (!parsed || !(parsed.value().identity == identity)) {
                continue;  // stale ACK for an earlier press, or noise
            }
            if (!have_peer) {
                memcpy(peer_mac, frame.mac, ESP_NOW_ETH_ALEN);
                ensure_peer(peer_mac);
                have_peer = true;
                ESP_LOGI(TAG, "locked onto receiver %02X:%02X:%02X:%02X:%02X:%02X",
                         peer_mac[0], peer_mac[1], peer_mac[2],
                         peer_mac[3], peer_mac[4], peer_mac[5]);
            }
            stats.record_latency(esp_timer_get_time() - t0);
            switch (parsed.value().status) {
                case padel::protocol::AckStatus::Accepted:
                    ++stats.accepted;
                    break;
                case padel::protocol::AckStatus::DuplicateAccepted:
                    ++stats.duplicate_accepted;
                    break;
                default:
                    ESP_LOGW(TAG, "terminal rejection status=%d",
                             static_cast<int>(parsed.value().status));
                    break;
            }
            return true;
        }
        return false;
    }
};

[[noreturn]] void run_sender() {
    ensure_peer(kBroadcastMac);
    Sender sender;

    ESP_LOGI(TAG, "sender: scripted burst of %d presses starting in 3 s...",
             CONFIG_LINKTEST_BURST_COUNT);
    vTaskDelay(pdMS_TO_TICKS(3000));

    for (int i = 0; i < CONFIG_LINKTEST_BURST_COUNT; ++i) {
        sender.press();
        if ((i + 1) % 50 == 0) {
            sender.stats.log_summary("burst");
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_LINKTEST_BURST_GAP_MS));
    }
    ESP_LOGI(TAG, "=== burst complete ===");
    sender.stats.log_summary("final");
    ESP_LOGI(TAG, "acceptance: receiver applied counter must equal accepted+dup_accepted"
                  " unique presses (%" PRIu32 ")",
             sender.stats.presses - sender.stats.failed);

    // Interactive phase: BOOT button = one real debounced press.
    gpio_config_t button{};
    button.pin_bit_mask = 1ULL << kBootButton;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
    ESP_LOGI(TAG, "sender: press BOOT to send single presses");

    bool was_down = false;
    while (true) {
        const bool down = gpio_get_level(kBootButton) == 0;
        if (down && !was_down) {
            vTaskDelay(pdMS_TO_TICKS(30));  // debounce
            if (gpio_get_level(kBootButton) == 0) {
                sender.press();
                sender.stats.log_summary("button");
            }
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------

[[noreturn]] void run_receiver() {
    padel::protocol::Deduplicator dedup;
    uint32_t applied = 0;
    uint32_t duplicates = 0;
    uint32_t dropped_acks = 0;
    uint32_t parse_errors = 0;

    ESP_LOGI(TAG, "receiver: waiting for presses (dropping %d%% of ACKs)",
             CONFIG_LINKTEST_ACK_DROP_PCT);

    RxFrame frame;
    while (true) {
        if (xQueueReceive(s_rx_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const auto parsed = padel::protocol::parse_point_intent(
            frame.data, static_cast<size_t>(frame.len));
        if (!parsed) {
            ++parse_errors;
            ESP_LOGW(TAG, "parse error %d (len=%d, total=%" PRIu32 ")",
                     static_cast<int>(parsed.error()), frame.len, parse_errors);
            continue;
        }

        padel::protocol::AckStatus status;
        switch (dedup.check_and_record(parsed.value().identity)) {
            case padel::protocol::DedupResult::New:
                ++applied;
                status = padel::protocol::AckStatus::Accepted;
                break;
            case padel::protocol::DedupResult::Duplicate:
                ++duplicates;
                status = padel::protocol::AckStatus::DuplicateAccepted;
                break;
            case padel::protocol::DedupResult::Stale:
            default:
                status = padel::protocol::AckStatus::RejectedInvalidPacket;
                break;
        }

        if (applied % 25 == 0 || status != padel::protocol::AckStatus::Accepted) {
            ESP_LOGI(TAG,
                     "applied=%" PRIu32 " duplicates=%" PRIu32 " dropped_acks=%" PRIu32
                     " (seq=%" PRIu32 " -> %s)",
                     applied, duplicates, dropped_acks, parsed.value().identity.sequence,
                     status == padel::protocol::AckStatus::Accepted          ? "Accepted"
                     : status == padel::protocol::AckStatus::DuplicateAccepted ? "DupAccepted"
                                                                               : "Rejected");
        }

        // Induced ACK loss: the sender must retry and dedup must hold.
        if (esp_random() % 100 < CONFIG_LINKTEST_ACK_DROP_PCT) {
            ++dropped_acks;
            continue;
        }

        ensure_peer(frame.mac);
        padel::protocol::AckPacket ack{};
        ack.court_id = CONFIG_LINKTEST_COURT_ID;
        ack.identity = parsed.value().identity;
        ack.status = status;
        ack.state_revision = applied;
        const auto bytes = padel::protocol::serialize(ack);
        esp_now_send(frame.mac, bytes.data(), bytes.size());
    }
}

}  // namespace

extern "C" void app_main(void) {
    init_radio();
#if CONFIG_LINKTEST_ROLE_SENDER
    run_sender();
#else
    run_receiver();
#endif
}
