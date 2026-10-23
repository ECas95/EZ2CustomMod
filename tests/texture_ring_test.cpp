// Texture ring: which requests it takes, and which slot each gets.
#include "../src/texture_ring.h"
#include <cassert>
#include <cstdio>

int main() {
    static const char* const dirs[] = {"songname", "discsmall", "disc", "eyecatch", "resultdisc"};
    std::string id;

    // "disc" must not swallow "discsmall" requests, and case does not matter.
    assert(RingRoleOf("system\\songname\\bms000123.bmp", dirs, 5, id) == 0 && id == "bms000123");
    assert(RingRoleOf("System\\DiscSmall\\BMS019096.abm", dirs, 5, id) == 1 && id == "bms019096");
    assert(RingRoleOf("system\\disc\\bms000007.bmp", dirs, 5, id) == 2 && id == "bms000007");
    assert(RingRoleOf("system\\eyecatch\\bms1.bmp", dirs, 5, id) == 3);
    // Native songs, other folders and nested paths stay with the game's own cache.
    assert(RingRoleOf("system\\songname\\babydance.bmp", dirs, 5, id) == -1);
    assert(RingRoleOf("system\\SongSelect\\Sortimage\\category_48.abm", dirs, 5, id) == -1);
    assert(RingRoleOf("system\\disc\\bms1\\frame01.bmp", dirs, 5, id) == -1);
    assert(RingRoleOf(nullptr, dirs, 5, id) == -1);

    // Unused slots fill in order; a song already in the ring gets its own slot back.
    std::vector<RingSlot> ring(3);
    int t1, t2, t3;
    bool same = false;
    RingSlot* s = PickRingSlot(ring, "bmsa", same);
    assert(s == &ring[0] && !same);
    s->tex = &t1; s->id = "bmsa"; s->stamp = 1;
    s = PickRingSlot(ring, "bmsb", same);
    assert(s == &ring[1] && !same);
    s->tex = &t2; s->id = "bmsb"; s->stamp = 2;
    s = PickRingSlot(ring, "bmsc", same);
    assert(s == &ring[2]);
    s->tex = &t3; s->id = "bmsc"; s->stamp = 3;
    s = PickRingSlot(ring, "bmsb", same);
    assert(s == &ring[1] && same);
    s->stamp = 4;                                   // b used again: a is now the oldest

    // Full ring: the least recently used slot is recycled, never one used since.
    s = PickRingSlot(ring, "bmsd", same);
    assert(s == &ring[0] && !same);
    s->id = "bmsd"; s->stamp = 5;
    s = PickRingSlot(ring, "bmse", same);
    assert(s == &ring[2] && !same);                 // c (3) is older than b (4) and d (5)

    std::vector<RingSlot> none;
    assert(PickRingSlot(none, "bmsa", same) == nullptr);
    // After a pool rebuild a slot whose texture now points at another surface is emptied; one
    // still on the surface it took survives. Then the emptied slot is the first one handed out.
    {
        struct FakeTex { void* surface; };
        int sA, sB, sNew;
        FakeTex ta{&sA}, tb{&sB};
        std::vector<RingSlot> r2(2);
        r2[0].tex = &ta; r2[0].surf = &sA; r2[0].id = "bmsa"; r2[0].stamp = 5;
        r2[1].tex = &tb; r2[1].surf = &sB; r2[1].id = "bmsb"; r2[1].stamp = 6;
        tb.surface = &sNew;                       // recycled at the same address for something else
        auto surfaceOf = [](void* t) { return static_cast<FakeTex*>(t)->surface; };
        assert(DropStaleRingSlots(r2, surfaceOf) == 1);
        assert(r2[0].tex == &ta && r2[0].id == "bmsa");
        assert(r2[1].tex == nullptr && r2[1].id.empty());
        s = PickRingSlot(r2, "bmsb", same);
        assert(s == &r2[1] && !same);             // not "same": the old b texture is gone
        assert(DropStaleRingSlots(r2, surfaceOf) == 0);
    }
    std::puts("texture_ring_test OK");
    return 0;
}
