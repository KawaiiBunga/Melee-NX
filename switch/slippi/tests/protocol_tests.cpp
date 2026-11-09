// Host tests for switch/slippi/slippi_protocol. Run: powershell -File switch\slippi\tests\run-tests.ps1
#include <cstdio>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "slippi_protocol.h"

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

static void TestPacket() {
    Packet p;
    p << std::uint8_t{0xAB} << std::uint16_t{0x1234} << std::uint32_t{0xDEADBEEF} << std::int32_t{-2} << true << std::string("hi");
    const std::uint8_t want[] = {0xAB, 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF, 0xFF, 0xFF, 0xFF, 0xFE, 1, 0, 0, 0, 2, 'h', 'i'};
    CHECK(p.Size() == sizeof want && std::memcmp(p.Data(), want, sizeof want) == 0);
    Packet r(p.Data(), p.Size());
    std::uint8_t a = 0;
    std::uint16_t b = 0;
    std::uint32_t c = 0;
    std::int32_t d = 0;
    bool e = false;
    std::string s;
    CHECK((r >> a >> b >> c >> d >> e >> s) && a == 0xAB && b == 0x1234 && c == 0xDEADBEEF && d == -2 && e && s == "hi");
    std::uint8_t extra = 7;
    CHECK(!(r >> extra) && extra == 7);  // past the end: fails and leaves the value
    // A string length larger than the packet fails instead of reading out of bounds.
    const std::uint8_t lie[] = {0, 0, 1, 0, 'x'};
    Packet l(lie, sizeof lie);
    std::string out;
    CHECK(!(l >> out));
}

static void TestPads() {
    std::deque<Pad> local;
    for (int f = 10; f >= 8; --f) {  // newest first
        Pad pad;
        pad.frame = f;
        for (int i = 0; i < kPadDataSize; ++i) pad.data[i] = static_cast<std::uint8_t>(f * 16 + i);
        local.push_back(pad);
    }
    const Packet msg = EncodePads(1, 6, 0x11223344, local);
    CHECK(msg.Size() == static_cast<size_t>(kPadHeaderSize + 3 * kPadDataSize));
    CHECK(msg.Data()[0] == kMsgPad);

    std::deque<Pad> remote;
    PadHeader h;
    int added = 0;
    // An empty queue counts from frame 0: frames 1..10 are due but the packet holds 3, so it is
    // refused as too small (Slippi Dolphin's rule). The header still parses.
    CHECK(DecodePads(msg.Data(), msg.Size(), h, remote, &added) == PadError::TooSmall);
    CHECK(h.frame == 10 && h.port == 1 && h.checksumFrame == 6 && h.checksum == 0x11223344);
    CHECK(added == 0 && remote.empty());
    remote.clear();
    Pad seed;
    seed.frame = 7;
    remote.push_front(seed);
    CHECK(DecodePads(msg.Data(), msg.Size(), h, remote, &added) == PadError::None);
    CHECK(added == 3 && remote.size() == 4 && remote.front().frame == 10 && remote[1].frame == 9 && remote[2].frame == 8);
    CHECK(remote.front().data[0] == 10 * 16 && remote[2].data[7] == 8 * 16 + 7);
    // Resending the same packet adds nothing.
    CHECK(DecodePads(msg.Data(), msg.Size(), h, remote, &added) == PadError::None && added == 0);
    // Truncated payload, oversize gap and bad header are refused.
    std::deque<Pad> q{seed};
    CHECK(DecodePads(msg.Data(), msg.Size() - 1, h, q, &added) == PadError::TooSmall && q.size() == 1);
    std::deque<Pad> far{Pad{}};
    far.front().frame = -200;
    CHECK(DecodePads(msg.Data(), msg.Size(), h, far, &added) == PadError::TooMany);
    const std::uint8_t junk[] = {kMsgPad, 0, 0};
    CHECK(DecodePads(junk, sizeof junk, h, q, &added) == PadError::BadHeader);
    const Packet ack = EncodePadAck(10, 1);
    const std::uint8_t wantAck[] = {kMsgPadAck, 0, 0, 0, 10, 1};
    CHECK(ack.Size() == sizeof wantAck && std::memcmp(ack.Data(), wantAck, sizeof wantAck) == 0);
}

static void TestSelections() {
    PlayerSelections s;
    s.playerIdx = 1;
    s.characterId = 0x14;
    s.characterColor = 2;
    s.isCharacterSelected = true;
    s.stageId = 0x1F;
    s.isStageSelected = true;
    s.rngOffset = 0xCAFEBABE;
    s.teamId = 1;
    s.altStageMode = 3;
    Packet p = EncodeSelections(s);
    CHECK(p.Size() == 1 + 1 + 1 + 1 + 1 + 2 + 1 + 4 + 1 + 1);
    Packet r(p.Data(), p.Size());
    std::uint8_t id = 0;
    r >> id;
    PlayerSelections o;
    CHECK(id == kMsgMatchSelections && DecodeSelections(r, o));
    CHECK(o.characterId == 0x14 && o.stageId == 0x1F && o.rngOffset == 0xCAFEBABE && o.altStageMode == 3 && o.isStageSelected);
    Packet shortP(p.Data(), p.Size() - 2);
    shortP >> id;
    CHECK(!DecodeSelections(shortP, o));
    CHECK(IsAllowedChatMessage(136) && IsAllowedChatMessage(kChatDisabled) && !IsAllowedChatMessage(1));
}

static void TestMatchmaking() {
    Account me{"uid1", "SECRET", "ME#1", "Me"};
    const auto req = nlohmann::json::parse(CreateTicketRequest(me, Mode::Direct, "OP#2", "192.168.1.5:41000"));
    CHECK(req["type"] == "create-ticket" && req["appVersion"] == kSlippiSemver);
    CHECK(req["user"]["playKey"] == "SECRET" && req["search"]["mode"] == 2);
    CHECK(req["search"]["connectCode"].is_array() && req["search"]["connectCode"].size() == 4 && req["search"]["connectCode"][2] == '#');

    const char* good = R"({"type":"get-ticket-resp","matchId":"m1","isHost":true,"stages":[2,3],
      "players":[{"uid":"a","displayName":"A","connectCode":"A#1","port":1,"isLocalPlayer":true,"ipAddress":"9.9.9.9:1","ipAddressLan":"10.0.0.2:1"},
                 {"uid":"b","displayName":"B","connectCode":"B#2","port":2,"isLocalPlayer":false,"ipAddress":"8.8.8.8:2","ipAddressLan":"10.0.0.3:2"}]})";
    Ticket t = ParseTicket(good);
    CHECK(t.ok && t.matchId == "m1" && t.isHost && t.localPlayerIndex == 0 && t.players.size() == 2);
    CHECK(t.remoteIps.size() == 1 && t.remoteIps[0] == "8.8.8.8:2");  // different external IP: use it
    CHECK(t.stages.size() == 2);

    const char* sameNat = R"({"type":"get-ticket-resp","players":[
      {"port":2,"isLocalPlayer":true,"ipAddress":"9.9.9.9:1"},{"port":1,"ipAddress":"9.9.9.9:2","ipAddressLan":"10.0.0.3:2"}]})";
    t = ParseTicket(sameNat);
    CHECK(t.ok && t.localPlayerIndex == 1 && t.remoteIps[0] == "10.0.0.3:2" && t.stages.size() == 6);

    CHECK(!ParseTicket("nope").ok);
    CHECK(!ParseTicket(R"({"type":"create-ticket-resp"})").ok);
    t = ParseTicket(R"({"type":"get-ticket-resp","error":"outdated","latestVersion":"3.9.9"})");
    CHECK(!t.ok && t.serverError == "outdated" && t.latestVersion == "3.9.9");
    CHECK(!ParseTicket(R"({"type":"get-ticket-resp","players":[{"port":1,"isLocalPlayer":true}]})").ok);  // 1 player
    CHECK(!ParseTicket(R"({"type":"get-ticket-resp","players":[{"port":1,"isLocalPlayer":true},{"port":1}]})").ok);
    CHECK(!ParseTicket(R"({"type":"get-ticket-resp","players":[{"port":1,"isLocalPlayer":true},{"port":9}]})").ok);
    CHECK(!ParseTicket(R"({"type":"get-ticket-resp","players":[{"port":1},{"port":2}]})").ok);  // no local
    CHECK(!ParseTicket(R"({"type":"get-ticket-resp","players":[{"port":1,"isLocalPlayer":true},{"port":2,"isLocalPlayer":true}]})").ok);
    CHECK(!ParseTicket(R"({"type":"get-ticket-resp","players":[{"port":"1","isLocalPlayer":true},{"port":2}]})").ok);  // wrong type
    CHECK(!ParseTicket(R"({"type":"get-ticket-resp","players":[1,2]})").ok);

    const std::string code = ConnectCodeForGame("AB#12");
    CHECK(code.size() == 10 && code[2] == static_cast<char>(0x81) && code[3] == static_cast<char>(0x94) && code[4] == '1');
}

int main() {
    TestPacket();
    TestPads();
    TestSelections();
    TestMatchmaking();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
