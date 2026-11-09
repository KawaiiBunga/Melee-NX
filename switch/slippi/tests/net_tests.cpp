// Loopback tests for switch/slippi/slippi_net: two netplay clients on 127.0.0.1 exchange
// selections, pads, acks and chat; then two matchmaking instances pair through local peering.
// Run: powershell -File switch\slippi\tests\run-tests.ps1
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

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

using namespace slippi;

template <class F>
static bool WaitFor(F cond, int ms = 10000) {
    for (int t = 0; t < ms; t += 10) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

static void Quiet(const char*) {}

static void TestNetplayPair() {
    NetplayClient a({"127.0.0.1"}, {51700}, 1, 51701, true, 0);
    NetplayClient b({"127.0.0.1"}, {51701}, 1, 51700, false, 1);
    CHECK(WaitFor([&] { return a.GetStatus() == NetplayClient::Status::Connected &&
                               b.GetStatus() == NetplayClient::Status::Connected; }));

    PlayerSelections sel;
    sel.characterId = 0x14;
    sel.characterColor = 1;
    sel.isCharacterSelected = true;
    sel.stageId = 0x1F;
    sel.isStageSelected = true;
    sel.rngOffset = 1234;
    a.SetMatchSelections(sel);
    CHECK(WaitFor([&] { return b.GetMatchInfo().remote[0].isStageSelected; }));
    const MatchInfo mi = b.GetMatchInfo();
    CHECK(mi.remote[0].characterId == 0x14 && mi.remote[0].stageId == 0x1F && mi.remote[0].rngOffset == 1234);

    a.StartGame();
    b.StartGame();
    for (int f = 1; f <= 20; ++f) {
        std::uint8_t pad[kPadDataSize];
        for (int i = 0; i < kPadDataSize; ++i) pad[i] = static_cast<std::uint8_t>(f + i);
        a.SendPad(f, pad, f - 1, 0xABC000u + f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(WaitFor([&] { return b.GetRemotePads(0, 64).latestFrame == 20; }));
    const RemotePads rp = b.GetRemotePads(0, 64);
    CHECK(rp.pads.size() == 20 && rp.pads.front().frame == 1 && rp.pads.back().frame == 20);
    CHECK(rp.pads.back().data[0] == 20 && rp.pads.back().data[7] == 27);
    CHECK(rp.checksumFrame == 19 && rp.checksum == 0xABC000u + 20);
    CHECK(rp.playerIdx == 0 && !rp.disconnected);
    b.DropOldRemoteInputs(15);
    CHECK(b.GetRemotePads(0, 64).pads.front().frame == 15);

    // b acks a's pads, so a measures a ping and stops resending acked frames.
    CHECK(WaitFor([&] { return a.LastPingMs() >= 0 && a.GetRemotePads(0, 1).latestFrame == 0; }));
    std::uint8_t pad[kPadDataSize] = {9, 9, 9, 9, 9, 9, 9, 9};
    b.SendPad(1, pad);
    CHECK(WaitFor([&] { return a.GetRemotePads(0, 4).latestFrame == 1; }));

    a.SendChatMessage(136);
    std::uint8_t from = 0;
    CHECK(WaitFor([&] { return b.TakeRemoteChatMessage(&from) == 136; }));
    CHECK(from == 0);

    a.ForceDisconnect();
    CHECK(WaitFor([&] { return b.GetRemotePads(0, 1).disconnected; }));
}

static void TestNetplayTimeout() {
    // Nobody listens on the remote port: the client gives up after its 8-second window.
    NetplayClient lone({"127.0.0.1"}, {51790}, 1, 51791, true, 0);
    CHECK(WaitFor([&] { return lone.GetStatus() == NetplayClient::Status::Failed; }, 12000));
}

static void TestLocalPeerMatchmaking() {
    Account me1{"u1", "k1", "ONE#1", "One"}, me2{"u2", "k2", "TWO#2", "Two"};
    Matchmaking::LocalPeer p1{true, 0, 51710, {"127.0.0.1:51711"}};
    Matchmaking::LocalPeer p2{true, 1, 51711, {"127.0.0.1:51710"}};
    Matchmaking m1(me1, "", p1), m2(me2, "", p2);
    m1.FindMatch({Mode::Direct, "TWO#2"});
    m2.FindMatch({Mode::Direct, "ONE#1"});
    CHECK(WaitFor([&] { return m1.GetState() == Matchmaking::State::ConnectionSuccess &&
                               m2.GetState() == Matchmaking::State::ConnectionSuccess; }, 15000));
    auto c1 = m1.TakeNetplayClient();
    auto c2 = m2.TakeNetplayClient();
    CHECK(c1 && c2 && c1->IsDecider() && !c2->IsDecider());
    CHECK(m1.GetTicket().players.size() == 2 && m1.GetTicket().localPlayerIndex == 0);
    if (c1 && c2) {
        std::uint8_t pad[kPadDataSize] = {1, 2, 3, 4, 5, 6, 7, 8};
        c2->SendPad(1, pad);
        CHECK(WaitFor([&] { return c1->GetRemotePads(0, 4).latestFrame == 1; }));
    }
}

static void TestVersionGate() {
    CHECK(VersionNewer("3.6.5", "3.6.4") && VersionNewer("4.0.0", "3.9.9") && !VersionNewer("3.6.4", "3.6.4"));
    CHECK(!VersionNewer("", "3.6.4") && !VersionNewer("junk", "3.6.4"));
    Matchmaking m(Account{"u", "k", "A#1", "A"}, "9.0.0");
    m.FindMatch({Mode::Unranked, ""});
    CHECK(m.GetState() == Matchmaking::State::Error && m.Error().find("9.0.0") != std::string::npos);
}

int main() {
    SetLogger(Quiet);
    if (!EnetReady()) {
        std::printf("ENet failed to start\n");
        return 1;
    }
    TestVersionGate();
    TestNetplayPair();
    TestLocalPeerMatchmaking();
    TestNetplayTimeout();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
