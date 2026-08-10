#include "padel/application/roster.hpp"

#include <algorithm>
#include <cctype>

namespace padel::application {
namespace {

const char* const kSeedNames[] = {"Jose",   "Zoe",     "William", "Szewei",
                                   "Ruxandra", "Lewis", "Luigi",   "Raymond",
                                   "Paulina", "Vineet", "Louis",   "Adrien"};

std::string trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string lowered(const std::string& text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

void sort_by_name(std::vector<Player>& players) {
    std::sort(players.begin(), players.end(), [](const Player& a, const Player& b) {
        return lowered(a.name) < lowered(b.name);
    });
}

}  // namespace

PlayerRoster::PlayerRoster(IRosterStore& store) : store_(store) {
    players_ = store_.load();
    if (players_.empty()) {
        for (const char* name : kSeedNames) {
            players_.push_back(Player{next_id_++, name, false});
        }
        store_.save(players_);
    }
    for (const Player& player : players_) {
        next_id_ = std::max(next_id_, player.id + 1);
    }
    sort_by_name(players_);
}

std::vector<Player> PlayerRoster::filtered(const std::string& query) const {
    const std::string needle = lowered(trimmed(query));
    if (needle.empty()) {
        return players_;
    }
    std::vector<Player> matches;
    for (const Player& player : players_) {
        if (lowered(player.name).find(needle) != std::string::npos) {
            matches.push_back(player);
        }
    }
    return matches;
}

std::optional<Player> PlayerRoster::add_player(const std::string& name) {
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        return std::nullopt;
    }
    const std::string key = lowered(clean);
    for (const Player& player : players_) {
        if (lowered(player.name) == key) {
            return std::nullopt;  // duplicate
        }
    }
    const Player player{next_id_++, clean, false};
    players_.push_back(player);
    sort_by_name(players_);
    store_.save(players_);
    return player;
}

Player PlayerRoster::make_guest() {
    // Guest ids live above the member range and are session-scoped.
    const std::uint32_t guest_number = static_cast<std::uint32_t>(guests_.size()) + 1;
    Player guest{0x80000000u + guest_number,
                 guest_number == 1 ? "GUEST" : "GUEST " + std::to_string(guest_number), true};
    guests_.push_back(guest);
    return guest;
}

std::optional<Player> PlayerRoster::find(std::uint32_t id) const {
    for (const Player& player : players_) {
        if (player.id == id) {
            return player;
        }
    }
    for (const Player& guest : guests_) {
        if (guest.id == id) {
            return guest;
        }
    }
    return std::nullopt;
}

}  // namespace padel::application
