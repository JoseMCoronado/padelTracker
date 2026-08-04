#include "storage.hpp"

#include <cstdio>
#include <cstring>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"

namespace storage {
namespace {

const char* TAG = "storage";

// Fixed-size NVS blob for assignments (kMaxRemotes on the service is 8).
struct AssignmentBlob {
    struct Entry {
        uint32_t remote_id;
        uint8_t team;  // 0 = A, 1 = B
    } entries[8];
    uint8_t count;
};

}  // namespace

bool mount() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/littlefs";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0;
    size_t used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "littlefs mounted: %zu/%zu bytes used", used, total);
    return true;
}

bool archive_journal() {
    // Find a free archive slot.
    char archived[64];
    for (int i = 0; i < 1000; ++i) {
        std::snprintf(archived, sizeof(archived), "/littlefs/journal-%03d.bin", i);
        FILE* probe = std::fopen(archived, "rb");
        if (probe == nullptr) {
            break;
        }
        std::fclose(probe);
    }
    if (std::rename(journal_path(), archived) != 0) {
        ESP_LOGE(TAG, "journal archive failed");
        return false;
    }
    ESP_LOGI(TAG, "journal archived to %s", archived);
    return true;
}

bool NvsSettings::open() {
    nvs_handle_t handle = 0;
    if (nvs_open("padel_court", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    handle_ = handle;
    return true;
}

std::vector<padel::application::StoredAssignment> NvsSettings::load_assignments() {
    std::vector<padel::application::StoredAssignment> assignments;
    AssignmentBlob blob{};
    size_t size = sizeof(blob);
    if (nvs_get_blob(handle_, "assign", &blob, &size) != ESP_OK || size != sizeof(blob)) {
        return assignments;
    }
    const uint8_t count = blob.count <= 8 ? blob.count : 8;
    for (uint8_t i = 0; i < count; ++i) {
        assignments.push_back({blob.entries[i].remote_id,
                               blob.entries[i].team == 0 ? padel::TeamId::A
                                                         : padel::TeamId::B});
    }
    return assignments;
}

bool NvsSettings::save_assignments(
    const std::vector<padel::application::StoredAssignment>& assignments) {
    AssignmentBlob blob{};
    blob.count = 0;
    for (const auto& assignment : assignments) {
        if (blob.count >= 8) {
            break;
        }
        blob.entries[blob.count].remote_id = assignment.remote_id;
        blob.entries[blob.count].team = assignment.team == padel::TeamId::A ? 0 : 1;
        ++blob.count;
    }
    return nvs_set_blob(handle_, "assign", &blob, sizeof(blob)) == ESP_OK &&
           nvs_commit(handle_) == ESP_OK;
}

}  // namespace storage
