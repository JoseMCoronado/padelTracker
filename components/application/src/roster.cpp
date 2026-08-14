#include "padel/application/roster.hpp"

#include <algorithm>
#include <cctype>

namespace padel::application {
namespace {

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

std::vector<std::string> parse_club_list(const std::string& text) {
    std::vector<std::string> names;
    std::size_t line_begin = 0;
    while (line_begin <= text.size()) {
        std::size_t line_end = text.find('\n', line_begin);
        if (line_end == std::string::npos) {
            line_end = text.size();
        }
        std::string line = text.substr(line_begin, line_end - line_begin);
        line_begin = line_end + 1;

        if (const std::size_t comment = line.find('#'); comment != std::string::npos) {
            line.erase(comment);
        }
        const std::string name = trimmed(line);
        if (name.empty()) {
            continue;
        }
        const std::string key = lowered(name);
        const bool seen = std::any_of(names.begin(), names.end(), [&](const std::string& other) {
            return lowered(other) == key;
        });
        if (!seen) {
            names.push_back(name);
        }
        if (line_end == text.size()) {
            break;
        }
    }
    return names;
}

PlayerRoster::PlayerRoster(IRosterStore& store) : store_(store) {
    players_ = store_.load();
    for (const Player& player : players_) {
        next_id_ = std::max(next_id_, player.id + 1);
    }
    sort_by_name(players_);
}

ClubListSync PlayerRoster::apply_club_list(const std::vector<std::string>& names) {
    ClubListSync sync{};
    if (names.empty()) {
        return sync;  // "no file" must never read as "no club"
    }
    sync.applied = true;

    const auto listed = [&names](const std::string& name) {
        const std::string key = lowered(name);
        return std::any_of(names.begin(), names.end(), [&key](const std::string& other) {
            return lowered(other) == key;
        });
    };

    bool changed = false;
    for (const std::string& name : names) {
        const std::string key = lowered(name);
        const auto existing = std::find_if(players_.begin(), players_.end(),
                                           [&key](const Player& player) {
                                               return lowered(player.name) == key;
                                           });
        if (existing == players_.end()) {
            players_.push_back(Player{next_id_++, name, false, true});
            ++sync.added;
            changed = true;
        } else if (!existing->from_club_list) {
            // Typed in courtside first, on the list now: the list owns them.
            existing->from_club_list = true;
            changed = true;
        }
    }

    const std::size_t before = players_.size();
    players_.erase(std::remove_if(players_.begin(), players_.end(),
                                  [&listed](const Player& player) {
                                      return player.from_club_list && !listed(player.name);
                                  }),
                   players_.end());
    sync.removed = static_cast<int>(before - players_.size());
    changed = changed || sync.removed > 0;

    if (changed) {
        sort_by_name(players_);
        store_.save(players_);
    }
    return sync;
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
    const Player player{next_id_++, clean, false, false};
    players_.push_back(player);
    sort_by_name(players_);
    store_.save(players_);
    return player;
}

Player PlayerRoster::make_guest() {
    // Guest ids live above the member range and are session-scoped.
    const std::uint32_t guest_number = static_cast<std::uint32_t>(guests_.size()) + 1;
    Player guest{0x80000000u + guest_number,
                 guest_number == 1 ? "GUEST" : "GUEST " + std::to_string(guest_number), true,
                 false};
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
