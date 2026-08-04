// Storage adapters for the court: LittleFS mount for the journal (via the
// existing POSIX StdioFileBackend) and an NVS-backed ISettings for the
// remote allow-list (spec 10.8, 12.2).
#pragma once

#include <vector>

#include "padel/application/settings.hpp"

namespace storage {

// Mounts the "storage" LittleFS partition at /littlefs. Formats on first
// boot. Returns false on unrecoverable mount failure.
bool mount();

inline const char* journal_path() { return "/littlefs/journal.bin"; }

// Renames the current journal aside (journal-<n>.bin) so a fresh match
// starts a fresh journal (spec 14.7).
bool archive_journal();

class NvsSettings : public padel::application::ISettings {
public:
    bool open();

    std::vector<padel::application::StoredAssignment> load_assignments() override;
    bool save_assignments(
        const std::vector<padel::application::StoredAssignment>& assignments) override;

private:
    unsigned int handle_ = 0;  // nvs_handle_t, avoids nvs.h in the header
};

}  // namespace storage
