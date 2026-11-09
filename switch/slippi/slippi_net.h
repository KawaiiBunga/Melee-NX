// SPDX-License-Identifier: GPL-3.0-or-later
// Slippi Online network clients: the peer-to-peer netplay client (pad exchange, acks, time sync,
// selections, chat) and the matchmaking client (mm.slippi.gg, or local peering for tests).
//
// Ported from Melee Unlocked's port/runtime/hle/slippi_net.{h,cpp} (GPL-2.0-or-later, inspected at
// 6d5e2e4b), itself a port of Dolphin's SlippiNetplay / SlippiMatchmaking. Wire formats come from
// slippi_protocol.h. Left out: YouGame, early inputs, the donor's build/ping messages, the
// session version check over HTTP, network-lag simulation and test hooks. See docs/SLIPPI.md.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "slippi_protocol.h"

struct _ENetHost;
struct _ENetPeer;

namespace slippi {

// Where log lines go (the game routes them to its runtime log). Never passed the play key.
using LogFn = void (*)(const char* line);
void SetLogger(LogFn fn);

bool EnetReady();  // initialises ENet once
std::uint64_t TimeUs();
std::uint64_t TimeMs();

// "3.6.4" > "3.6.3"; empty or malformed strings are never newer.
bool VersionNewer(const std::string& a, const std::string& b);

struct MatchInfo {
    PlayerSelections local;
    PlayerSelections remote[kRemotePlayerMax];
};

struct RemotePads {
    std::int32_t latestFrame = 0;
    std::int32_t checksumFrame = 0;
    std::uint32_t checksum = 0;
    std::uint8_t playerIdx = 0;
    bool disconnected = false;
    std::vector<Pad> pads;  // oldest first
};

class NetplayClient {
public:
    enum class Status { Unset, Initiated, Connected, Failed, Disconnected };

    // `addrs`/`ports`: one per remote player, in player-index order with the local player left out.
    NetplayClient(std::vector<std::string> addrs, std::vector<std::uint16_t> ports, std::uint8_t remoteCount,
                  std::uint16_t localPort, bool isDecider, std::uint8_t playerIdx);
    ~NetplayClient();
    NetplayClient(const NetplayClient&) = delete;
    NetplayClient& operator=(const NetplayClient&) = delete;

    Status GetStatus() const { return status_.load(std::memory_order_acquire); }
    bool IsDecider() const { return isDecider_; }
    std::uint8_t LocalPlayerIdx() const { return playerIdx_; }

    // Game thread.
    void StartGame();
    void SendPad(std::int32_t frame, const std::uint8_t data[kPadDataSize], std::int32_t checksumFrame = 0,
                 std::uint32_t checksum = 0);
    // A skipped (time-sync) frame: resend the unacked pads without adding one, as Slippi does.
    void ResendPads();
    int ActiveRemoteCount() const;
    RemotePads GetRemotePads(int remoteIndex, int maxFrames);
    void DropOldRemoteInputs(std::int32_t finalizedFrame);
    void SetMatchSelections(const PlayerSelections& s);
    MatchInfo GetMatchInfo();
    void SendChatMessage(int messageId);
    int TakeRemoteChatMessage(std::uint8_t* fromPlayer);  // 0 when none
    std::int32_t CalcTimeOffsetUs();
    int LastPingMs() const { return static_cast<int>(lastPingMs_.load(std::memory_order_relaxed)); }
    void ForceDisconnect();

private:
    struct FrameTiming {
        std::int32_t frame = 0;
        std::uint64_t timeUs = 0;
    };
    struct Connection {
        std::uint8_t playerIdx = 0;
        bool disconnected = false;
    };

    std::uint8_t RemoteIndexFromPort(std::uint8_t port) const { return port > playerIdx_ ? port - 1 : port; }
    void ThreadFunc();
    void OnData(const std::uint8_t* data, size_t size, _ENetPeer* peer);
    void Send(const Packet& p);
    void SendAsync(Packet p);
    void Disconnect();
    bool AllPeersGoneForKey(const std::string& key);
    bool AllConnectionsGone();

    _ENetHost* host_ = nullptr;
    std::vector<_ENetPeer*> peers_;
    std::thread thread_;
    std::atomic<bool> loop_{true};
    std::atomic<Status> status_{Status::Unset};
    bool isDecider_ = false;
    std::uint8_t playerIdx_ = 0;
    std::uint8_t remoteCount_ = 0;

    // Lock order when more than one is taken: selection_, timing_, pad_. async_ is a leaf.
    std::mutex pad_, ack_, async_, timing_, chat_, selection_;
    std::deque<Pad> localPads_;  // game thread only; newest first
    std::int32_t localChecksumFrame_ = 0;
    std::uint32_t localChecksum_ = 0;
    std::deque<Pad> remotePads_[kRemotePlayerMax];  // pad_; newest first
    struct Checksum {
        std::int32_t frame = 0;
        std::uint32_t value = 0;
    } remoteChecksums_[kRemotePlayerMax];  // pad_
    std::int32_t lastFrameAcked_[kRemotePlayerMax] = {};  // ack_
    std::deque<FrameTiming> ackTimers_[kRemotePlayerMax];  // ack_
    std::uint64_t pingUs_[kRemotePlayerMax] = {};
    std::atomic<std::uint32_t> lastPingMs_{0};
    FrameTiming lastFrameTiming_[kRemotePlayerMax];  // timing_
    std::vector<std::int32_t> frameOffsets_[kRemotePlayerMax];  // timing_
    int frameOffsetIdx_[kRemotePlayerMax] = {};  // timing_
    bool gameStarted_ = false;  // timing_
    MatchInfo matchInfo_;  // selection_
    int remoteChatId_ = 0;  // chat_
    std::uint8_t remoteChatFrom_ = 0;  // chat_
    std::deque<Packet> asyncQueue_;  // async_
    std::unordered_map<std::string, std::map<_ENetPeer*, Connection>> connections_;  // network thread
    std::atomic<bool> playerActive_[kPlayerCountMax] = {};
};

// Local peering (no server), for tests and LAN play between melee-nx consoles: fixed player index,
// local port and every other player's "ip:port" in player-index order.
struct LocalPeering {
    bool enabled = false;
    int localIndex = 0;
    std::uint16_t localPort = 0;
    std::vector<std::string> remotes;
};

class Matchmaking {
public:
    using LocalPeer = LocalPeering;
    enum class State { Idle, Initializing, Matchmaking, OpponentConnecting, ConnectionSuccess, Error };
    struct Search {
        Mode mode = Mode::Unranked;
        std::string connectCode;  // Direct: the opponent's code, as the game's text
    };
    // `latestVersion`: from user.json; a server version newer than kSlippiSemver blocks the search.
    Matchmaking(Account me, std::string latestVersion, LocalPeer local = {});
    ~Matchmaking();
    Matchmaking(const Matchmaking&) = delete;
    Matchmaking& operator=(const Matchmaking&) = delete;

    void FindMatch(Search search);
    State GetState() const { return state_.load(); }
    bool IsSearching() const;
    std::string Error() const;
    // After ConnectionSuccess: the connected client (moves it out), players and stages.
    std::unique_ptr<NetplayClient> TakeNetplayClient();
    Ticket GetTicket() const;

private:
    void Thread();
    void StartMatchmaking();
    void HandleMatchmaking();
    void HandleConnecting();
    bool Ingest(const Ticket& t);
    void Fail(const std::string& message);
    void DisconnectFromServer();

    Account me_;
    std::string latestVersion_;
    LocalPeer local_;
    Search search_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> terminated_{false};
    std::thread thread_;
    _ENetHost* host_ = nullptr;  // matchmaking thread
    _ENetPeer* server_ = nullptr;
    std::uint16_t hostPort_ = 0;
    mutable std::mutex result_;  // leaf lock: error_, ticket_, client_
    std::string error_;
    Ticket ticket_;
    std::unique_ptr<NetplayClient> client_;
};

}  // namespace slippi
