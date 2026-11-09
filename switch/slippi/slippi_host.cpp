// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Melee Unlocked's port/runtime/hle/slippi_online.cpp (GPL-2.0-or-later), itself the
// online half of Slippi Dolphin's CEXISlippi. Reply layouts are byte-for-byte the donor's, because
// the game-side online code (mu_online.c and the online menus) reads them as Slippi's code does.
//
// Left out from the donor: YouGame, Discord, early inputs, the session trace, build checks against
// mod clients, Ranked bots, game prep steps, and the emulation-speed trim of time sync (melee-nx
// paces frames by vsync; halting and advancing frames are kept). Player names are converted to the
// game's Shift-JIS for ASCII only. See docs/SLIPPI.md.
#include "slippi_host.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "perf_switch.h"
#include "rollback_switch.h"
#include "slippi_net.h"

// The console pad report (dolphin/pad.h PADStatus), for the lobby's cancel button.
struct PadReport {
    std::uint16_t button;
    std::int8_t stickX, stickY, substickX, substickY;
    std::uint8_t triggerLeft, triggerRight, analogA, analogB;
    std::int8_t err;
};
extern "C" std::uint32_t VIGetRetraceCount(void);
extern "C" void VIWaitForRetrace(void);
extern "C" std::uint32_t PADRead(PadReport* status);

namespace {

using json = nlohmann::json;
using slippi::kPlayerCountMax;
using slippi::kRemotePlayerMax;
using slippi::kRollbackMaxFrames;

std::string gDir = "sdmc:/switch/melee-nx/slippi";  // user.json and the code history
slippi::LocalPeering gLocalPeer;                     // tests only: peer without the server
constexpr int kPadFullSize = 12;
constexpr int kOnlineLockstepInterval = 30;
constexpr std::uint8_t kChatDisabled = 0x10;

enum Cmd : std::uint8_t {
    CMD_ONLINE_INPUTS = 0xB0, CMD_CAPTURE_SAVESTATE = 0xB1, CMD_LOAD_SAVESTATE = 0xB2, CMD_GET_MATCH_STATE = 0xB3,
    CMD_FIND_OPPONENT = 0xB4, CMD_SET_MATCH_SELECTIONS = 0xB5, CMD_OPEN_LOGIN = 0xB6, CMD_LOGOUT = 0xB7,
    CMD_UPDATE = 0xB8, CMD_GET_ONLINE_STATUS = 0xB9, CMD_CLEANUP_CONNECTION = 0xBA, CMD_SEND_CHAT_MESSAGE = 0xBB,
    CMD_GET_NEW_SEED = 0xBC, CMD_REPORT_GAME = 0xBD, CMD_FETCH_CODE_SUGGESTION = 0xBE, CMD_OVERWRITE_SELECTIONS = 0xBF,
    CMD_GP_COMPLETE_STEP = 0xC0, CMD_GP_FETCH_STEP = 0xC1, CMD_REPORT_SET_COMPLETE = 0xC2,
    CMD_GET_PLAYER_SETTINGS = 0xC3, CMD_REPORT_MATCH_STATUS_UPDATE = 0xC4, CMD_GET_DELAY = 0xD5,
    CMD_GET_RANK = 0xE3, CMD_FETCH_RANK = 0xE4, CMD_GET_RANK_VISIBILITY = 0xE5,
};

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("[slippi] %s\n", buf);
}

std::uint32_t Be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint16_t Be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
void AppendU32(std::vector<std::uint8_t>& q, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) q.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// ---- text for the game (Shift-JIS) ----------------------------------------------------------------

// Up to `length` characters as the game's 2-byte text: ASCII punctuation becomes its full-width
// Shift-JIS form, as Slippi Dolphin writes it (convert_narrow_special_shiftjis in the donor).
// shortcut: non-ASCII characters become '?'; port a UTF-8 -> Shift-JIS table for Japanese names.
std::string GameText(const std::string& utf8, int length) {
    static const std::unordered_map<char, std::uint16_t> special = {
        {'!', 0x8149}, {'"', 0x8168}, {'#', 0x8194}, {'$', 0x8190}, {'%', 0x8193}, {'&', 0x8195}, {'\'', 0x8166},
        {'(', 0x8169}, {')', 0x816a}, {'*', 0x8196}, {'+', 0x817b}, {',', 0x8143}, {'-', 0x817c}, {'.', 0x8144},
        {'/', 0x815e}, {':', 0x8146}, {';', 0x8147}, {'<', 0x8183}, {'=', 0x8181}, {'>', 0x8184}, {'?', 0x8148},
        {'@', 0x8197}, {'[', 0x816d}, {'\\', 0x815f}, {']', 0x816e}, {'^', 0x814f}, {'_', 0x8151}, {'`', 0x814d},
        {'{', 0x816f}, {'|', 0x8162}, {'}', 0x8170}, {'~', 0x8160},
    };
    std::string out;
    int count = 0;
    for (size_t i = 0; i < utf8.size() && count < length; ++count) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        i += len;
        char ch = len == 1 ? static_cast<char>(c) : '?';
        if (auto it = special.find(ch); it != special.end()) {
            out += static_cast<char>(it->second >> 8);
            out += static_cast<char>(it->second & 0xFF);
        } else {
            out += ch;
        }
    }
    out.resize(static_cast<size_t>(length) * 2 + 1);
    return out;
}

std::string TruncateChars(const std::string& s, int length) {
    std::string out;
    int count = 0;
    for (size_t i = 0; i < s.size() && count < length; ++count) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        out.append(s, i, len);
        i += len;
    }
    return out;
}

// Shift-JIS from the game (connect code field) to UTF-8, for the codes the player typed: full-width
// letters, digits and '#' map back to ASCII; anything else is dropped.
std::string GameCodeToUtf8(const std::string& sj) {
    std::string out;
    for (size_t i = 0; i < sj.size(); ++i) {
        const auto c = static_cast<unsigned char>(sj[i]);
        if (c < 0x80) {
            if (c) out += static_cast<char>(c);
            continue;
        }
        if (i + 1 >= sj.size()) break;
        const std::uint16_t w = static_cast<std::uint16_t>((c << 8) | static_cast<unsigned char>(sj[++i]));
        if (w == 0x8194) out += '#';
        else if (w >= 0x824F && w <= 0x8258) out += static_cast<char>('0' + (w - 0x824F));
        else if (w >= 0x8260 && w <= 0x8279) out += static_cast<char>('A' + (w - 0x8260));
        else if (w >= 0x8281 && w <= 0x829A) out += static_cast<char>('a' + (w - 0x8281));
    }
    return out;
}

// ---- account and code history ---------------------------------------------------------------------

struct User {
    slippi::Account account;
    std::string latestVersion;
    bool loggedIn = false;

    bool Load() {
        std::ifstream in(gDir + "/user.json", std::ios::binary);
        loggedIn = false;
        if (!in) return false;
        const json j = json::parse(in, nullptr, false);
        if (!j.is_object()) return false;
        account.uid = j.value("uid", "");
        account.playKey = j.value("playKey", "");
        account.connectCode = j.value("connectCode", "");
        account.displayName = j.value("displayName", "");
        latestVersion = j.value("latestVersion", "");
        loggedIn = !account.uid.empty() && !account.playKey.empty() && !account.connectCode.empty();
        return loggedIn;
    }
};

class CodeHistory {
public:
    explicit CodeHistory(std::string path) : path_(std::move(path)) {
        std::ifstream in(path_, std::ios::binary);
        if (!in) return;
        const json j = json::parse(in, nullptr, false);
        if (!j.is_array()) return;
        for (const auto& e : j) {
            if (e.is_object() && e.contains("connectCode") && e["connectCode"].is_string())
                codes_.push_back(e["connectCode"].get<std::string>());
        }
    }
    std::string Get(int i) const { return i >= 0 && i < static_cast<int>(codes_.size()) ? codes_[static_cast<size_t>(i)] : "1"; }
    int Length() const { return static_cast<int>(codes_.size()); }
    void Add(const std::string& code) {
        if (code.empty()) return;
        codes_.erase(std::remove(codes_.begin(), codes_.end(), code), codes_.end());
        codes_.insert(codes_.begin(), code);
        if (codes_.size() > 20) codes_.resize(20);
        json j = json::array();
        for (const auto& c : codes_) j.push_back({{"connectCode", c}, {"lastPlayed", ""}});
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (out) out << j.dump(2);
    }

private:
    std::string path_;
    std::vector<std::string> codes_;
};

// ---- session state ---------------------------------------------------------------------------------

struct Session {
    User user;
    std::unique_ptr<CodeHistory> direct, teams;
    std::unique_ptr<slippi::Matchmaking> matchmaking;
    std::unique_ptr<slippi::NetplayClient> netplay;
    slippi::Ticket ticket;  // the match the last search found
    slippi::PlayerSelections localSelections;
    slippi::Mode lastMode = slippi::Mode::Unranked;
    std::vector<slippi::PlayerSelections> overwrite;
    std::vector<std::uint16_t> allowedStages = {0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20};
    std::string forcedError;
    std::uint8_t localPlayerIndex = 0, remotePlayerIndex = 1;
    std::uint32_t stalls[kRemotePlayerMax] = {};
    int framesToSkip = 0, framesToAdvance = 0;
    bool skipping = false, advancing = false;
    std::mt19937 rng{std::random_device{}()};
    std::uint64_t rollbacks = 0;
    std::uint8_t sentChat = 0;
    int delay = 2;
    bool resim = false;
    bool initialised = false;
    std::string opponentCode;  // the code the launcher passed (MELEE_NX_SLIPPI_CODE); empty: Unranked
    bool enabled = false;      // the launcher chose Slippi
    std::string status;        // the line drawn over the game
    std::uint32_t statusUntil = 0;
    bool lobbyOpen = false;    // the lobby is presenting frames itself
};

Session g;

void Init() {
    if (g.initialised) return;
    g.initialised = true;
    g.direct = std::make_unique<CodeHistory>(gDir + "/direct-codes.json");
    g.teams = std::make_unique<CodeHistory>(gDir + "/teams-codes.json");
    g.delay = std::clamp(melee_nx_perf_int("slippi_delay", 2), 1, 9);
    g.user.Load();
    const char* mode = std::getenv("MELEE_NX_MODE");
    const char* code = std::getenv("MELEE_NX_SLIPPI_CODE");
    g.opponentCode = code ? code : "";
    g.enabled = mode && std::strcmp(mode, "slippi") == 0;  // no code: Unranked
    slippi::SetLogger([](const char* line) { std::printf("[slippi net] %s\n", line); });
    Log("host ready: %s, delay %d", g.user.loggedIn ? g.user.account.connectCode.c_str() : "signed out", g.delay);
}

std::uint8_t RemoteCount() {
    const size_t n = g.ticket.players.empty() ? 0 : g.ticket.players.size() - 1;
    return static_cast<std::uint8_t>(std::min<size_t>(n, kRemotePlayerMax));
}

bool Disconnected() {
    if (!g.netplay) return true;
    const auto st = g.netplay->GetStatus();
    return st != slippi::NetplayClient::Status::Connected;
}

std::uint16_t RandomStage() {
    if (g.allowedStages.empty()) return 0x1F;
    return g.allowedStages[g.rng() % g.allowedStages.size()];
}

void CleanupConnection() {
    Log("connection cleanup");
    g.matchmaking.reset();
    g.netplay.reset();
    g.localSelections = slippi::PlayerSelections();
    g.forcedError.clear();
    g.overwrite.clear();
    g.ticket = slippi::Ticket();
}

// ---- per-frame online flow ------------------------------------------------------------------------

bool ShouldSkipFrame(std::int32_t frame, std::int32_t finalized) {
    const auto st = g.netplay->GetStatus();
    if (st == slippi::NetplayClient::Status::Failed || st == slippi::NetplayClient::Status::Disconnected) return false;
    bool needsInputs = false;
    for (int i = 0; i < RemoteCount(); ++i) {
        const slippi::RemotePads pads = g.netplay->GetRemotePads(i, kRollbackMaxFrames);
        if (pads.disconnected) {
            g.stalls[i] = 0;
            continue;
        }
        bool enough = pads.latestFrame - finalized >= (frame - finalized - kRollbackMaxFrames);
        if (frame == 2 && pads.latestFrame < 1) enough = false;  // start in step with the other game
        if (enough) {
            g.stalls[i] = 0;
            continue;
        }
        if (++g.stalls[i] > 60 * 7) {
            Log("force-disconnecting after a 7 s stall (frame %d, their latest %d)", frame, pads.latestFrame);
            g.netplay->ForceDisconnect();
            g.stalls[i] = 0;
            continue;
        }
        needsInputs = true;
    }
    if (needsInputs) return true;
    constexpr std::int32_t frameTime = 16683, t1 = 10000, t2 = 2 * frameTime + t1;
    if (frame % kOnlineLockstepInterval == 0 && !g.skipping) {
        const std::int32_t offset = g.netplay->CalcTimeOffsetUs();
        if (offset > (frame <= 120 ? t1 : t2)) {
            g.skipping = true;
            g.framesToSkip = std::min((offset - t1) / frameTime + 1, frame <= 120 ? 5 : 1);
            Log("halting on frame %d for time sync (offset %d us, %d frames)", frame, offset, g.framesToSkip);
        }
    }
    if (g.framesToSkip > 0) {
        --g.framesToSkip;
        return true;
    }
    g.skipping = false;
    return false;
}

bool ShouldAdvanceFrame(std::int32_t frame) {
    if (frame % kOnlineLockstepInterval == 0) {
        const std::int32_t offset = g.netplay->CalcTimeOffsetUs();
        constexpr std::int32_t frameTime = 16683, t1 = 10000, t2 = frameTime + t1;
        if (offset < -t2 && !g.advancing) {
            g.advancing = true;
            g.framesToAdvance = std::min((-offset - t1) / frameTime + 1, frame > 120 ? 3 : 0);
            Log("advancing on frame %d for time sync (offset %d us, %d frames)", frame, offset, g.framesToAdvance);
        }
    }
    if (g.framesToAdvance > 0) {
        if (frame % 5 != 0) return false;
        --g.framesToAdvance;
        return true;
    }
    g.advancing = false;
    return false;
}

void PrepareOpponentInputs(std::int32_t frame, bool skip, std::vector<std::uint8_t>& q) {
    q.clear();
    std::uint8_t result = 1;
    if (skip) result = 2;
    else if (g.netplay->GetStatus() != slippi::NetplayClient::Status::Connected) result = 3;
    else if (ShouldAdvanceFrame(frame)) result = 4;
    q.push_back(result);
    const std::uint8_t remotes = RemoteCount();
    q.push_back(remotes);
    slippi::RemotePads pads[kRemotePlayerMax];
    std::int32_t latestFromOpps = -124;
    std::uint32_t lastChecksumFrame = 0, lastChecksum = 0;
    for (int i = 0; i < remotes; ++i) {
        pads[i] = g.netplay->GetRemotePads(i, kRollbackMaxFrames);
        if (pads[i].disconnected) continue;
        if (pads[i].latestFrame > latestFromOpps) {
            latestFromOpps = pads[i].latestFrame;
            lastChecksumFrame = static_cast<std::uint32_t>(pads[i].checksumFrame);
            lastChecksum = pads[i].checksum;
        }
    }
    constexpr std::int32_t kDespawnInterval = 30;
    std::uint8_t despawn[kRemotePlayerMax] = {};
    for (int i = 0; i < remotes; ++i) {
        if (!pads[i].disconnected) continue;
        const std::int32_t threshold = pads[i].latestFrame + 2 * kRollbackMaxFrames + 2;
        if (frame >= ((threshold + kDespawnInterval - 1) / kDespawnInterval) * kDespawnInterval) despawn[i] = 1;
    }
    for (int i = 0; i < remotes; ++i) {
        if (!pads[i].disconnected) {
            AppendU32(q, static_cast<std::uint32_t>(pads[i].checksumFrame));
            AppendU32(q, pads[i].checksum);
        } else {
            pads[i].latestFrame = latestFromOpps;
            AppendU32(q, lastChecksumFrame);
            AppendU32(q, lastChecksum);
        }
    }
    for (int i = remotes; i < kRemotePlayerMax; ++i) {
        AppendU32(q, 0);
        AppendU32(q, 0);
    }
    std::int32_t latestRead[kRemotePlayerMax] = {};
    for (int i = 0; i < remotes; ++i) {
        latestRead[i] = std::min(pads[i].latestFrame, frame);
        AppendU32(q, static_cast<std::uint32_t>(latestRead[i]));
    }
    for (int i = remotes; i < kRemotePlayerMax; ++i) {
        latestRead[i] = frame;
        AppendU32(q, static_cast<std::uint32_t>(frame));
    }
    AppendU32(q, static_cast<std::uint32_t>(*std::min_element(std::begin(latestRead), std::end(latestRead))));
    // Up to 7 pads per remote, newest first from the frame being read, 12 bytes each (8 used).
    for (int i = 0; i < kRemotePlayerMax; ++i) {
        std::vector<std::uint8_t> tx;
        if (i < remotes) {
            for (auto it = pads[i].pads.rbegin(); it != pads[i].pads.rend(); ++it) {
                if (it->frame > frame) continue;
                tx.insert(tx.end(), it->data, it->data + slippi::kPadDataSize);
                tx.insert(tx.end(), kPadFullSize - slippi::kPadDataSize, 0);
            }
        }
        tx.resize(static_cast<size_t>(kPadFullSize) * kRollbackMaxFrames, 0);
        q.insert(q.end(), tx.begin(), tx.end());
    }
    for (int i = 0; i < kRemotePlayerMax; ++i) q.push_back(despawn[i]);
}

void HandleOnlineInputs(const std::uint8_t* p, std::vector<std::uint8_t>& q) {
    q.clear();
    const auto frame = static_cast<std::int32_t>(Be32(p)), finalized = static_cast<std::int32_t>(Be32(p + 4));
    const std::uint32_t finalizedChecksum = Be32(p + 8);
    const std::uint8_t delay = p[12];
    const std::uint8_t* inputs = p + 13;
    if (frame == 1) {
        melee_nx::rollback::SlotsReset(kRollbackMaxFrames);
        for (auto& s : g.stalls) s = 0;
        g.framesToSkip = g.framesToAdvance = 0;
        g.skipping = g.advancing = false;
        g.localSelections = slippi::PlayerSelections();
        if (g.netplay) g.netplay->StartGame();
        Log("online game starts, delay %d frames", static_cast<int>(delay));
    }
    if (Disconnected()) {
        q.push_back(3);
        return;
    }
    g.netplay->DropOldRemoteInputs(finalized);
    const bool skip = ShouldSkipFrame(frame, finalized);
    if (skip) {
        g.netplay->ResendPads();
    } else {
        if (frame == 1) {
            const std::uint8_t zero[slippi::kPadDataSize] = {};
            for (int i = 1; i <= delay; ++i) g.netplay->SendPad(i, zero);
        }
        g.netplay->SendPad(frame + delay, inputs, finalized, finalizedChecksum);
    }
    PrepareOpponentInputs(frame, skip, q);
}

// ---- lobby -----------------------------------------------------------------------------------------

void StartFindMatch(const std::uint8_t* p) {
    g.forcedError.clear();
    if (!g.user.Load()) {
        g.forcedError = "Sign in to Slippi in the Melee NX launcher";
        return;
    }
    const auto mode = static_cast<slippi::Mode>(p[0]);
    std::string sj(reinterpret_cast<const char*>(p) + 1, 18);
    sj.erase(std::find(sj.begin(), sj.end(), '\0'), sj.end());
    if (mode == slippi::Mode::Direct) g.direct->Add(GameCodeToUtf8(sj));
    if (mode == slippi::Mode::Teams) g.teams->Add(GameCodeToUtf8(sj));
    g.lastMode = mode;
    if (mode == slippi::Mode::Ranked) {
        g.forcedError = "Ranked is not available in Melee NX";
        return;
    }
    if ((mode == slippi::Mode::Unranked || mode == slippi::Mode::Party) && g.localSelections.characterId >= 26) {
        g.forcedError = "The character you selected is not allowed in this mode";
        return;
    }
    g.netplay.reset();
    g.matchmaking = std::make_unique<slippi::Matchmaking>(g.user.account, g.user.latestVersion, gLocalPeer);
    g.matchmaking->FindMatch({mode, sj});
    Log("search started (mode %d)", static_cast<int>(mode));
}

bool TagMatches(const std::uint8_t* input, std::uint8_t len, const std::string& tag) {
    const std::string jis = GameText(tag, 8);
    for (int i = 0; i < len; ++i) {
        if (input[i * 3] != static_cast<std::uint8_t>(jis[static_cast<size_t>(i) * 2]) ||
            input[i * 3 + 1] != static_cast<std::uint8_t>(jis[static_cast<size_t>(i) * 2 + 1]))
            return false;
    }
    return true;
}

void HandleNameEntryLoad(const std::uint8_t* p, std::vector<std::uint8_t>& q) {
    const std::uint8_t len = std::min<std::uint8_t>(p[24], 8);
    const std::uint32_t initial = Be32(p + 25);
    const std::uint8_t scroll = p[29], mode = p[30];
    CodeHistory* history = mode == static_cast<std::uint8_t>(slippi::Mode::Teams) ? g.teams.get() : g.direct.get();
    std::uint32_t cur = initial;
    if (scroll == 1) ++cur;
    else if (scroll == 2) cur = cur > 0 ? cur - 1 : cur;
    else if (scroll == 3) cur = 0;
    std::string tag = "1";
    while (cur < static_cast<std::uint32_t>(history->Length())) {
        tag = history->Get(static_cast<int>(cur));
        if (TagMatches(p, len, tag)) break;
        cur = scroll == 2 ? cur - 1 : cur + 1;
    }
    tag = history->Get(static_cast<int>(cur));
    if (tag == "1") {
        const std::string initTag = history->Get(static_cast<int>(initial));
        if (TagMatches(p, len, initTag)) {
            tag = initTag;
            cur = initial;
        }
    }
    q.clear();
    if (tag == "1") {
        q.push_back(0);
        q.insert(q.end(), p, p + 3 * len);
        q.insert(q.end(), static_cast<size_t>(3 * (8 - len)), 0);
        q.push_back(len);
        AppendU32(q, initial);
        return;
    }
    q.push_back(1);
    const std::string jis = GameText(tag, 8);
    for (int i = 0; i < 8; ++i) {
        q.push_back(static_cast<std::uint8_t>(jis[static_cast<size_t>(i) * 2]));
        q.push_back(static_cast<std::uint8_t>(jis[static_cast<size_t>(i) * 2 + 1]));
        q.push_back(0);
    }
    q.push_back(static_cast<std::uint8_t>(std::min<size_t>(TruncateChars(tag, 8).size(), 8)));
    AppendU32(q, cur);
}

void SetMatchSelections(const std::uint8_t* p) {
    slippi::PlayerSelections s;
    s.teamId = p[0];
    s.characterId = p[1];
    s.characterColor = p[2];
    s.isCharacterSelected = p[3] != 0;
    s.stageId = Be16(p + 4);
    const std::uint8_t stageOption = p[6];
    s.altStageMode = p[8];
    s.isStageSelected = stageOption == 1 || stageOption == 3;
    if (stageOption == 3) s.stageId = RandomStage();
    s.rngOffset = g.rng() % 0xFFFF;
    slippi::PlayerSelections& l = g.localSelections;
    l.rngOffset = s.rngOffset;
    if (s.isStageSelected) {
        l.stageId = s.stageId;
        l.isStageSelected = true;
        l.altStageMode = s.altStageMode;
    }
    if (s.isCharacterSelected) {
        l.characterId = s.characterId;
        l.characterColor = s.characterColor;
        l.teamId = s.teamId;
        l.isCharacterSelected = true;
    }
    if (g.netplay) g.netplay->SetMatchSelections(l);
}

std::string PlayerName(int i) {
    return i >= 0 && i < static_cast<int>(g.ticket.players.size()) ? g.ticket.players[static_cast<size_t>(i)].displayName : "";
}

void PrepareMatchState(std::vector<std::uint8_t>& q) {
    static std::vector<std::uint8_t> block = {
        0x32, 0x01, 0x86, 0x4C, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x6E, 0x00, 0x1F, 0x00, 0x00,
        0x01, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
        0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
        0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x05, 0x00, 0x04, 0x01, 0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
        0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
        0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
        0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
        0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
        0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
        0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
        0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
        0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
        0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
        0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
    };
    q.clear();
    auto state = slippi::Matchmaking::State::Idle;
    if (!g.forcedError.empty()) state = slippi::Matchmaking::State::Error;
    else if (g.matchmaking) state = g.matchmaking->GetState();
    q.push_back(static_cast<std::uint8_t>(state));
    const std::uint8_t localReady = g.localSelections.isCharacterSelected ? 1 : 0;
    std::uint8_t remoteReady = 0;
    std::uint16_t altStageMode = 0;
    if (state == slippi::Matchmaking::State::ConnectionSuccess) {
        if (!g.netplay) {
            g.netplay = g.matchmaking->TakeNetplayClient();
            g.ticket = g.matchmaking->GetTicket();
            g.allowedStages = g.ticket.stages.empty() ? std::vector<std::uint16_t>{0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20} : g.ticket.stages;
            g.localPlayerIndex = static_cast<std::uint8_t>(std::clamp(g.ticket.localPlayerIndex, 0, kPlayerCountMax - 1));
            g.localSelections.stageId = RandomStage();
            if (g.netplay) g.netplay->SetMatchSelections(g.localSelections);
        }
        const bool connected = g.netplay && g.netplay->GetStatus() == slippi::NetplayClient::Status::Connected &&
                               g.netplay->ActiveRemoteCount() == RemoteCount();
        if (!connected) {
            CleanupConnection();
            PrepareMatchState(q);
            return;
        }
        const slippi::MatchInfo mi = g.netplay->GetMatchInfo();
        remoteReady = 1;
        for (int i = 0; i < RemoteCount(); ++i) {
            if (!mi.remote[i].isCharacterSelected) remoteReady = 0;
        }
        if (RemoteCount() == 1) {
            g.localPlayerIndex = g.netplay->IsDecider() ? 0 : 1;
            g.remotePlayerIndex = g.netplay->IsDecider() ? 1 : 0;
        }
    } else {
        g.netplay.reset();
    }
    std::uint32_t rngOffset = 0;
    std::uint8_t chatId = 0, chatPlayer = 0, sentChat = 0;
    q.push_back(localReady);
    q.push_back(remoteReady);
    q.push_back(g.localPlayerIndex);
    q.push_back(g.remotePlayerIndex);
    std::string localName;
    if (g.netplay) {
        sentChat = g.sentChat;
        g.sentChat = 0;
        if (sentChat == 0) {
            std::uint8_t from = 0;
            chatId = static_cast<std::uint8_t>(g.netplay->TakeRemoteChatMessage(&from));
            chatPlayer = from;
        }
        if (RemoteCount() == 1) chatPlayer = sentChat ? g.localPlayerIndex : g.remotePlayerIndex;
        localName = g.user.account.displayName;
    }
    if (localReady && remoteReady) {
        const bool decider = g.netplay->IsDecider();
        const std::uint8_t remotes = RemoteCount();
        const slippi::MatchInfo mi = g.netplay->GetMatchInfo();
        slippi::PlayerSelections lps = mi.local;
        slippi::PlayerSelections rps[kRemotePlayerMax];
        for (int i = 0; i < kRemotePlayerMax; ++i) rps[i] = mi.remote[i];
        std::vector<slippi::PlayerSelections*> ordered(static_cast<size_t>(remotes) + 1, nullptr);
        if (lps.playerIdx < ordered.size()) ordered[lps.playerIdx] = &lps;
        for (int i = 0; i < remotes; ++i) {
            if (rps[i].playerIdx < ordered.size()) ordered[rps[i].playerIdx] = &rps[i];
        }
        for (auto& o : ordered) {
            if (!o) o = &lps;
        }
        for (size_t i = 0; i < g.overwrite.size() && i < ordered.size(); ++i) {
            ordered[i]->characterId = g.overwrite[i].characterId;
            ordered[i]->characterColor = g.overwrite[i].characterColor;
            ordered[i]->stageId = g.overwrite[i].stageId;
        }
        std::uint16_t stageId = 0x1F;
        for (auto* s : ordered) {
            if (!s->isStageSelected) continue;
            stageId = s->stageId;
            altStageMode = s->altStageMode;
            break;
        }
        if (stageId == 0) {
            static const std::uint16_t legal[] = {2, 3, 8, 28, 31, 32};
            stageId = legal[(decider ? lps.rngOffset : rps[0].rngOffset) % 6];
        }
        bool charsOk = lps.characterId < 26;
        for (int i = 0; i < remotes; ++i) charsOk = charsOk && rps[i].characterId < 26;
        const bool fixedRules = g.lastMode == slippi::Mode::Unranked || g.lastMode == slippi::Mode::Party;
        if (!charsOk || (fixedRules && std::find(g.allowedStages.begin(), g.allowedStages.end(), stageId) == g.allowedStages.end())) {
            CleanupConnection();
            g.forcedError = "A character or stage is not allowed in this mode";
            PrepareMatchState(q);
            return;
        }
        rngOffset = decider ? lps.rngOffset : rps[0].rngOffset;
        const bool teams = g.lastMode == slippi::Mode::Teams;
        const std::uint8_t firstTeam = ordered[0]->teamId;
        bool sameTeam = true;
        for (auto* s : ordered) sameTeam = sameTeam && s->teamId == firstTeam;
        static const std::uint8_t perms[6][4] = {{0, 0, 1, 1}, {1, 1, 0, 0}, {0, 1, 1, 0},
                                                 {1, 0, 0, 1}, {0, 1, 0, 1}, {1, 0, 1, 0}};
        const std::uint8_t* assign = perms[rngOffset % 6];
        for (auto* s : ordered) {
            if (!s->isCharacterSelected) continue;
            std::uint8_t team = teams ? s->teamId : 0;
            if (teams && sameTeam) team = assign[s->playerIdx];
            block[0x60 + s->playerIdx * 0x24] = s->characterId;
            block[0x63 + s->playerIdx * 0x24] = s->characterColor;
            block[0x67 + s->playerIdx * 0x24] = 0;
            block[0x69 + s->playerIdx * 0x24] = team;
        }
        std::unordered_map<std::uint16_t, std::uint8_t> colorCounts;
        for (int i = 0; i < kPlayerCountMax; ++i) {
            if (block[0x61 + i * 0x24] != 0) continue;
            std::uint8_t c = block[0x60 + i * 0x24];
            c = c == 0x13 ? 0x12 : c;
            const std::uint16_t key = static_cast<std::uint16_t>((c << 8) | (teams ? block[0x69 + i * 0x24] : block[0x63 + i * 0x24]));
            block[0x67 + 0x24 * i] = colorCounts[key]++;
        }
        block[0x8] = teams ? 1 : 0;
        block[0x61 + 2 * 0x24] = remotes >= 2 ? 0 : 3;
        block[0x61 + 3 * 0x24] = remotes >= 3 ? 0 : 3;
        block[0xE] = static_cast<std::uint8_t>(stageId >> 8);
        block[0xF] = static_cast<std::uint8_t>(stageId);
        const bool pauseAllowed = g.lastMode == slippi::Mode::Direct;
        block[2] = pauseAllowed ? (block[2] & 0xF7) : (block[2] | 0x8);
        if (g.lastMode != slippi::Mode::Direct && g.lastMode != slippi::Mode::Teams) altStageMode = 0;
        for (int i = 0; i < 4; ++i) {
            block[0x62 + i * 0x24] = 4;
            block[0x70 + i * 0x24] = 0;
            block[0x71 + i * 0x24] = 0;
        }
        static std::string lastSetup;
        char setup[128];
        std::snprintf(setup, sizeof setup, "stage %u rng %08X P1 char %u P2 char %u", stageId, rngOffset, block[0x60],
                      block[0x60 + 0x24]);
        if (lastSetup != setup) {
            lastSetup = setup;
            Log("match setup %s", setup);
        }
    }
    const std::uint32_t timer = g.lastMode == slippi::Mode::Party ? 5 * 60 : 8 * 60;
    block[0x0] = g.lastMode == slippi::Mode::Party ? 0x12 : 0x32;
    block[0x3] = g.lastMode == slippi::Mode::Party ? 0xCC : 0x4C;
    block[0x10] = static_cast<std::uint8_t>(timer >> 24);
    block[0x11] = static_cast<std::uint8_t>(timer >> 16);
    block[0x12] = static_cast<std::uint8_t>(timer >> 8);
    block[0x13] = static_cast<std::uint8_t>(timer);
    block[0xB] = 0xFF;
    std::uint64_t items = 0xF80000000F000000ull;
    if (g.ticket.items != 0) {
        items |= static_cast<std::uint64_t>(g.ticket.items) << 28;
        block[0xB] = 3;
    }
    for (int i = 0; i < 8; ++i) block[0x23 + i] = static_cast<std::uint8_t>(items >> (56 - 8 * i));
    AppendU32(q, rngOffset);
    q.push_back(static_cast<std::uint8_t>(g.delay));
    q.push_back(sentChat);
    q.push_back(chatId);
    q.push_back(chatPlayer);
    q.push_back(0);  // ranks: none shown
    q.push_back(0);
    const std::string ln = GameText(localName, 15);
    q.insert(q.end(), ln.begin(), ln.end());
    for (int i = 0; i < 4; ++i) {
        const std::string n = GameText(PlayerName(i), 15);
        q.insert(q.end(), n.begin(), n.end());
    }
    std::vector<std::string> opponents;
    const int teamIdx = block[0x69 + g.localPlayerIndex * 0x24];
    for (int i = 0; i < 4; ++i) {
        const bool human = block[0x61 + i * 0x24] == 0;
        const bool same = block[0x69 + i * 0x24] == teamIdx && g.lastMode == slippi::Mode::Teams;
        if (g.localPlayerIndex == i || !human || same) continue;
        const std::string n = PlayerName(i);
        if (!n.empty()) opponents.push_back(n);
    }
    const size_t numOpp = opponents.empty() ? 1 : opponents.size();
    const int charsPer = static_cast<int>((15 - (numOpp - 1)) / numOpp);
    std::string oppText;
    for (const auto& n : opponents) {
        if (!oppText.empty()) oppText += "/";
        oppText += TruncateChars(n, charsPer);
    }
    const std::string opp = GameText(oppText, 15);
    q.insert(q.end(), opp.begin(), opp.end());
    for (int i = 0; i < 4; ++i) {
        const std::string c =
            slippi::ConnectCodeForGame(i < static_cast<int>(g.ticket.players.size()) ? g.ticket.players[static_cast<size_t>(i)].connectCode : "");
        q.insert(q.end(), c.begin(), c.end());
    }
    for (int i = 0; i < 4; ++i) {
        std::string uid = i < static_cast<int>(g.ticket.players.size()) ? g.ticket.players[static_cast<size_t>(i)].uid : "";
        uid.resize(29);
        q.insert(q.end(), uid.begin(), uid.end());
    }
    const std::string err = GameText(!g.forcedError.empty() ? g.forcedError : (g.matchmaking ? g.matchmaking->Error() : ""), 120);
    q.insert(q.end(), err.begin(), err.end());
    q.insert(q.end(), block.begin(), block.end());
    std::string matchId = g.ticket.matchId;
    matchId.resize(51);
    q.insert(q.end(), matchId.begin(), matchId.end());
    q.push_back(static_cast<std::uint8_t>(altStageMode));
}

void PrepareOnlineStatus(std::vector<std::uint8_t>& q) {
    q.clear();
    g.user.Load();
    q.push_back(g.user.loggedIn ? 1 : 0);
    const std::string name = GameText(g.user.account.displayName, 15);
    q.insert(q.end(), name.begin(), name.end());
    const std::string code = slippi::ConnectCodeForGame(g.user.account.connectCode);
    q.insert(q.end(), code.begin(), code.end());
}

void HandleReportGame(const std::uint8_t* p) {
    const std::uint32_t frames = Be32(p + 1);
    Log("game report: mode %u, %u frames, winner %d, end %u; %llu rollbacks", p[0], frames, static_cast<std::int8_t>(p[13]),
        p[14], static_cast<unsigned long long>(g.rollbacks));
    // shortcut: Slippi's game report upload (slippi_report.cpp) is not ported; results stay local.
}

void HandlePlayerSettings(std::vector<std::uint8_t>& q) {
    static const char* const kDefaults[16] = {"ggs", "one more", "brb", "good luck", "well played", "that was fun",
                                              "thanks", "too good", "sorry", "my b", "lol", "wow", "gotta go",
                                              "one sec", "let's play again later", "bad connection"};
    q.clear();
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 16; ++j) {
            std::string s = GameText(kDefaults[j], 25);
            s.resize(51);
            q.insert(q.end(), s.begin(), s.end());
        }
    }
}

bool PayloadValid(std::uint8_t cmd, const std::uint8_t* payload, std::uint32_t len) {
    std::uint32_t expected = 0;
    switch (cmd) {
        case CMD_ONLINE_INPUTS: expected = 25; break;
        case CMD_CAPTURE_SAVESTATE: case CMD_LOAD_SAVESTATE: expected = 32; break;
        case CMD_FIND_OPPONENT: expected = 19; break;
        case CMD_SET_MATCH_SELECTIONS: expected = 9; break;
        case CMD_SEND_CHAT_MESSAGE: expected = 2; break;
        case CMD_REPORT_GAME: expected = 368; break;
        case CMD_FETCH_CODE_SUGGESTION: expected = 31; break;
        case CMD_OVERWRITE_SELECTIONS: expected = 14; break;
        case CMD_GP_COMPLETE_STEP: expected = 5; break;
        case CMD_GP_FETCH_STEP: case CMD_REPORT_SET_COMPLETE: case CMD_REPORT_MATCH_STATUS_UPDATE: expected = 1; break;
        default: expected = 0; break;
    }
    return len == expected && (expected == 0 || payload);
}

bool Handle(std::uint8_t cmd, const std::uint8_t* p, std::vector<std::uint8_t>& q) {
    switch (cmd) {
        case CMD_ONLINE_INPUTS: HandleOnlineInputs(p, q); return true;
        case CMD_CAPTURE_SAVESTATE:
            if (!Disconnected()) melee_nx::rollback::SlotCapture(static_cast<std::int32_t>(Be32(p)));
            return true;
        case CMD_LOAD_SAVESTATE:
            // The preserve-block list (p+4..) names console addresses the native game has no use for:
            // its online bookkeeping is outside the snapshot (melee_game_host) by construction.
            if (melee_nx::rollback::SlotLoad(static_cast<std::int32_t>(Be32(p)))) ++g.rollbacks;
            else Log("no savestate for frame %d", static_cast<int>(Be32(p)));
            return true;
        case CMD_GET_MATCH_STATE: PrepareMatchState(q); return true;
        case CMD_FIND_OPPONENT: StartFindMatch(p); return true;
        case CMD_SET_MATCH_SELECTIONS: SetMatchSelections(p); return true;
        case CMD_OPEN_LOGIN: g.user.Load(); return true;
        case CMD_LOGOUT: return true;  // the launcher owns the account
        case CMD_UPDATE: return true;
        case CMD_GET_ONLINE_STATUS: PrepareOnlineStatus(q); return true;
        case CMD_CLEANUP_CONNECTION: CleanupConnection(); return true;
        case CMD_SEND_CHAT_MESSAGE:
            if (g.netplay) {
                g.netplay->SendChatMessage(p[0]);
                g.sentChat = p[0];
            }
            return true;
        case CMD_GET_NEW_SEED: q.clear(); AppendU32(q, g.rng() % 0xFFFFFFFFu); return true;
        case CMD_REPORT_GAME: HandleReportGame(p); return true;
        case CMD_FETCH_CODE_SUGGESTION: HandleNameEntryLoad(p, q); return true;
        case CMD_OVERWRITE_SELECTIONS: {
            g.overwrite.clear();
            const std::uint16_t stage = Be16(p);
            for (int i = 0; i < 4; ++i) {
                const std::uint8_t* c = p + 2 + i * 3;
                if (!c[0]) continue;
                slippi::PlayerSelections s;
                s.isCharacterSelected = true;
                s.characterId = c[1];
                s.characterColor = c[2];
                s.isStageSelected = true;
                s.stageId = stage;
                s.playerIdx = static_cast<std::uint8_t>(i);
                g.overwrite.push_back(s);
            }
            return true;
        }
        case CMD_GP_COMPLETE_STEP: case CMD_REPORT_SET_COMPLETE: case CMD_REPORT_MATCH_STATUS_UPDATE: return true;
        case CMD_GP_FETCH_STEP: q.assign(6, 0); return true;
        case CMD_GET_PLAYER_SETTINGS: HandlePlayerSettings(q); return true;
        case CMD_GET_DELAY:
            q.clear();
            q.push_back(g.matchmaking ? 1 : 0);
            q.push_back(g.matchmaking ? static_cast<std::uint8_t>(g.delay) : 0);
            return true;
        case CMD_GET_RANK: q.assign(16, 0); q[1] = 1; return true;
        case CMD_FETCH_RANK: return true;
        case CMD_GET_RANK_VISIBILITY: q.assign(1, 0); return true;
        default: return false;
    }
}

}  // namespace

extern "C" std::int32_t mu_slippi_command(std::uint8_t command, const std::uint8_t* payload, std::uint32_t payloadSize,
                                          std::uint8_t* response, std::uint32_t capacity, std::uint32_t* responseSize) {
    if (!responseSize) return -2;
    *responseSize = 0;
    Init();
    if (!PayloadValid(command, payload, payloadSize)) {
        Log("bad payload for command %02X (%u bytes)", command, payloadSize);
        return -2;
    }
    std::vector<std::uint8_t> reply;
    if (!Handle(command, payload, reply)) return -1;
    if (reply.size() > capacity || (!reply.empty() && !response)) return -2;
    if (!reply.empty()) std::memcpy(response, reply.data(), reply.size());
    *responseSize = static_cast<std::uint32_t>(reply.size());
    return 0;
}

extern "C" void melee_nx_slippi_resim_phase(std::int32_t entering) { g.resim = entering != 0; }
extern "C" std::uint32_t melee_nx_slippi_retrace_count(void) { return VIGetRetraceCount(); }
extern "C" void melee_nx_slippi_idle(void) { std::this_thread::sleep_for(std::chrono::microseconds(300)); }
extern "C" void melee_nx_slippi_log(const char* text) { std::printf("[slippi game] %s\n", text ? text : ""); }

// ---- the lobby: Direct mode without Slippi's menus ----------------------------------------------
// Slippi's own online menus need its disc files (SlippiCSS.dat), so the opponent's code comes from
// the launcher and VS mode's own character and stage select pick the rest. The game calls this as
// the stage select is left; it presents frames itself while it waits.

namespace {

void Status(const std::string& text, std::uint32_t frames) {
    g.status = text;
    g.statusUntil = VIGetRetraceCount() + frames;
}

// "ABCD#123" as the game's name-entry text: nine full-width Shift-JIS characters.
void CodeToGameText(const std::string& code, std::uint8_t out[18]) {
    std::memset(out, 0, 18);
    size_t n = 0;
    for (char c : code) {
        std::uint16_t w = 0;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') w = static_cast<std::uint16_t>(0x8260 + (c - 'A'));
        else if (c >= '0' && c <= '9') w = static_cast<std::uint16_t>(0x824F + (c - '0'));
        else if (c == '#') w = 0x8194;
        if (!w || n + 2 > 18) continue;
        out[n++] = static_cast<std::uint8_t>(w >> 8);
        out[n++] = static_cast<std::uint8_t>(w);
    }
}

// Present `frames` frames; true when B was pressed on `padPort` meanwhile.
bool WaitFrames(int frames, int padPort, bool* held) {
    bool pressed = false;
    for (int i = 0; i < frames; ++i) {
        VIWaitForRetrace();
        PadReport pads[4] = {};
        PADRead(pads);
        const bool b = padPort >= 0 && padPort < 4 && pads[padPort].err == 0 && (pads[padPort].button & 0x0200) != 0;
        if (b && !*held) pressed = true;
        *held = b;
    }
    return pressed;
}

}  // namespace

extern "C" std::int32_t melee_nx_slippi_enabled(void) {
    Init();
    return g.enabled ? 1 : 0;
}

extern "C" const char* melee_nx_slippi_status_text(void) {
    return !g.status.empty() && static_cast<std::int32_t>(g.statusUntil - VIGetRetraceCount()) > 0 ? g.status.c_str() : "";
}

extern "C" std::int32_t melee_nx_slippi_lobby_open(void) { return g.lobbyOpen ? 1 : 0; }

// What to do next, on the character select: nothing else on screen says the game is in Slippi mode.
extern "C" void melee_nx_slippi_hint(std::uint32_t frames) {
    Init();
    if (!g.enabled) return;
    Status(g.opponentCode.empty() ? "Slippi Unranked  -  pick a character, press Start, then pick any stage to search"
                                  : "Slippi Direct vs " + g.opponentCode + "  -  pick a character, press Start, then pick a stage",
           frames);
}
extern "C" std::int32_t melee_nx_slippi_mode(void) { return static_cast<std::int32_t>(g.lastMode); }

extern "C" void melee_nx_slippi_status(const char* text, std::uint32_t frames) { Status(text ? text : "", frames); }

extern "C" std::int32_t melee_nx_slippi_lobby(std::uint8_t character, std::uint8_t color, std::uint16_t stage,
                                              std::int32_t padPort, std::uint8_t* state, std::uint32_t capacity) {
    Init();
    // While this waits nothing draws: the port holds the last frame under the status line.
    struct Open {
        Open() { g.lobbyOpen = true; }
        ~Open() { g.lobbyOpen = false; }
    } open;
    constexpr std::uint32_t kForever = 0x40000000;
    bool held = true;  // the button that left the stage select may still be down
    const auto fail = [&](const std::string& why) {
        Log("lobby: %s", why.c_str());
        Status(why, kForever);
        WaitFrames(180, padPort, &held);
        Status("", 0);
        CleanupConnection();
        return 0;
    };
    const bool direct = !g.opponentCode.empty();
    const auto search = [&] {
        std::uint8_t find[19] = {static_cast<std::uint8_t>(direct ? slippi::Mode::Direct : slippi::Mode::Unranked)};
        if (direct) CodeToGameText(g.opponentCode, find + 1);
        StartFindMatch(find);
    };
    // Direct, stage option 1: this player's stage is a pick. The first picked stage in port order is
    // played, which is how Slippi Dolphin reads the same selections. Unranked, option 3: a random
    // stage from the server's list, as Slippi's own character select sends; the pick is not used.
    const std::uint8_t selections[9] = {0, character, color, 1, static_cast<std::uint8_t>(stage >> 8),
                                        static_cast<std::uint8_t>(stage), static_cast<std::uint8_t>(direct ? 1 : 3), 0, 0};
    // Still connected after the last game: the same opponent, no new search. The picks go out once,
    // and only on an open connection: connecting draws a random stage into the stored selections,
    // which the other game must never see as this player's pick.
    bool sentConnected = g.netplay && !Disconnected();
    if (sentConnected) {
        Log("lobby: rematch on the open connection");
        SetMatchSelections(selections);
    } else {
        CleanupConnection();
        search();
    }
    std::vector<std::uint8_t> q;
    for (;;) {
        PrepareMatchState(q);
        const auto st = static_cast<slippi::Matchmaking::State>(q[0]);
        if (st == slippi::Matchmaking::State::Error) {
            const std::string why = !g.forcedError.empty() ? g.forcedError : (g.matchmaking ? g.matchmaking->Error() : "");
            return fail(why.empty() ? "Could not connect" : why);
        }
        if (st == slippi::Matchmaking::State::Idle) return fail("The opponent left");
        if (st == slippi::Matchmaking::State::ConnectionSuccess) {
            if (!sentConnected) {
                SetMatchSelections(selections);
                sentConnected = true;
                continue;
            }
            if (q[1] && q[2] && q.size() <= capacity) {
                std::memcpy(state, q.data(), q.size());
                Status("", 0);
                Log("lobby: match ready, local port %d", static_cast<int>(q[3]));
                return 1;
            }
            Status("Connected to " + PlayerName(g.remotePlayerIndex) + "  -  waiting for their pick  (B: cancel)", kForever);
        } else {
            Status(direct ? "Looking for " + g.opponentCode + "  (B: cancel)" : "Searching for an Unranked match  (B: cancel)", kForever);
        }
        if (WaitFrames(1, padPort, &held)) {
            Log("lobby: cancelled");
            Status("", 0);
            CleanupConnection();
            return 0;
        }
    }
}

// Tests: account files from `dir`, and peering without the server.
extern "C" void melee_nx_slippi_test_setup(const char* dir, std::int32_t localIndex, std::uint16_t localPort,
                                           const char* remote, const char* opponentCode) {
    gDir = dir;
    gLocalPeer.enabled = true;
    gLocalPeer.localIndex = localIndex;
    gLocalPeer.localPort = localPort;
    gLocalPeer.remotes = {remote};
    Init();
    g.user.Load();
    g.opponentCode = opponentCode;
    g.enabled = true;
}