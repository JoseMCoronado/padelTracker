// Player roster for club play (dropdown-free touch picker feeds from this)
// and the per-round results log — the foundation for future player stats
// and sync. Storage is behind small interfaces: files natively, LittleFS
// files on the device; sync is a later concern.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace padel::application {

struct Player {
    std::uint32_t id = 0;
    std::string name;
    bool guest = false;  // ephemeral, never persisted, excluded from stats
    // Came from the checked-in club list rather than NEW PLAYER on the court.
    // The list owns these: dropping a name from it drops the player. Players
    // typed in courtside are nobody's to delete, so they survive every sync.
    bool from_club_list = true;
};

class IRosterStore {
public:
    virtual ~IRosterStore() = default;
    virtual std::vector<Player> load() = 0;
    virtual bool save(const std::vector<Player>& players) = 0;
};

// One line per player per completed club round.
struct RoundResult {
    std::uint32_t player_id = 0;
    std::string player_name;  // denormalized so guest rows stay readable
    bool guest = false;
    std::uint8_t wins = 0;    // 0..2 across the two mini-sets
    int differential = 0;
    bool top2 = false;
    bool decided_by_coin_flip = false;  // this player's spot came from the flip
    std::uint64_t timestamp_ms = 0;
};

class IResultsLog {
public:
    virtual ~IResultsLog() = default;
    virtual bool append(const RoundResult& result) = 0;
};

// The checked-in club list (config/players.txt): one name per line, '#' starts
// a comment, blank lines are skipped, surrounding space trimmed, and names
// repeated case-insensitively collapse to one. Parsing is separate from
// reading so the firmware can hand over a list embedded in its binary while
// the simulator reads the file off disk.
std::vector<std::string> parse_club_list(const std::string& text);

// What a sync did, for the boot log.
struct ClubListSync {
    int added = 0;
    int removed = 0;
    bool applied = false;  // false when the list was missing or empty
};

// In-memory roster over a store. Starts as whatever the store holds — the club
// list fills it (see apply_club_list) — and guests are minted on demand and
// never written back.
class PlayerRoster {
public:
    explicit PlayerRoster(IRosterStore& store);

    // Members only (no guests), sorted by name for stable UI grids.
    const std::vector<Player>& players() const { return players_; }

    // Case-insensitive substring filter ("jo" -> JOSE, JOHN).
    std::vector<Player> filtered(const std::string& query) const;

    // Adds a member (trimmed; empty or duplicate names rejected) and
    // persists. Returns the new player.
    std::optional<Player> add_player(const std::string& name);

    // Reconciles the roster with the club list, which wins for its own
    // entries: names it gained are added keeping existing ids, names it lost
    // are dropped, and a name it now carries adopts a player who had been
    // typed in courtside. Players the list never mentioned stay put.
    //
    // An empty list is ignored rather than obeyed. A missing file, a failed
    // read or a truncated one would otherwise read as "the club has no
    // members" and delete everybody.
    ClubListSync apply_club_list(const std::vector<std::string>& names);

    // Mints GUEST / GUEST 2 / ... for this session; not persisted.
    Player make_guest();

    std::optional<Player> find(std::uint32_t id) const;

private:
    IRosterStore& store_;
    std::vector<Player> players_;
    std::vector<Player> guests_;
    std::uint32_t next_id_ = 1;
};

}  // namespace padel::application
