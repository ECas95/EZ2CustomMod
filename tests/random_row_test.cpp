// Guards the RANDOM row the filter puts at the top of every category.
//
// visible[0] is reserved before phase 2 runs, so the real songs land at 1..N. That off-by-one runs
// through four places that have to agree, and each one fails differently:
//
//   - phase 2 starting at 0 would overwrite the RANDOM row with a real song
//   - publishing `written` without dropping the reserved row leaves a category of exactly one
//     RANDOM row that can only roll itself
//   - a roll of 0 would copy the row over itself and play whatever it already held
//   - the selection restore matching at index 0 lands the cursor on RANDOM whenever the song it
//     happened to roll is the one being restored
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o random_row_test tests/random_row_test.cpp
//   ./random_row_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- the shape of the filter's phase 2 + the RANDOM row, over a plain vector -------------------
struct Row { std::string id; float level; unsigned nameTex; unsigned discTex; };

struct Filter {
    std::vector<Row> visible;
    unsigned written = 0;
    int lastPick = -1;
};

static void rollRandomRow(Filter& f, unsigned count, unsigned randomTex) {
    if (!count) return;
    const unsigned pick = 1 + (unsigned)(rand() % (int)count);
    f.lastPick = (int)pick;
    f.visible[0] = f.visible[pick];      // a byte-for-byte copy of a real row
    f.visible[0].nameTex = randomTex;    // ...except the songname
    f.visible[0].discTex = 0;            // ...and the disc stays hidden
}

// gathered = what the category holds; a level of 0 means "no chart in this mode".
static Filter runFilter(const std::vector<Row>& gathered, const std::string& want,
                        unsigned& selectedOut, unsigned randomTex) {
    Filter f;
    f.visible.assign(gathered.size() + 1, Row{"", 0.0f, 0, 0});
    f.written = 1;                                   // row 0 reserved
    for (const Row& g : gathered) {
        if (g.level == 0.0f) continue;               // the game's own visibility test
        f.visible[f.written] = g;
        ++f.written;
    }
    if (f.written > 1) rollRandomRow(f, f.written - 1, randomTex);
    else                f.written = 0;               // nothing to roll: take the row back

    selectedOut = 0;
    for (unsigned i = 1; i < f.written; ++i)         // starts at 1, never matches row 0
        if (!want.empty() && f.visible[i].id == want) { selectedOut = i; break; }
    return f;
}
// -----------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

static std::vector<Row> makeSongs(unsigned n, unsigned unplayable = 0) {
    std::vector<Row> v;
    for (unsigned i = 0; i < n; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "bms%06u", i);
        v.push_back(Row{id, i < unplayable ? 0.0f : 5.0f, 0x1000u + i, 0x2000u + i});
    }
    return v;
}

int main() {
    const unsigned TEX = 0xABCD;
    std::srand(20260901);

    // A normal category: one RANDOM row plus every playable song.
    {
        unsigned sel = 0;
        Filter f = runFilter(makeSongs(40), "", sel, TEX);
        check(f.written == 41, "40 songs publish 41 rows (RANDOM + 40)");
        check(f.visible[0].nameTex == TEX, "row 0 carries the RANDOM tile");
        check(f.visible[0].discTex == 0, "row 0 shows no disc, so the roll stays hidden");
        check(!f.visible[0].id.empty(), "row 0 carries a REAL id, so confirming it plays a song");
        check(f.visible[1].id == "bms000000", "the first real song is at index 1, not 0");
    }

    // Unplayable songs are skipped, and the count still leaves exactly one reserved row.
    {
        unsigned sel = 0;
        Filter f = runFilter(makeSongs(40, 15), "", sel, TEX);   // 15 have no chart here
        check(f.written == 26, "25 playable songs publish 26 rows");
        check(f.visible[0].nameTex == TEX, "the RANDOM row survives the skipping");
    }

    // A category with nothing playable must publish NOTHING, not a lone RANDOM row.
    {
        unsigned sel = 0;
        Filter f = runFilter(makeSongs(12, 12), "", sel, TEX);
        check(f.written == 0, "a category with no playable song publishes no rows at all");
    }
    {
        unsigned sel = 0;
        Filter f = runFilter(std::vector<Row>(), "", sel, TEX);
        check(f.written == 0, "an empty category publishes no rows either");
    }

    // The roll must always land on a real row: never 0 (itself), never past the end.
    {
        for (unsigned n : {1u, 2u, 3u, 17u, 400u}) {
            for (int trial = 0; trial < 400; ++trial) {
                unsigned sel = 0;
                Filter f = runFilter(makeSongs(n), "", sel, TEX);
                if (f.written == 0) continue;
                if (f.lastPick >= 1 && (unsigned)f.lastPick < f.written) continue;
                char msg[96];
                std::snprintf(msg, sizeof(msg), "roll landed on %d with %u rows", f.lastPick, f.written);
                check(false, msg);
                break;
            }
        }
        check(true, "every roll picks a real row (1..N), never row 0 and never past the end");
    }

    // Restoring a selection must not land on RANDOM just because it rolled that same song.
    {
        // Force the case: roll repeatedly until row 0 holds the id we are about to restore.
        bool covered = false;
        for (int trial = 0; trial < 2000 && !covered; ++trial) {
            unsigned sel = 0;
            Filter f = runFilter(makeSongs(6), "", sel, TEX);
            const std::string rolled = f.visible[0].id;
            unsigned sel2 = 0;
            Filter g = runFilter(makeSongs(6), rolled, sel2, TEX);
            if (g.visible[0].id != rolled) continue;      // need the same roll to reproduce it
            covered = true;
            check(sel2 != 0, "restoring the rolled song selects the real row, not RANDOM");
            check(g.visible[sel2].id == rolled, "and it is the right row");
        }
        check(covered, "the test actually reached the case it is guarding");
    }

    if (g_failures == 0) std::printf("random row: ok\n");
    return g_failures ? 1 : 0;
}
