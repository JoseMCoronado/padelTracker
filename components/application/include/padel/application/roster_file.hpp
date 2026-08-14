// File-backed roster store ("id|name|origin" lines) and append-only CSV
// results log. Plain cstdio so the same code runs on the desktop simulator and
// on LittleFS via the ESP-IDF VFS.
#pragma once

#include <optional>
#include <string>

#include "padel/application/roster.hpp"

namespace padel::application {

// Whole-file read for the club list. Returns nothing when the file is missing
// or unreadable, which apply_club_list treats as "leave the roster alone".
std::optional<std::string> read_text_file(const std::string& path);

class FileRosterStore : public IRosterStore {
public:
    explicit FileRosterStore(std::string path) : path_(std::move(path)) {}

    std::vector<Player> load() override;
    bool save(const std::vector<Player>& players) override;

private:
    std::string path_;
};

class FileResultsLog : public IResultsLog {
public:
    explicit FileResultsLog(std::string path) : path_(std::move(path)) {}

    bool append(const RoundResult& result) override;

private:
    std::string path_;
};

}  // namespace padel::application
