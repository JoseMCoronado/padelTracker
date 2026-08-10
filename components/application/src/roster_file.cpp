#include "padel/application/roster_file.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace padel::application {

std::vector<Player> FileRosterStore::load() {
    std::vector<Player> players;
    std::FILE* file = std::fopen(path_.c_str(), "r");
    if (file == nullptr) {
        return players;
    }
    char line[128];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char* sep = std::strchr(line, '|');
        if (sep == nullptr || sep == line) {
            continue;
        }
        *sep = '\0';
        std::string name = sep + 1;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
            name.pop_back();
        }
        if (name.empty()) {
            continue;
        }
        Player player{};
        player.id = static_cast<std::uint32_t>(std::strtoul(line, nullptr, 10));
        player.name = std::move(name);
        players.push_back(std::move(player));
    }
    std::fclose(file);
    return players;
}

bool FileRosterStore::save(const std::vector<Player>& players) {
    std::FILE* file = std::fopen(path_.c_str(), "w");
    if (file == nullptr) {
        return false;
    }
    bool ok = true;
    for (const Player& player : players) {
        if (std::fprintf(file, "%lu|%s\n", static_cast<unsigned long>(player.id),
                         player.name.c_str()) < 0) {
            ok = false;
            break;
        }
    }
    // fflush + fclose so a power cut right after a save loses at most this file.
    std::fflush(file);
    std::fclose(file);
    return ok;
}

bool FileResultsLog::append(const RoundResult& r) {
    // Write the CSV header only when creating the file.
    std::FILE* probe = std::fopen(path_.c_str(), "r");
    const bool fresh = probe == nullptr;
    if (probe != nullptr) {
        std::fclose(probe);
    }

    std::FILE* file = std::fopen(path_.c_str(), "a");
    if (file == nullptr) {
        return false;
    }
    if (fresh) {
        std::fprintf(file,
                     "timestamp_ms,player_id,player_name,guest,wins,differential,top2,coin_flip\n");
    }
    const bool ok =
        std::fprintf(file, "%llu,%lu,%s,%d,%d,%d,%d,%d\n",
                     static_cast<unsigned long long>(r.timestamp_ms),
                     static_cast<unsigned long>(r.player_id), r.player_name.c_str(),
                     r.guest ? 1 : 0, static_cast<int>(r.wins), r.differential, r.top2 ? 1 : 0,
                     r.decided_by_coin_flip ? 1 : 0) >= 0;
    std::fflush(file);
    std::fclose(file);
    return ok;
}

}  // namespace padel::application
