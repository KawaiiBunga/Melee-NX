// SPDX-License-Identifier: GPL-3.0-or-later
// Slippi Online wire formats, with no sockets, threads or platform calls, so they build for the
// Switch and run in host tests.
//
// Ported from Melee Unlocked's port/runtime/hle/slippi_net.{h,cpp} and slippi_netplay_receive.h
// (https://github.com/Hero88go/melee-unlocked, GPL-2.0-or-later, inspected at 6d5e2e4b), which
// port Dolphin's SlippiNetplay / SlippiMatchmaking. The formats must stay byte-for-byte compatible
// with Slippi Dolphin. Removed here: YouGame, early inputs, build/ping extensions, and the
// Windows string conversions. See docs/SLIPPI.md for what still has to be ported around this.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace slippi {

// The netplay version melee-unlocked speaks to Slippi's servers. Matchmaking refuses an
// outdated version, so this must follow upstream releases.
inline constexpr const char* kSlippiSemver = "3.6.4";
inline constexpr const char* kMatchmakingHost = "mm.slippi.gg";
inline constexpr std::uint16_t kMatchmakingPort = 43113;

inline constexpr int kRollbackMaxFrames = 7;
inline constexpr int kRemotePlayerMax = 3;
inline constexpr int kPlayerCountMax = kRemotePlayerMax + 1;
inline constexpr int kPadDataSize = 8;
inline constexpr int kPadHeaderSize = 14;  // id, frame, port, checksum frame, checksum
inline constexpr int kMaxPadsPerPacket = 128;
inline constexpr std::uint8_t kChatDisabled = 0x10;

enum NetMsg : std::uint8_t {
    kMsgPad = 0x80,
    kMsgPadAck = 0x81,
    kMsgMatchSelections = 0x82,
    kMsgConnSelected = 0x83,
    kMsgChatMessage = 0x84,
    kMsgCompleteStep = 0x85,
    kMsgSyncedState = 0x86,
};

// sf::Packet's wire format: integers big-endian, bool as one byte, strings as u32 length + bytes.
// Reads past the end set ok() false and leave the target untouched.
class Packet {
public:
    Packet() = default;
    Packet(const std::uint8_t* data, size_t size) : buf_(data, data + size) {}
    void Append(const void* data, size_t size);
    const std::uint8_t* Data() const { return buf_.data(); }
    size_t Size() const { return buf_.size(); }
    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    Packet& operator<<(std::uint8_t v);
    Packet& operator<<(bool v) { return *this << static_cast<std::uint8_t>(v ? 1 : 0); }
    Packet& operator<<(std::uint16_t v);
    Packet& operator<<(std::uint32_t v);
    Packet& operator<<(std::int32_t v) { return *this << static_cast<std::uint32_t>(v); }
    Packet& operator<<(const std::string& s);
    Packet& operator>>(std::uint8_t& v);
    Packet& operator>>(bool& v);
    Packet& operator>>(std::uint16_t& v);
    Packet& operator>>(std::uint32_t& v);
    Packet& operator>>(std::int32_t& v);
    Packet& operator>>(std::string& s);

private:
    bool Check(size_t n);
    std::vector<std::uint8_t> buf_;
    size_t pos_ = 0;
    bool ok_ = true;
};

struct Pad {
    std::int32_t frame = 0;
    std::uint8_t data[kPadDataSize] = {};
};

// A pad message: the newest frame first, then older frames back to the oldest un-acked one.
// `pads` is ordered newest first, as Slippi Dolphin keeps its local queue.
Packet EncodePads(std::uint8_t localPort, std::int32_t checksumFrame, std::uint32_t checksum,
                  const std::deque<Pad>& pads);

struct PadHeader {
    std::int32_t frame = 0;
    std::uint8_t port = 0;
    std::int32_t checksumFrame = 0;
    std::uint32_t checksum = 0;
};
enum class PadError { None, BadHeader, TooSmall, TooMany };
// Reads a whole pad message (id included) and pushes the frames newer than `queue`'s front onto it,
// keeping it newest first. Returns how many were added (0 when nothing new).
PadError DecodePads(const std::uint8_t* data, size_t size, PadHeader& header, std::deque<Pad>& queue, int* added);

Packet EncodePadAck(std::int32_t frame, std::uint8_t localPort);

struct PlayerSelections {
    std::uint8_t playerIdx = 0, characterId = 0, characterColor = 0, teamId = 0;
    bool isCharacterSelected = false;
    std::uint16_t stageId = 0;
    bool isStageSelected = false;
    std::uint8_t altStageMode = 0;
    std::uint32_t rngOffset = 0;
};
Packet EncodeSelections(const PlayerSelections& s);
// `p` is positioned after the message id.
bool DecodeSelections(Packet& p, PlayerSelections& out);
// Chat message ids the game's quick chat can send (plus "chat disabled").
bool IsAllowedChatMessage(int id);

// The local player's account as matchmaking needs it.
struct Account {
    std::string uid, playKey, connectCode, displayName;
};
enum class Mode : int { Ranked = 0, Unranked = 1, Direct = 2, Teams = 3, Party = 4 };
// The create-ticket request sent to mm.slippi.gg. It carries the play key: never log it.
std::string CreateTicketRequest(const Account& me, Mode mode, const std::string& directCode,
                                const std::string& lanAddress);

struct Player {
    std::string uid, displayName, connectCode;
    int port = 0;  // 1..4
    bool isBot = false;
};
// A get-ticket-resp, read without trusting any of it: ok only with 2..4 players, every port 1..4
// used once, and exactly one local player, so no count or index from it can overrun the fixed
// per-player arrays.
struct Ticket {
    bool ok = false;
    std::string error;
    std::string matchId;
    std::vector<Player> players;
    int localPlayerIndex = 0;            // the local player's port - 1
    std::vector<std::string> remoteIps;  // "ip:port" per remote (LAN address when behind the same IP)
    bool isHost = false;
    std::vector<std::uint16_t> stages;
    std::uint32_t items = 0;
    std::string latestVersion;  // set when the server refused the version
    std::string serverError;    // "error" field
};
Ticket ParseTicket(const std::string& jsonText);

// "ABC#123" as the game's name-tag text: '#' becomes the Shift-JIS full-width sharp, padded to 10.
std::string ConnectCodeForGame(const std::string& code);

}  // namespace slippi
