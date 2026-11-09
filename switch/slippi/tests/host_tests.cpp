// Loopback test for switch/slippi/slippi_host: the host commands the game sends, driven by a
// scripted game side against a scripted remote player (a bare NetplayClient on 127.0.0.1).
// Covers the lobby, the match state reply, the per-frame input exchange, savestate commands, a
// rematch on the open connection and a disconnect.
// Run: powershell -File switch\slippi\tests\run-tests.ps1
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include "perf_switch.h"
#include "rollback_switch.h"
#include "slippi_host.h"
#include "slippi_net.h"

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            ++g_passed;                                                 \
        } else {                                                        \
            ++g_failed;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

// ---- what the game build provides ----
static std::atomic<std::uint32_t> g_retrace{0};
static int g_captures = 0, g_loads = 0, g_resets = 0;
static std::int32_t g_lastCapture = -1;
struct PadReport {
    std::uint16_t button;
    std::int8_t sticks[4];
    std::uint8_t analog[4];
    std::int8_t err;
};
extern "C" std::uint32_t VIGetRetraceCount(void) { return g_retrace; }
extern "C" void VIWaitForRetrace(void) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ++g_retrace;
}
extern "C" std::uint32_t PADRead(PadReport* status) {
    std::memset(status, 0, 4 * sizeof *status);
    return 0;
}
extern "C" int melee_nx_perf_int(const char*, int fallback) { return fallback; }
namespace melee_nx::rollback {
void SlotsReset(int) { ++g_resets; }
bool SlotCapture(std::int32_t frame) {
    ++g_captures;
    g_lastCapture = frame;
    return true;
}
bool SlotLoad(std::int32_t frame) {
    ++g_loads;
    return frame == g_lastCapture;
}
}  // namespace melee_nx::rollback

using namespace slippi;

template <class F>
static bool WaitFor(F cond, int ms = 10000) {
    for (int t = 0; t < ms; t += 10) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

static std::uint8_t g_reply[4096];
static std::uint32_t g_got = 0;
static int Command(std::uint8_t cmd, const std::vector<std::uint8_t>& payload) {
    return mu_slippi_command(cmd, payload.empty() ? nullptr : payload.data(), static_cast<std::uint32_t>(payload.size()),
                             g_reply, sizeof g_reply, &g_got);
}

static std::vector<std::uint8_t> InputsPayload(std::int32_t frame, std::int32_t finalized, std::uint8_t delay,
                                               std::uint8_t button) {
    std::vector<std::uint8_t> p(25, 0);
    for (int i = 0; i < 4; ++i) {
        p[static_cast<size_t>(i)] = static_cast<std::uint8_t>(frame >> (24 - 8 * i));
        p[static_cast<size_t>(4 + i)] = static_cast<std::uint8_t>(finalized >> (24 - 8 * i));
    }
    p[12] = delay;
    p[14] = button;
    return p;
}

enum { kRxbSize = 2 + 8 * 3 + 4 * 3 + 4 + 12 * 7 * 3 + 3, kRxbFrames = 2 + 8 * 3, kRxbInputs = kRxbFrames + 4 * 3 + 4 };

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    {
        std::ofstream out(dir + "/user.json", std::ios::binary | std::ios::trunc);
        out << R"({"uid":"u1","playKey":"k1","connectCode":"HOST#1","displayName":"Host","latestVersion":""})";
    }
    if (!EnetReady()) {
        std::printf("ENet failed to start\n");
        return 1;
    }
    melee_nx_slippi_test_setup(dir.c_str(), 0, 51720, "127.0.0.1:51721", "peer#002");
    CHECK(melee_nx_slippi_enabled() == 1);
    CHECK(Command(0xB9, {}) == 0 && g_got > 1 && g_reply[0] == 1);  // online status: signed in
    CHECK(Command(0x7F, {}) == -1);                                 // unknown command
    CHECK(Command(0xB0, {1, 2, 3}) == -2);                          // wrong payload size

    // The remote player: connects, then picks Fox without picking a stage.
    NetplayClient peer({"127.0.0.1"}, {51720}, 1, 51721, false, 1);
    PlayerSelections pick;
    pick.characterId = 2;
    pick.characterColor = 1;
    pick.isCharacterSelected = true;
    std::thread picker([&] {
        WaitFor([&] { return peer.GetStatus() == NetplayClient::Status::Connected; });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        peer.SetMatchSelections(pick);
    });

    static std::uint8_t state[4096];
    CHECK(melee_nx_slippi_lobby(0x14, 3, 0x1F, 0, state, sizeof state) == 1);
    picker.join();
    const std::uint8_t* block = state + 598;
    CHECK(state[0] == 4 && state[1] == 1 && state[2] == 1);        // connected, both ready
    CHECK(state[3] == 0 && state[4] == 1);                         // the decider is port 1
    CHECK(block[0x60] == 0x14 && block[0x63] == 3);                // our pick
    CHECK(block[0x60 + 0x24] == 2 && block[0x63 + 0x24] == 1);     // theirs
    CHECK(block[0xE] == 0 && block[0xF] == 0x1F);                  // the stage we picked
    CHECK(block[0x61] == 0 && block[0x61 + 0x24] == 0 && block[0x61 + 0x48] == 3);  // two humans
    CHECK(std::strcmp(melee_nx_slippi_status_text(), "") == 0);
    CHECK(WaitFor([&] { return peer.GetMatchInfo().remote[0].isStageSelected; }));
    CHECK(peer.GetMatchInfo().remote[0].characterId == 0x14 && peer.GetMatchInfo().remote[0].stageId == 0x1F);

    // The match: both sides send a pad per frame; the host's reply carries the remote ones.
    const std::uint8_t delay = state[9];
    CHECK(delay >= 1 && delay <= 9);
    peer.StartGame();
    bool sawRemote = false;
    int skips = 0;
    for (std::int32_t frame = 1; frame <= 60;) {
        std::uint8_t pad[kPadDataSize] = {0, static_cast<std::uint8_t>(frame & 0x7F), 0, 0, 0, 0, 0, 0};
        peer.SendPad(frame + delay, pad);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        CHECK(Command(0xB0, InputsPayload(frame, frame > 8 ? frame - 8 : 0, delay, 0x01)) == 0);
        if (g_got != kRxbSize) {
            CHECK(g_got == kRxbSize);
            break;
        }
        if (g_reply[0] == 2) {  // waiting for the other game: the same frame again
            if (++skips > 2000) break;
            continue;
        }
        CHECK(g_reply[0] == 1 || g_reply[0] == 4);
        CHECK(g_reply[1] == 1);
        const std::int32_t latest = static_cast<std::int32_t>((g_reply[kRxbFrames] << 24) | (g_reply[kRxbFrames + 1] << 16) |
                                                               (g_reply[kRxbFrames + 2] << 8) | g_reply[kRxbFrames + 3]);
        CHECK(latest <= frame);
        // Entry 0 is the pad of frame `latest`; the peer put that frame's number in its second byte.
        if (latest > delay && g_reply[kRxbInputs + 1] == static_cast<std::uint8_t>((latest - delay) & 0x7F)) sawRemote = true;
        ++frame;
    }
    CHECK(sawRemote);
    CHECK(g_resets == 1);
    CHECK(WaitFor([&] { return peer.GetRemotePads(0, 4).latestFrame >= 60; }));
    CHECK(peer.GetRemotePads(0, 1).pads.back().data[1] == 0x01);  // our button reached them

    std::vector<std::uint8_t> ss(32, 0);
    ss[3] = 40;
    CHECK(Command(0xB1, ss) == 0 && g_captures == 1 && g_lastCapture == 40);
    CHECK(Command(0xB2, ss) == 0 && g_loads == 1);

    // Next game on the same connection: no new search.
    pick.characterId = 9;
    std::thread repick([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        peer.StartGame();
        peer.SetMatchSelections(pick);
    });
    CHECK(Command(0xB0, InputsPayload(1, 0, delay, 0)) == 0);  // the next game's first frame
    CHECK(melee_nx_slippi_lobby(0x14, 0, 0x20, 0, state, sizeof state) == 1);
    repick.join();
    CHECK(block[0x60 + 0x24] == 9 && block[0xF] == 0x20);

    // They leave: the inputs reply is the one-byte "disconnected".
    peer.ForceDisconnect();
    CHECK(WaitFor([&] {
        Command(0xB0, InputsPayload(2, 0, delay, 0));
        return g_got == 1 && g_reply[0] == 3;
    }));
    Command(0xBA, {});  // cleanup

    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
