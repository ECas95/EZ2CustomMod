// Guards the BGA preview's ownership of the disc slot.
//
// The preview borrows one pointer -- obj+260, the selected song's disc -- and must give it back.
// Both crashes this code has produced were ownership mistakes rather than rendering ones:
//
//   1. It built its texture on system\disc\initdisc.bmp. The texture pool is keyed by filename, so
//      that is the SAME object the game uses, and releasing its surface freed one still in use.
//   2. Leaving song select frees the SongSelect object. The next entry gets a new one, and the
//      restore wrote the saved pointer back into the OLD address -- a write to obj+0x104 on freed
//      memory, which is 260 exactly.
//
// Neither is visible in a frame of video; both are visible in the state machine below. So the state
// machine is what gets pinned.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o preview_lifecycle_test tests/preview_lifecycle_test.cpp
//   ./preview_lifecycle_test
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

// ---- the model ---------------------------------------------------------------------------------
static const uint32_t kObjDiscSlot = 260;

// Objects that are still alive. Writing anywhere else is the bug.
static std::set<uint32_t> g_live;
static bool g_wroteToDeadObject = false;
static uint32_t g_lastWriteAddr = 0;

static void SlotWrite(uint32_t obj, uint32_t value) {
    (void)value;
    g_lastWriteAddr = obj + kObjDiscSlot;
    if (g_live.find(obj) == g_live.end()) g_wroteToDeadObject = true;
}

struct Preview {
    uint32_t obj = 0;
    std::string wantId, haveId;
    uint32_t savedSlot = 0;
    bool swapped = false;
    void* texture = nullptr;
    void* surface = nullptr;
    bool initTried = false;
    uint32_t frameCount = 0;
};

// Copied from PreviewRestoreSlot: onObj is what the caller holds right now.
static void RestoreSlot(Preview& p, uint32_t onObj) {
    if (p.swapped && p.obj && onObj == p.obj) SlotWrite(p.obj, p.savedSlot);
    p.swapped = false;
    p.savedSlot = 0;
}

// Copied from BgaPreviewTick's dispatch.
static void Tick(Preview& p, uint32_t obj, const std::string& id) {
    if (obj != p.obj) {
        RestoreSlot(p, 0);                 // the old object is gone: drop, never write
        p.texture = nullptr;
        p.surface = nullptr;
        p.initTried = false;
        p.haveId.clear();
        p.frameCount = 0;
        p.obj = obj;
        p.wantId = id;
        return;
    }
    if (id != p.wantId) {
        RestoreSlot(p, obj);
        p.wantId = id;
        return;
    }
}

// What happens once a preview is actually running on the current song.
static void Swap(Preview& p, uint32_t obj, uint32_t currentDisc) {
    if (!p.swapped) { p.savedSlot = currentDisc; p.swapped = true; }
    SlotWrite(obj, 0xBEEF);
}
// -------------------------------------------------------------------------------------------------

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

int main() {
    // Moving between songs on the same screen: the disc goes back each time.
    {
        g_live = {0x1000}; g_wroteToDeadObject = false;
        Preview p;
        Tick(p, 0x1000, "bms1");
        Swap(p, 0x1000, 0xAAAA);
        check(p.swapped, "the preview owns the slot while it runs");
        Tick(p, 0x1000, "bms2");
        check(!p.swapped, "moving to another song gives the slot back");
        check(g_lastWriteAddr == 0x1000 + kObjDiscSlot, "and gives it back on the right object");
        check(!g_wroteToDeadObject, "no write ever landed outside a live object");
    }

    // The crash: leave song select, come back, and the object is a different one.
    {
        g_live = {0x3fce0000}; g_wroteToDeadObject = false;
        Preview p;
        Tick(p, 0x3fce0000, "bms1");
        Swap(p, 0x3fce0000, 0xAAAA);
        check(p.swapped, "a preview is running when the screen is left");

        g_live = {0x4de10000};                       // old object freed, new one created
        Tick(p, 0x4de10000, "bms1");
        check(!g_wroteToDeadObject,
              "re-entering song select does not write the disc back into the freed object");
        check(p.obj == 0x4de10000, "the preview follows the new object");
        check(!p.swapped, "and owns nothing on it yet");
    }

    // The texture pool goes with the object, so nothing may be carried across.
    {
        g_live = {0x1000}; g_wroteToDeadObject = false;
        Preview p;
        Tick(p, 0x1000, "bms1");
        p.texture = (void*)0x1234; p.surface = (void*)0x5678;
        p.initTried = true; p.haveId = "bms1"; p.frameCount = 60;

        g_live = {0x2000};
        Tick(p, 0x2000, "bms1");
        check(p.texture == nullptr, "the texture is dropped with the object that owned its pool");
        check(p.surface == nullptr, "so is the surface");
        check(!p.initTried, "and the build is allowed to happen again");
        check(p.frameCount == 0 && p.haveId.empty(), "decoded frames are not reused across screens");
    }

    // The same id on a new object must still be treated as new: it is a different record.
    {
        g_live = {0x1000}; g_wroteToDeadObject = false;
        Preview p;
        Tick(p, 0x1000, "bms1");
        Swap(p, 0x1000, 0xAAAA);
        g_live = {0x2000};
        Tick(p, 0x2000, "bms1");                     // identical id, new object
        check(!g_wroteToDeadObject, "an unchanged id does not excuse writing to the old object");
    }

    // Restoring with nothing swapped is harmless, and restoring twice does not write twice.
    {
        g_live = {0x1000}; g_wroteToDeadObject = false;
        Preview p;
        Tick(p, 0x1000, "bms1");
        RestoreSlot(p, 0x1000);
        check(!g_wroteToDeadObject, "restoring when nothing was swapped writes nothing");
        Swap(p, 0x1000, 0xAAAA);
        RestoreSlot(p, 0x1000);
        g_lastWriteAddr = 0;
        RestoreSlot(p, 0x1000);
        check(g_lastWriteAddr == 0, "a second restore is a no-op");
    }

    if (g_failures == 0) std::printf("preview lifecycle: ok\n");
    return g_failures ? 1 : 0;
}
