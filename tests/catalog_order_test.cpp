// Guards GetCustomBmsSongsForMode's ordering.
//
// It sorted the copied songs themselves, finding each one's category rank by a linear _stricmp
// over every category -- twice per comparison. Over a 23k-song mode that was over a second on the
// cabinet, and song select paid it on every category change. It now sorts (rank, index) pairs with
// the ranks from a hash map. The only thing that matters is that the order is EXACTLY the old one:
// the inject's idempotency marker is the first id of this list, and the wheel order is this list.
// So this brute-forces both against each other on random catalogues with mixed-case categories,
// duplicate titles and other modes interleaved.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o catalog_order_test tests/catalog_order_test.cpp
//   ./catalog_order_test
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
static int _stricmp(const char* a, const char* b) {
    while (*a && *b) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
        ++a; ++b;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
#endif

struct Song { std::string id, mode, category, title; };
static std::vector<Song> g_songs;

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)tolower(c); });
    return s;
}
static bool TitleLess(const Song& a, const Song& b) { return a.title < b.title; }

static std::vector<Song> oldOrder(const std::string& n) {
    std::vector<Song> out;
    for (const auto& s : g_songs) if (n.empty() || _stricmp(s.mode.c_str(), n.c_str()) == 0) out.push_back(s);
    std::vector<std::string> categories;
    for (const auto& song : out) {
        bool seen = false;
        for (const auto& category : categories)
            if (_stricmp(category.c_str(), song.category.c_str()) == 0) { seen = true; break; }
        if (!seen) categories.push_back(song.category);
    }
    auto categoryRank = [&](const std::string& category) {
        for (size_t i = 0; i < categories.size(); ++i)
            if (_stricmp(categories[i].c_str(), category.c_str()) == 0) return i;
        return categories.size();
    };
    std::stable_sort(out.begin(), out.end(), [&](const Song& a, const Song& b) {
        size_t ra = categoryRank(a.category), rb = categoryRank(b.category);
        return ra != rb ? ra < rb : TitleLess(a, b);
    });
    return out;
}

static std::vector<Song> newOrder(const std::string& n) {
    std::vector<std::pair<size_t, size_t> > order;
    std::unordered_map<std::string, size_t> ranks;
    for (size_t i = 0; i < g_songs.size(); ++i) {
        const Song& s = g_songs[i];
        if (!n.empty() && _stricmp(s.mode.c_str(), n.c_str()) != 0) continue;
        const size_t rank = ranks.emplace(lowerCopy(s.category), ranks.size()).first->second;
        order.emplace_back(rank, i);
    }
    std::stable_sort(order.begin(), order.end(), [](const std::pair<size_t, size_t>& a, const std::pair<size_t, size_t>& b) {
        return a.first != b.first ? a.first < b.first : TitleLess(g_songs[a.second], g_songs[b.second]);
    });
    std::vector<Song> out;
    for (const auto& o : order) out.push_back(g_songs[o.second]);
    return out;
}

int main() {
    const char* cats[] = {"ANX", "anx", "Arc.1", "ARC.1", "piu", "PIU", "Zeta", "b"};
    const char* modes[] = {"5keyMix", "5KEYMIX", "streetMix"};
    const char* titles[] = {"a", "b", "B", "c", "a", "zz"};
    srand(1337);
    for (int round = 0; round < 300; ++round) {
        g_songs.clear();
        const int n = rand() % 120;
        for (int i = 0; i < n; ++i)
            g_songs.push_back({"bms" + std::to_string(i), modes[rand() % 3], cats[rand() % 8], titles[rand() % 6]});
        for (const char* mode : {"5keyMix", "streetMix", "", "nothing"}) {
            const std::vector<Song> a = oldOrder(mode), b = newOrder(mode);
            if (a.size() != b.size()) { printf("FAIL round %d mode '%s': size %zu vs %zu\n", round, mode, a.size(), b.size()); return 1; }
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].id != b[i].id) { printf("FAIL round %d mode '%s' at %zu: %s vs %s\n", round, mode, i, a[i].id.c_str(), b[i].id.c_str()); return 1; }
        }
    }
    printf("catalog order: ok (300 random catalogues x 4 modes, identical to the linear-rank sort)\n");
    return 0;
}
