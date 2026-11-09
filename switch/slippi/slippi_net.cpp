// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Melee Unlocked's slippi_net.cpp (GPL-2.0-or-later); see slippi_net.h.
#include "slippi_net.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>

#include <enet/enet.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace slippi {
namespace {

using json = nlohmann::json;

std::atomic<LogFn> g_log{nullptr};

void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (LogFn fn = g_log.load()) {
        fn(buf);
    } else {
        std::printf("[slippi] %s\n", buf);
    }
}

// Wakes the network thread out of enet_host_service with a one-byte datagram to itself; the
// intercept callback turns it into a dummy event.
void WakeThread(ENetHost* host) {
    if (!host) return;
    ENetAddress address;
    if (host->address.port != 0)
        address.port = host->address.port;
    else
        enet_socket_get_address(host->socket, &address);
    address.host = 0x0100007f;
    std::uint8_t byte = 0;
    ENetBuffer buf;
    buf.data = &byte;
    buf.dataLength = 1;
    enet_socket_send(host->socket, &address, &buf, 1);
}

int ENET_CALLBACK Intercept(ENetHost* host, ENetEvent* event) {
    if (host->receivedDataLength == 1 && host->receivedData[0] == 0) {
        event->type = static_cast<ENetEventType>(42);
        return 1;
    }
    return 0;
}

std::string PeerKey(ENetPeer* p) {
    std::stringstream s;
    s << p->address.host << "-" << p->address.port;
    return s.str();
}

bool SameAddress(const ENetAddress& a, const ENetAddress& b) { return a.host == b.host && a.port == b.port; }

}  // namespace

void SetLogger(LogFn fn) { g_log.store(fn); }

std::uint64_t TimeUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::uint64_t TimeMs() { return TimeUs() / 1000; }

bool EnetReady() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        ok = enet_initialize() == 0;
        if (!ok) Log("enet_initialize failed");
    });
    return ok;
}

bool VersionNewer(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s, int out[3]) {
        out[0] = out[1] = out[2] = 0;
        return !s.empty() && std::sscanf(s.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]) >= 1;
    };
    int x[3], y[3];
    if (!parse(a, x) || !parse(b, y)) return false;
    for (int i = 0; i < 3; ++i) {
        if (x[i] != y[i]) return x[i] > y[i];
    }
    return false;
}

// ---- NetplayClient ---------------------------------------------------------------------------

NetplayClient::NetplayClient(std::vector<std::string> addrs, std::vector<std::uint16_t> ports, std::uint8_t remoteCount,
                             std::uint16_t localPort, bool isDecider, std::uint8_t playerIdx) {
    isDecider_ = isDecider;
    const size_t usable = std::min<size_t>({static_cast<size_t>(remoteCount), static_cast<size_t>(kRemotePlayerMax),
                                            addrs.size(), ports.size()});
    remoteCount_ = static_cast<std::uint8_t>(usable);
    playerIdx_ = playerIdx;
    Log("netplay: decider %d, player %u, %u remote", isDecider, playerIdx, remoteCount_);
    if (playerIdx >= kPlayerCountMax || remoteCount_ == 0 || !EnetReady()) {
        status_.store(Status::Failed);
        return;
    }
    for (int i = 0, j = 0; i < kRemotePlayerMax; ++i, ++j) {
        if (j == playerIdx) ++j;
        matchInfo_.remote[i] = PlayerSelections();
        matchInfo_.remote[i].playerIdx = static_cast<std::uint8_t>(j);
    }
    ENetAddress local;
    local.host = ENET_HOST_ANY;
    local.port = localPort;
    host_ = enet_host_create(localPort > 0 ? &local : nullptr, 10, 3, 0, 0);
    if (!host_) {
        Log("netplay: cannot create ENet host");
        status_.store(Status::Failed);
        return;
    }
    for (int i = 0; i < remoteCount_; ++i) {
        ENetAddress addr;
        enet_address_set_host(&addr, addrs[static_cast<size_t>(i)].c_str());
        addr.port = ports[static_cast<size_t>(i)];
        ENetPeer* peer = enet_host_connect(host_, &addr, 3, 0);
        peers_.push_back(peer);
        if (!peer) continue;
        Connection c;
        c.playerIdx = matchInfo_.remote[i].playerIdx;
        connections_[PeerKey(peer)][peer] = c;
        playerActive_[c.playerIdx].store(true);
    }
    status_.store(Status::Initiated, std::memory_order_release);
    thread_ = std::thread(&NetplayClient::ThreadFunc, this);
}

NetplayClient::~NetplayClient() {
    loop_.store(false);
    if (host_) WakeThread(host_);
    if (thread_.joinable()) thread_.join();
    if (!peers_.empty()) Disconnect();
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
}

void NetplayClient::OnData(const std::uint8_t* data, size_t size, ENetPeer* peer) {
    Packet p(data, size);
    std::uint8_t id = 0;
    if (!(p >> id)) return;
    switch (id) {
        case kMsgPad: {
            const std::uint64_t now = TimeUs();
            PadHeader h;
            Packet head(data, size);
            std::uint8_t skip = 0;
            if (!(head >> skip >> h.frame >> h.port)) return;
            const std::uint8_t idx = RemoteIndexFromPort(h.port);
            if (idx >= remoteCount_) return;
            // A peer that reconnected on a new connection replaces the old one (Slippi's rule).
            const std::string key = PeerKey(peer);
            int live = 0;
            bool current = false;
            for (auto& c : connections_[key]) {
                if (c.second.disconnected) continue;
                if (c.first == peer) current = true;
                ++live;
            }
            if (current && live > 1 && playerIdx_ < h.port) {
                for (size_t i = 0; i < peers_.size(); ++i) {
                    if (peers_[i] && SameAddress(peer->address, peers_[i]->address)) peers_[i] = peer;
                }
                for (auto& c : connections_[key]) {
                    if (c.first == peer || c.second.disconnected) continue;
                    enet_peer_disconnect(c.first, 0);
                    c.second.disconnected = true;
                }
            }
            {
                std::lock_guard<std::mutex> lk(timing_);
                FrameTiming t = lastFrameTiming_[idx];
                if (!gameStarted_) t = {0, now};
                const std::int64_t opponentSend = static_cast<std::int64_t>(now) - static_cast<std::int64_t>(pingUs_[idx] / 2);
                const std::int64_t frameDiff = 16683 * static_cast<std::int64_t>(t.frame - h.frame);
                const auto offset = static_cast<std::int32_t>(opponentSend - static_cast<std::int64_t>(t.timeUs) + frameDiff);
                auto& buf = frameOffsets_[idx];
                if (static_cast<int>(buf.size()) < 30)
                    buf.push_back(offset);
                else
                    buf[static_cast<size_t>(frameOffsetIdx_[idx])] = offset;
                frameOffsetIdx_[idx] = (frameOffsetIdx_[idx] + 1) % 30;
            }
            int added = 0;
            {
                std::lock_guard<std::mutex> lk(pad_);
                const PadError err = DecodePads(data, size, h, remotePads_[idx], &added);
                if (err != PadError::None) {
                    Log("netplay: bad pad message (%d)", static_cast<int>(err));
                    return;
                }
                remoteChecksums_[idx] = {h.checksumFrame, h.checksum};
            }
            if (added > 0) {
                const Packet ack = EncodePadAck(h.frame, playerIdx_);
                enet_peer_send(peer, 2, enet_packet_create(ack.Data(), ack.Size(), ENET_PACKET_FLAG_UNSEQUENCED));
            }
            break;
        }
        case kMsgPadAck: {
            std::int32_t frame = 0;
            std::uint8_t port = 0;
            if (!(p >> frame >> port)) return;
            const std::uint8_t idx = RemoteIndexFromPort(port);
            if (idx >= remoteCount_) return;
            std::lock_guard<std::mutex> lk(ack_);
            lastFrameAcked_[idx] = std::max(lastFrameAcked_[idx], frame);
            auto& timers = ackTimers_[idx];
            while (!timers.empty() && timers.front().frame < frame) timers.pop_front();
            if (timers.empty() || timers.front().frame != frame) break;
            pingUs_[idx] = TimeUs() - timers.front().timeUs;
            timers.pop_front();
            if (idx == 0) lastPingMs_.store(static_cast<std::uint32_t>(pingUs_[idx] / 1000), std::memory_order_relaxed);
            break;
        }
        case kMsgMatchSelections: {
            PlayerSelections s;
            if (!DecodeSelections(p, s)) return;
            const std::uint8_t idx = RemoteIndexFromPort(s.playerIdx);
            if (idx >= remoteCount_) return;
            std::scoped_lock lock(selection_, timing_, pad_);
            PlayerSelections& merged = matchInfo_.remote[idx];
            merged.rngOffset = s.rngOffset;
            if (s.isStageSelected) {
                merged.stageId = s.stageId;
                merged.isStageSelected = true;
                merged.altStageMode = s.altStageMode;
            }
            if (s.isCharacterSelected) {
                merged.characterId = s.characterId;
                merged.characterColor = s.characterColor;
                merged.teamId = s.teamId;
                merged.isCharacterSelected = true;
            }
            if (gameStarted_) remotePads_[idx].clear();  // a rematch: the previous game's pads are stale
            gameStarted_ = false;
            break;
        }
        case kMsgChatMessage: {
            std::int32_t message = 0;
            std::uint8_t from = 0;
            if (!(p >> message >> from) || !IsAllowedChatMessage(message)) return;
            std::lock_guard<std::mutex> lk(chat_);
            remoteChatId_ = message;
            remoteChatFrom_ = from;
            break;
        }
        default:
            break;  // the donor's own extensions and Ranked messages are ignored, as Slippi Dolphin does
    }
}

void NetplayClient::Send(const Packet& p) {
    for (ENetPeer* peer : peers_) {
        if (!peer) continue;
        auto conn = connections_.find(PeerKey(peer));
        if (conn != connections_.end()) {
            auto it = conn->second.find(peer);
            if (it != conn->second.end() && it->second.disconnected) continue;
        }
        const std::uint8_t id = p.Data()[0];
        const bool fast = id == kMsgPad || id == kMsgPadAck;
        ENetPacket* packet =
            enet_packet_create(p.Data(), p.Size(), fast ? ENET_PACKET_FLAG_UNSEQUENCED : ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, fast ? 1 : 0, packet);
    }
}

void NetplayClient::SendAsync(Packet p) {
    if (GetStatus() == Status::Disconnected) return;
    {
        std::lock_guard<std::mutex> lk(async_);
        asyncQueue_.push_back(std::move(p));
    }
    WakeThread(host_);
}

void NetplayClient::Disconnect() {
    status_.store(Status::Disconnected, std::memory_order_release);
    if (connections_.empty()) return;
    for (auto& c : connections_) {
        for (auto& peer : c.second) enet_peer_disconnect(peer.first, 0);
    }
    ENetEvent ev;
    while (enet_host_service(host_, &ev, 1000) > 0) {
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
    }
    for (auto& c : connections_) {
        for (auto& peer : c.second) enet_peer_reset(peer.first);
    }
    connections_.clear();
    for (auto& a : playerActive_) a.store(false);
    peers_.clear();
}

bool NetplayClient::AllPeersGoneForKey(const std::string& key) {
    auto it = connections_.find(key);
    if (it == connections_.end()) return true;
    for (auto& p : it->second) {
        if (!p.second.disconnected) return false;
    }
    return true;
}

bool NetplayClient::AllConnectionsGone() {
    for (auto& c : connections_) {
        for (auto& p : c.second) {
            if (!p.second.disconnected) return false;
        }
    }
    return true;
}

void NetplayClient::ThreadFunc() {
    const std::uint64_t start = TimeMs();
    std::vector<bool> connected(remoteCount_, false);
    std::vector<ENetAddress> remoteAddrs;
    for (ENetPeer* p : peers_) remoteAddrs.push_back(p ? p->address : ENetAddress{});

    // Connecting: every remote must answer within 8 seconds.
    while (GetStatus() == Status::Initiated) {
        ENetEvent ev;
        if (enet_host_service(host_, &ev, 250) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                if (ev.peer) OnData(ev.packet->data, ev.packet->dataLength, ev.peer);
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_CONNECT && ev.peer) {
                int idx = 0;
                for (int i = 0; i < static_cast<int>(remoteAddrs.size()); ++i) {
                    if (SameAddress(remoteAddrs[static_cast<size_t>(i)], ev.peer->address)) {
                        idx = i;
                        break;
                    }
                }
                Connection c;
                c.playerIdx = matchInfo_.remote[idx].playerIdx;
                connections_[PeerKey(ev.peer)][ev.peer] = c;
                playerActive_[c.playerIdx].store(true);
                // Exact address first: several players can share one host (LAN, local tests).
                int slot = -1;
                for (size_t i = 0; i < peers_.size() && slot < 0; ++i) {
                    if (connected[i] && SameAddress(ev.peer->address, remoteAddrs[i])) {
                        peers_[i] = ev.peer;
                        slot = -2;
                    }
                }
                for (size_t i = 0; i < peers_.size() && slot == -1; ++i) {
                    if (!connected[i] && SameAddress(remoteAddrs[i], ev.peer->address)) slot = static_cast<int>(i);
                }
                for (size_t i = 0; i < peers_.size() && slot == -1; ++i) {
                    if (!connected[i] && remoteAddrs[i].host == ev.peer->address.host) slot = static_cast<int>(i);
                }
                if (slot >= 0) {
                    peers_[static_cast<size_t>(slot)] = ev.peer;
                    connected[static_cast<size_t>(slot)] = true;
                }
            }
        }
        if (std::all_of(connected.begin(), connected.end(), [](bool b) { return b; })) {
            host_->intercept = Intercept;
            Log("netplay: connected");
            status_.store(Status::Connected, std::memory_order_release);
            break;
        }
        if (TimeMs() - start >= 8000 || !loop_.load()) {
            Log("netplay: connection failed");
            status_.store(Status::Failed, std::memory_order_release);
            return;
        }
    }

    while (loop_.load() && GetStatus() != Status::Disconnected) {
        for (auto& c : connections_) {
            for (auto& peer : c.second) {
                if (peer.second.disconnected || playerActive_[peer.second.playerIdx].load()) continue;
                enet_peer_disconnect(peer.first, 0);
                peer.second.disconnected = true;
            }
        }
        ENetEvent ev;
        const int net = enet_host_service(host_, &ev, 250);
        for (;;) {
            Packet out;
            {
                std::lock_guard<std::mutex> lk(async_);
                if (asyncQueue_.empty()) break;
                out = std::move(asyncQueue_.front());
                asyncQueue_.pop_front();
            }
            Send(out);
        }
        if (net <= 0) continue;
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            OnData(ev.packet->data, ev.packet->dataLength, ev.peer);
            enet_packet_destroy(ev.packet);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            const std::string key = PeerKey(ev.peer);
            auto it = connections_.find(key);
            if (it != connections_.end() && it->second.count(ev.peer)) it->second[ev.peer].disconnected = true;
            const bool gone = AllPeersGoneForKey(key);
            if (gone && it != connections_.end() && it->second.count(ev.peer))
                playerActive_[it->second[ev.peer].playerIdx].store(false);
            bool ours = false;
            for (ENetPeer* p : peers_) ours = ours || (p && SameAddress(ev.peer->address, p->address));
            Log("netplay: peer disconnected");
            if (ours && gone && AllConnectionsGone()) loop_.store(false);
        } else if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            int idx = 0;
            for (int i = 0; i < static_cast<int>(peers_.size()); ++i) {
                if (peers_[static_cast<size_t>(i)] && SameAddress(peers_[static_cast<size_t>(i)]->address, ev.peer->address)) idx = i;
            }
            Connection c;
            c.playerIdx = matchInfo_.remote[idx].playerIdx;
            connections_[PeerKey(ev.peer)][ev.peer] = c;
            playerActive_[c.playerIdx].store(true);
        }
    }
    Disconnect();
}

void NetplayClient::StartGame() {
    localPads_.clear();
    {
        std::lock_guard<std::mutex> lk(timing_);
        gameStarted_ = false;
        for (int i = 0; i < remoteCount_; ++i) lastFrameTiming_[i] = {0, TimeUs()};
    }
    {
        std::lock_guard<std::mutex> lk(ack_);
        for (int i = 0; i < remoteCount_; ++i) {
            lastFrameAcked_[i] = 0;
            ackTimers_[i].clear();
        }
    }
    std::lock_guard<std::mutex> lk(selection_);
    const MatchInfo fresh{};
    for (int i = 0; i < kRemotePlayerMax; ++i) {
        const std::uint8_t keep = matchInfo_.remote[i].playerIdx;
        matchInfo_.remote[i] = fresh.remote[i];
        matchInfo_.remote[i].playerIdx = keep;
    }
    matchInfo_.local = PlayerSelections();
}

void NetplayClient::SendPad(std::int32_t frame, const std::uint8_t data[kPadDataSize], std::int32_t checksumFrame,
                            std::uint32_t checksum) {
    const Status st = GetStatus();
    if (st == Status::Failed || st == Status::Disconnected) return;
    Pad pad;
    pad.frame = frame;
    std::memcpy(pad.data, data, kPadDataSize);
    localPads_.push_front(pad);
    localChecksumFrame_ = checksumFrame;
    localChecksum_ = checksum;
    // Resend everything the slowest active peer has not acked, but never more than 128 frames.
    int minAck = INT_MAX;
    {
        std::lock_guard<std::mutex> lk(ack_);
        for (int i = 0; i < remoteCount_; ++i) {
            if (playerActive_[matchInfo_.remote[i].playerIdx].load()) minAck = std::min(minAck, lastFrameAcked_[i]);
        }
    }
    minAck = std::max(minAck == INT_MAX ? frame : minAck, frame - kMaxPadsPerPacket + 1);
    while (!localPads_.empty() && localPads_.back().frame <= minAck - 1) localPads_.pop_back();
    SendAsync(EncodePads(playerIdx_, localChecksumFrame_, localChecksum_, localPads_));
    const std::uint64_t t = TimeUs();
    {
        std::lock_guard<std::mutex> lk(timing_);
        gameStarted_ = true;
        for (int i = 0; i < remoteCount_; ++i) lastFrameTiming_[i] = {frame, t};
    }
    std::lock_guard<std::mutex> lk(ack_);
    for (int i = 0; i < remoteCount_; ++i) ackTimers_[i].push_back({frame, t});
}

void NetplayClient::ResendPads() {
    const Status st = GetStatus();
    if (st == Status::Failed || st == Status::Disconnected || localPads_.empty()) return;
    SendAsync(EncodePads(playerIdx_, localChecksumFrame_, localChecksum_, localPads_));
}

int NetplayClient::ActiveRemoteCount() const {
    int n = 0;
    for (int i = 0; i < remoteCount_; ++i) n += playerActive_[matchInfo_.remote[i].playerIdx].load() ? 1 : 0;
    return n;
}

RemotePads NetplayClient::GetRemotePads(int remoteIndex, int maxFrames) {
    RemotePads out;
    std::lock_guard<std::mutex> lk(pad_);
    if (remoteIndex < 0 || remoteIndex >= remoteCount_) return out;
    const auto& q = remotePads_[remoteIndex];
    out.checksumFrame = remoteChecksums_[remoteIndex].frame;
    out.checksum = remoteChecksums_[remoteIndex].value;
    out.playerIdx = static_cast<std::uint8_t>(remoteIndex >= playerIdx_ ? remoteIndex + 1 : remoteIndex);
    out.disconnected = !playerActive_[out.playerIdx].load();
    if (q.empty()) return out;
    out.latestFrame = q.front().frame;
    const int n = std::min(maxFrames, static_cast<int>(q.size()));
    for (int i = n - 1; i >= 0; --i) out.pads.push_back(q[static_cast<size_t>(i)]);
    return out;
}

void NetplayClient::DropOldRemoteInputs(std::int32_t finalizedFrame) {
    std::lock_guard<std::mutex> lk(pad_);
    for (int i = 0; i < remoteCount_; ++i) {
        while (remotePads_[i].size() > 1 && remotePads_[i].back().frame < finalizedFrame) remotePads_[i].pop_back();
    }
}

void NetplayClient::SetMatchSelections(const PlayerSelections& s) {
    Packet p;
    {
        std::lock_guard<std::mutex> lk(selection_);
        PlayerSelections& local = matchInfo_.local;
        local.rngOffset = s.rngOffset;
        if (s.isStageSelected) {
            local.stageId = s.stageId;
            local.isStageSelected = true;
            local.altStageMode = s.altStageMode;
        }
        if (s.isCharacterSelected) {
            local.characterId = s.characterId;
            local.characterColor = s.characterColor;
            local.teamId = s.teamId;
            local.isCharacterSelected = true;
        }
        local.playerIdx = playerIdx_;
        p = EncodeSelections(local);
    }
    SendAsync(std::move(p));
}

MatchInfo NetplayClient::GetMatchInfo() {
    std::lock_guard<std::mutex> lk(selection_);
    return matchInfo_;
}

void NetplayClient::SendChatMessage(int messageId) {
    Packet p;
    p << static_cast<std::uint8_t>(kMsgChatMessage) << static_cast<std::int32_t>(messageId) << playerIdx_;
    SendAsync(std::move(p));
}

int NetplayClient::TakeRemoteChatMessage(std::uint8_t* fromPlayer) {
    std::lock_guard<std::mutex> lk(chat_);
    const int id = remoteChatId_;
    if (fromPlayer) *fromPlayer = remoteChatFrom_;
    remoteChatId_ = 0;
    return id;
}

std::int32_t NetplayClient::CalcTimeOffsetUs() {
    std::vector<int> offsets;
    for (int i = 0; i < remoteCount_; ++i) {
        if (!playerActive_[matchInfo_.remote[i].playerIdx].load()) continue;
        std::vector<std::int32_t> buf;
        {
            std::lock_guard<std::mutex> lk(timing_);
            buf = frameOffsets_[i];
        }
        if (buf.empty()) continue;
        std::sort(buf.begin(), buf.end());
        // The middle third, as Slippi Dolphin averages it.
        const int n = static_cast<int>(buf.size()), off = n / 3, end = n - off;
        if (end <= off) return 0;
        long long sum = 0;
        for (int k = off; k < end; ++k) sum += buf[static_cast<size_t>(k)];
        offsets.push_back(static_cast<int>(sum / (end - off)));
    }
    return offsets.empty() ? 0 : *std::min_element(offsets.begin(), offsets.end());
}

void NetplayClient::ForceDisconnect() {
    for (int i = 0; i < remoteCount_; ++i) playerActive_[matchInfo_.remote[i].playerIdx].store(false);
    if (GetStatus() == Status::Connected) status_.store(Status::Disconnected, std::memory_order_release);
    if (host_) WakeThread(host_);
}

// ---- Matchmaking -----------------------------------------------------------------------------

namespace {

int MmSend(ENetPeer* server, const std::string& text) {
    return enet_peer_send(server, 0, enet_packet_create(text.data(), text.size(), ENET_PACKET_FLAG_RELIABLE));
}

// 0: got a message; -1: nothing yet; -2: server left.
int MmReceive(ENetHost* host, std::string& out, int timeoutMs) {
    for (int waited = 0; waited < std::max(timeoutMs, 250); waited += 250) {
        ENetEvent ev;
        if (enet_host_service(host, &ev, 250) <= 0) continue;
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            out.assign(reinterpret_cast<const char*>(ev.packet->data), ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
            return 0;
        }
        if (ev.type == ENET_EVENT_TYPE_DISCONNECT) return -2;
    }
    return -1;
}

std::string LocalAddress(ENetAddress* server) {
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) return {};
    ENetAddress a;
    std::string out;
    if (enet_socket_connect(s, server) != -1 && enet_socket_get_address(s, &a) != -1) {
        in_addr in;
        in.s_addr = a.host;
        char buf[32] = {};
        inet_ntop(AF_INET, &in, buf, sizeof buf);
        out = buf;
    }
    enet_socket_destroy(s);
    return out;
}

}  // namespace

Matchmaking::Matchmaking(Account me, std::string latestVersion, LocalPeer local)
    : me_(std::move(me)), latestVersion_(std::move(latestVersion)), local_(std::move(local)) {}

Matchmaking::~Matchmaking() {
    terminated_ = true;
    Fail("Matchmaking shut down");
    if (thread_.joinable()) thread_.join();
    DisconnectFromServer();
}

bool Matchmaking::IsSearching() const {
    const State s = state_.load();
    return s == State::Initializing || s == State::Matchmaking || s == State::OpponentConnecting;
}

std::string Matchmaking::Error() const {
    std::lock_guard<std::mutex> lk(result_);
    return error_;
}

std::unique_ptr<NetplayClient> Matchmaking::TakeNetplayClient() {
    std::lock_guard<std::mutex> lk(result_);
    return std::move(client_);
}

Ticket Matchmaking::GetTicket() const {
    std::lock_guard<std::mutex> lk(result_);
    return ticket_;
}

void Matchmaking::Fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> lk(result_);
        error_ = message;
    }
    state_ = State::Error;  // after the text, so a reader of the state finds it
}

void Matchmaking::FindMatch(Search search) {
    if (thread_.joinable()) {
        terminated_ = true;
        thread_.join();
        terminated_ = false;
    }
    search_ = std::move(search);
    {
        std::lock_guard<std::mutex> lk(result_);
        error_.clear();
    }
    if (!EnetReady()) {
        Fail("Network startup failed");
        return;
    }
    if (!local_.enabled && VersionNewer(latestVersion_, kSlippiSemver)) {
        Fail("Slippi " + latestVersion_ + " is out. Update Melee NX");
        return;
    }
    state_ = State::Initializing;
    thread_ = std::thread(&Matchmaking::Thread, this);
}

void Matchmaking::Thread() {
    try {
        while (IsSearching() && !terminated_) {
            switch (state_.load()) {
                case State::Initializing: StartMatchmaking(); break;
                case State::Matchmaking: HandleMatchmaking(); break;
                case State::OpponentConnecting: HandleConnecting(); break;
                default: break;
            }
        }
    } catch (const std::exception& e) {
        Log("matchmaking stopped: %s", e.what());
        Fail("Matchmaking failed on an unexpected response");
    }
    DisconnectFromServer();
}

void Matchmaking::DisconnectFromServer() {
    if (server_ && host_) {
        enet_peer_disconnect(server_, 0);
        ENetEvent ev;
        while (enet_host_service(host_, &ev, 1000) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                server_ = nullptr;
                break;
            }
        }
        if (server_) enet_peer_reset(server_);
    }
    server_ = nullptr;
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
}

bool Matchmaking::Ingest(const Ticket& t) {
    if (!t.ok) {
        Log("matchmaking: ticket refused: %s", t.error.c_str());
        Fail(t.serverError.empty() ? "Invalid match details from the server" : t.serverError);
        return false;
    }
    std::lock_guard<std::mutex> lk(result_);
    ticket_ = t;
    return true;
}

void Matchmaking::StartMatchmaking() {
    static std::mt19937 rng(static_cast<std::uint32_t>(TimeMs()));
    if (local_.enabled) {
        // The ticket the server would send this instance, run through the same parser.
        hostPort_ = local_.localPort;
        json players = json::array();
        const int n = static_cast<int>(local_.remotes.size()) + 1;
        for (int i = 0, r = 0; i < n; ++i) {
            const bool local = i == local_.localIndex;
            const std::string addr = local ? "127.0.0.1:" + std::to_string(local_.localPort)
                                           : local_.remotes[static_cast<size_t>(r++)];
            players.push_back({{"uid", local ? me_.uid : "peer-" + std::to_string(i + 1)},
                               {"displayName", local ? me_.displayName : "Player " + std::to_string(i + 1)},
                               {"connectCode", local ? me_.connectCode : "PEER#00" + std::to_string(i + 1)},
                               {"port", i + 1},
                               {"isLocalPlayer", local},
                               {"ipAddress", addr},
                               {"ipAddressLan", addr}});
        }
        const json resp = {{"type", "get-ticket-resp"}, {"matchId", "local"}, {"players", players},
                           {"isHost", local_.localIndex == 0}};
        if (Ingest(ParseTicket(resp.dump()))) state_ = State::OpponentConnecting;
        return;
    }
    for (int retry = 0; !host_ && retry < 15; ++retry) {
        hostPort_ = static_cast<std::uint16_t>(41000 + rng() % 10000);
        ENetAddress addr;
        addr.host = ENET_HOST_ANY;
        addr.port = hostPort_;
        host_ = enet_host_create(&addr, 1, 3, 0, 0);
    }
    if (!host_) {
        Fail("Could not open a network port");
        return;
    }
    ENetAddress server;
    if (enet_address_set_host(&server, kMatchmakingHost) != 0) {
        Fail("Could not find the Slippi server");
        return;
    }
    server.port = kMatchmakingPort;
    server_ = enet_host_connect(host_, &server, 3, 0);
    if (!server_) {
        Fail("Could not reach the Slippi server");
        return;
    }
    bool connected = false;
    for (int attempt = 0; attempt < 20 && !connected && !terminated_; ++attempt) {
        ENetEvent ev;
        connected = enet_host_service(host_, &ev, 500) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT;
    }
    if (!connected) {
        Fail("Could not reach the Slippi server");
        return;
    }
    const std::string lan = LocalAddress(&server);
    MmSend(server_, CreateTicketRequest(me_, search_.mode, search_.connectCode,
                                        lan.empty() ? "" : lan + ":" + std::to_string(hostPort_)));
    std::string text;
    if (MmReceive(host_, text, 5000) != 0) {
        Fail("Could not join the queue");
        return;
    }
    const json resp = json::parse(text, nullptr, false);
    if (!resp.is_object() || resp.value("type", "") != "create-ticket-resp") {
        Fail("Unexpected answer from the Slippi server");
        return;
    }
    if (const auto err = resp.value("error", ""); !err.empty()) {
        Fail(err);
        return;
    }
    Log("matchmaking: in queue (mode %d)", static_cast<int>(search_.mode));
    state_ = State::Matchmaking;
}

void Matchmaking::HandleMatchmaking() {
    std::string text;
    const int r = MmReceive(host_, text, 2000);
    if (r == -1) return;
    if (r != 0) {
        Fail("Lost connection to the Slippi server");
        return;
    }
    const Ticket t = ParseTicket(text);
    if (!t.latestVersion.empty()) latestVersion_ = t.latestVersion;
    if (!Ingest(t)) return;
    DisconnectFromServer();
    Log("matchmaking: opponent found (decider %d)", t.isHost);
    state_ = State::OpponentConnecting;
}

void Matchmaking::HandleConnecting() {
    const Ticket t = GetTicket();
    std::vector<std::string> addrs;
    std::vector<std::uint16_t> ports;
    for (const auto& ip : t.remoteIps) {
        const size_t colon = ip.rfind(':');
        if (colon == std::string::npos) continue;
        addrs.push_back(ip.substr(0, colon));
        ports.push_back(static_cast<std::uint16_t>(std::atoi(ip.c_str() + colon + 1)));
    }
    auto client = std::make_unique<NetplayClient>(addrs, ports, static_cast<std::uint8_t>(addrs.size()), hostPort_, t.isHost,
                                                  static_cast<std::uint8_t>(t.localPlayerIndex));
    while (client->GetStatus() == NetplayClient::Status::Initiated) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (terminated_ || state_ != State::OpponentConnecting) return;
    }
    if (client->GetStatus() != NetplayClient::Status::Connected) {
        if (local_.enabled || search_.mode == Mode::Teams) {
            Fail("Could not connect to the other players");
            return;
        }
        Log("matchmaking: connection failed, searching again");
        state_ = State::Initializing;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(result_);
        client_ = std::move(client);
    }
    state_ = State::ConnectionSuccess;
}

}  // namespace slippi
