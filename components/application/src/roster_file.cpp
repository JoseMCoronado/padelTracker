#include "padel/application/roster_file.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace padel::application {

namespace {

// Third field of a roster line. Absent on files written before provenance was
// tracked, and back then every member came from the built-in list, so a
// two-field line reads as "club".
constexpr const char* kOriginClub = "club";
constexpr const char* kOriginLocal = "local";

}  // namespace

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
        std::string rest = sep + 1;
        while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) {
            rest.pop_back();
        }

        std::string name = rest;
        bool from_club_list = true;
        if (const std::size_t origin_sep = rest.rfind('|'); origin_sep != std::string::npos) {
            name = rest.substr(0, origin_sep);
            from_club_list = rest.substr(origin_sep + 1) != kOriginLocal;
        }
        if (name.empty()) {
            continue;
        }
        Player player{};
        player.id = static_cast<std::uint32_t>(std::strtoul(line, nullptr, 10));
        player.name = std::move(name);
        player.from_club_list = from_club_list;
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
        if (std::fprintf(file, "%lu|%s|%s\n", static_cast<unsigned long>(player.id),
                         player.name.c_str(),
                         player.from_club_list ? kOriginClub : kOriginLocal) < 0) {
            ok = false;
            break;
        }
    }
    // fflush + fclose so a power cut right after a save loses at most this file.
    std::fflush(file);
    std::fclose(file);
    return ok;
}

std::optional<std::string> read_text_file(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        return std::nullopt;
    }
    std::string text;
    char chunk[256];
    while (const std::size_t read = std::fread(chunk, 1, sizeof(chunk), file)) {
        text.append(chunk, read);
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        return std::nullopt;
    }
    return text;
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
