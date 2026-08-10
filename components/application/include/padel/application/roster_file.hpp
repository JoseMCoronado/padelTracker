// File-backed roster store ("id|name" lines) and append-only CSV results
// log. Plain cstdio so the same code runs on the desktop simulator and on
// LittleFS via the ESP-IDF VFS.
#pragma once

#include <string>

#include "padel/application/roster.hpp"

namespace padel::application {

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
