// SPDX-License-Identifier: GPL-3.0-or-later
// Formats ported from Melee Unlocked (GPL-2.0-or-later); see slippi_protocol.h.
#include "slippi_protocol.h"

#include <cstring>

#include <nlohmann/json.hpp>

namespace slippi {
namespace {

using json = nlohmann::json;

// Type-checked reads of server JSON: a missing key or a value of another type gives the default.
const json* Member(const json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}
std::string Str(const json& j, const char* key, const char* def = "") {
    const json* v = Member(j, key);
    return v && v->is_string() ? v->get<std::string>() : std::string(def);
}
int Int(const json& j, const char* key, int def) {
    const json* v = Member(j, key);
    return v && v->is_number_integer() ? v->get<int>() : def;
}
bool Bool(const json& j, const char* key, bool def) {
    const json* v = Member(j, key);
    return v && v->is_boolean() ? v->get<bool>() : def;
}

}  // namespace

// ---- Packet ----------------------------------------------------------------------------------

void Packet::Append(const void* data, size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + size);
}
Packet& Packet::operator<<(std::uint8_t v) {
    buf_.push_back(v);
    return *this;
}
Packet& Packet::operator<<(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v >> 8));
    buf_.push_back(static_cast<std::uint8_t>(v));
    return *this;
}
Packet& Packet::operator<<(std::uint32_t v) {
    for (int i = 3; i >= 0; --i) buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    return *this;
}
Packet& Packet::operator<<(const std::string& s) {
    *this << static_cast<std::uint32_t>(s.size());
    Append(s.data(), s.size());
    return *this;
}
bool Packet::Check(size_t n) {
    if (!ok_ || pos_ + n > buf_.size()) {
        ok_ = false;
        return false;
    }
    return true;
}
Packet& Packet::operator>>(std::uint8_t& v) {
    if (Check(1)) v = buf_[pos_++];
    return *this;
}
Packet& Packet::operator>>(bool& v) {
    std::uint8_t b = 0;
    if (Check(1)) {
        b = buf_[pos_++];
        v = b != 0;
    }
    return *this;
}
Packet& Packet::operator>>(std::uint16_t& v) {
    if (Check(2)) {
        v = static_cast<std::uint16_t>((buf_[pos_] << 8) | buf_[pos_ + 1]);
        pos_ += 2;
    }
    return *this;
}
Packet& Packet::operator>>(std::uint32_t& v) {
    if (Check(4)) {
        v = (std::uint32_t(buf_[pos_]) << 24) | (std::uint32_t(buf_[pos_ + 1]) << 16) |
            (std::uint32_t(buf_[pos_ + 2]) << 8) | buf_[pos_ + 3];
        pos_ += 4;
    }
    return *this;
}
Packet& Packet::operator>>(std::int32_t& v) {
    std::uint32_t u = 0;
    if (Check(4)) {
        *this >> u;
        v = static_cast<std::int32_t>(u);
    }
    return *this;
}
Packet& Packet::operator>>(std::string& s) {
    std::uint32_t n = 0;
    *this >> n;
    if (Check(n)) {
        s.assign(reinterpret_cast<const char*>(&buf_[pos_]), n);
        pos_ += n;
    }
    return *this;
}

// ---- Pads ------------------------------------------------------------------------------------

Packet EncodePads(std::uint8_t localPort, std::int32_t checksumFrame, std::uint32_t checksum, const std::deque<Pad>& pads) {
    Packet p;
    p << static_cast<std::uint8_t>(kMsgPad) << (pads.empty() ? 0 : pads.front().frame) << localPort << checksumFrame << checksum;
    for (const Pad& pad : pads) p.Append(pad.data, kPadDataSize);
    return p;
}

PadError DecodePads(const std::uint8_t* data, size_t size, PadHeader& h, std::deque<Pad>& queue, int* added) {
    if (added) *added = 0;
    Packet p(data, size);
    std::uint8_t id = 0;
    if (!(p >> id) || id != kMsgPad || !(p >> h.frame >> h.port >> h.checksumFrame >> h.checksum)) return PadError::BadHeader;
    // Same rule as Slippi Dolphin: frames newer than what is queued, newest first in the packet.
    const std::int32_t head = queue.empty() ? 0 : queue.front().frame;
    const std::int64_t count = static_cast<std::int64_t>(h.frame) - head;
    if (count <= 0) return PadError::None;
    if (count > kMaxPadsPerPacket) return PadError::TooMany;
    if (kPadHeaderSize + count * kPadDataSize > static_cast<std::int64_t>(size)) return PadError::TooSmall;
    for (std::int64_t i = count - 1; i >= 0; --i) {
        Pad pad;
        pad.frame = static_cast<std::int32_t>(h.frame - i);
        std::memcpy(pad.data, &data[kPadHeaderSize + i * kPadDataSize], kPadDataSize);
        queue.push_front(pad);
    }
    if (added) *added = static_cast<int>(count);
    return PadError::None;
}

Packet EncodePadAck(std::int32_t frame, std::uint8_t localPort) {
    Packet p;
    p << static_cast<std::uint8_t>(kMsgPadAck) << frame << localPort;
    return p;
}

// ---- Selections and chat ---------------------------------------------------------------------

Packet EncodeSelections(const PlayerSelections& s) {
    Packet p;
    p << static_cast<std::uint8_t>(kMsgMatchSelections) << s.characterId << s.characterColor << s.isCharacterSelected
      << s.playerIdx << s.stageId << s.isStageSelected << s.rngOffset << s.teamId << s.altStageMode;
    return p;
}

bool DecodeSelections(Packet& p, PlayerSelections& s) {
    return static_cast<bool>(p >> s.characterId >> s.characterColor >> s.isCharacterSelected >> s.playerIdx >> s.stageId >>
                             s.isStageSelected >> s.rngOffset >> s.teamId >> s.altStageMode);
}

bool IsAllowedChatMessage(int id) {
    static const int allowed[] = {136, 129, 130, 132, 34, 40, 33, 36, 72, 66, 68, 65, 24, 18, 20, 17, kChatDisabled};
    for (int a : allowed) {
        if (a == id) return true;
    }
    return false;
}

// ---- Matchmaking -----------------------------------------------------------------------------

std::string CreateTicketRequest(const Account& me, Mode mode, const std::string& directCode, const std::string& lanAddress) {
    // Slippi sends the Direct code as an array of bytes (the game's Shift-JIS text).
    const std::vector<std::uint8_t> code(directCode.begin(), directCode.end());
    json req;
    req["type"] = "create-ticket";
    req["user"] = {{"uid", me.uid}, {"playKey", me.playKey}, {"connectCode", me.connectCode}, {"displayName", me.displayName}};
    req["search"] = {{"mode", static_cast<int>(mode)}, {"connectCode", code}};
    req["appVersion"] = kSlippiSemver;
    req["ipAddressLan"] = lanAddress;
    return req.dump();
}

Ticket ParseTicket(const std::string& jsonText) {
    Ticket t;
    const json resp = json::parse(jsonText, nullptr, false);
    if (resp.is_discarded() || !resp.is_object()) {
        t.error = "not JSON";
        return t;
    }
    if (Str(resp, "type") != "get-ticket-resp") {
        t.error = "not a get-ticket-resp";
        return t;
    }
    t.serverError = Str(resp, "error");
    t.latestVersion = Str(resp, "latestVersion");
    if (!t.serverError.empty()) {
        t.error = t.serverError;
        return t;
    }
    t.matchId = Str(resp, "matchId");
    const json* queue = Member(resp, "players");
    if (!queue || !queue->is_array()) {
        t.error = "no player list";
        return t;
    }
    if (queue->size() < 2 || queue->size() > static_cast<size_t>(kPlayerCountMax)) {
        t.error = std::to_string(queue->size()) + " players";
        return t;
    }
    std::string localExternalIp;
    int locals = 0;
    bool used[kPlayerCountMax] = {};
    for (const auto& el : *queue) {
        if (!el.is_object()) {
            t.error = "a player entry is not an object";
            return t;
        }
        Player p;
        p.uid = Str(el, "uid");
        p.displayName = Str(el, "displayName");
        p.connectCode = Str(el, "connectCode");
        p.port = Int(el, "port", 0);
        p.isBot = Bool(el, "isBot", false);
        if (p.port < 1 || p.port > kPlayerCountMax) {
            t.error = "player port " + std::to_string(p.port);
            return t;
        }
        if (used[p.port - 1]) {
            t.error = "port " + std::to_string(p.port) + " used twice";
            return t;
        }
        used[p.port - 1] = true;
        if (Bool(el, "isLocalPlayer", false)) {
            ++locals;
            const std::string ip = Str(el, "ipAddress", "1.1.1.1:123");
            localExternalIp = ip.substr(0, ip.find(':'));
            t.localPlayerIndex = p.port - 1;
        }
        t.players.push_back(std::move(p));
    }
    if (locals != 1) {
        t.error = locals ? "more than one local player" : "no local player";
        return t;
    }
    for (const auto& el : *queue) {
        if (Int(el, "port", 0) - 1 == t.localPlayerIndex) continue;
        const std::string ext = Str(el, "ipAddress", "1.1.1.1:123");
        const std::string lan = Str(el, "ipAddressLan", "1.1.1.1:123");
        // Behind the same external address: reach the peer on the LAN.
        t.remoteIps.push_back(ext.substr(0, ext.find(':')) != localExternalIp || lan.empty() ? ext : lan);
    }
    t.isHost = Bool(resp, "isHost", false);
    if (const json* stages = Member(resp, "stages"); stages && stages->is_array()) {
        for (const auto& s : *stages) {
            if (s.is_number_integer()) t.stages.push_back(static_cast<std::uint16_t>(s.get<int>()));
        }
    }
    if (t.stages.empty()) {
        t.stages = {0x3, 0x8, 0x1C, 0x1F, 0x20};
        if (t.players.size() == 2) t.stages.push_back(0x2);
    }
    if (const json* items = Member(resp, "items"); items && items->is_number_unsigned()) t.items = items->get<std::uint32_t>();
    t.ok = true;
    return t;
}

std::string ConnectCodeForGame(const std::string& code) {
    std::string out;
    for (char c : code) {
        if (c == '#') {
            out += static_cast<char>(0x81);
            out += static_cast<char>(0x94);
        } else {
            out += c;
        }
    }
    out.resize(10);
    return out;
}

}  // namespace slippi
