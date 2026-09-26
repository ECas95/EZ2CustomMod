#pragma once
// What the player has done with the custom songs: favourites and plays. It feeds the three virtual
// categories at the head of the custom list (FAVORITES, RECENT, MOST PLAYED).
//
// A song is keyed by its folder relative to BMSData, lowercased -- not by its id. Ids are per mode
// (every auto-generated mode has its own copy of a song, with its own id) while the folder is the
// same for all of them, so a favourite marked in 5K is a favourite in every mode, and plays count
// once per song whichever mode they were in.
//
// Stored as text in CustomCache\playdata.txt, one record a line, tab-separated:
//   F <added>  <key>             a favourite, and when it was marked
//   P <count> <last> <key>       a song played <count> times, last at <last>
// Times are a monotonic counter bumped on every change, not a clock: only their order matters and
// the cabinet's clock is not something to rely on.
#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

class PlayData {
public:
    struct Play { unsigned long count = 0; unsigned long last = 0; };

    bool isFavorite(const std::string& key) const { return fav_.count(key) != 0; }

    // Returns the new state.
    bool toggleFavorite(const std::string& key) {
        ++clock_;
        auto it = fav_.find(key);
        if (it != fav_.end()) { fav_.erase(it); return false; }
        fav_[key] = clock_;
        return true;
    }

    void recordPlay(const std::string& key) {
        Play& p = plays_[key];
        ++p.count;
        p.last = ++clock_;
    }

    unsigned long plays(const std::string& key) const {
        auto it = plays_.find(key);
        return it == plays_.end() ? 0 : it->second.count;
    }

    // Newest favourite first.
    std::vector<std::string> favorites(size_t max) const {
        std::vector<std::pair<unsigned long, std::string>> v;
        for (const auto& f : fav_) v.push_back({f.second, f.first});
        std::sort(v.begin(), v.end(), [](const std::pair<unsigned long, std::string>& a,
                                         const std::pair<unsigned long, std::string>& b) { return a.first > b.first; });
        return keys(v, max);
    }

    // Most recently played first.
    std::vector<std::string> recent(size_t max) const {
        std::vector<std::pair<unsigned long, std::string>> v;
        for (const auto& p : plays_) v.push_back({p.second.last, p.first});
        std::sort(v.begin(), v.end(), [](const std::pair<unsigned long, std::string>& a,
                                         const std::pair<unsigned long, std::string>& b) { return a.first > b.first; });
        return keys(v, max);
    }

    // Most plays first; a tie goes to the one played more recently.
    std::vector<std::string> mostPlayed(size_t max) const {
        std::vector<std::pair<std::pair<unsigned long, unsigned long>, std::string>> v;
        for (const auto& p : plays_) v.push_back({{p.second.count, p.second.last}, p.first});
        std::sort(v.begin(), v.end(), [](const decltype(v)::value_type& a, const decltype(v)::value_type& b) {
            return a.first > b.first;
        });
        std::vector<std::string> out;
        for (size_t i = 0; i < v.size() && i < max; ++i) out.push_back(v[i].second);
        return out;
    }

    std::string serialize() const {
        std::ostringstream o;
        o << "C\t" << clock_ << "\n";
        for (const auto& f : fav_) o << "F\t" << f.second << "\t" << f.first << "\n";
        for (const auto& p : plays_) o << "P\t" << p.second.count << "\t" << p.second.last << "\t" << p.first << "\n";
        return o.str();
    }

    // Malformed lines are skipped, never fatal: the file is the player's history, and a bad line
    // must not cost the rest of it.
    void parse(const std::string& text) {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f;
            size_t a = 0;
            for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
            f.push_back(line.substr(a));
            char* end = nullptr;
            if (f[0] == "C" && f.size() >= 2) {
                clock_ = std::max(clock_, std::strtoul(f[1].c_str(), &end, 10));
            } else if (f[0] == "F" && f.size() >= 3 && !f[2].empty()) {
                const unsigned long t = std::strtoul(f[1].c_str(), &end, 10);
                fav_[f[2]] = t;
                clock_ = std::max(clock_, t);
            } else if (f[0] == "P" && f.size() >= 4 && !f[3].empty()) {
                Play p;
                p.count = std::strtoul(f[1].c_str(), &end, 10);
                p.last = std::strtoul(f[2].c_str(), &end, 10);
                if (p.count) plays_[f[3]] = p;
                clock_ = std::max(clock_, p.last);
            }
        }
    }

private:
    static std::vector<std::string> keys(const std::vector<std::pair<unsigned long, std::string>>& v, size_t max) {
        std::vector<std::string> out;
        for (size_t i = 0; i < v.size() && i < max; ++i) out.push_back(v[i].second);
        return out;
    }
    std::map<std::string, unsigned long> fav_;
    std::map<std::string, Play> plays_;
    unsigned long clock_ = 0;
};
