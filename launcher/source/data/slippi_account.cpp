// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/slippi_account.h"

#include <cstdio>

#include <nlohmann/json.hpp>

#include "data/http.h"
#include "data/settings.h"

namespace mnx::slippi {
namespace {

constexpr const char* kUserApi = "https://users-rest-dot-slippi.uc.r.appspot.com/user/";

std::string Text(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool UrlSafe(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

}  // namespace

bool ParseAccount(const std::string& text, Account& out, std::string& error) {
    if (text.size() > 16 * 1024) {
        error = "That file is too big to be a Slippi user.json.";
        return false;
    }
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) {
        error = "That is not a Slippi user.json (not JSON).";
        return false;
    }
    Account a;
    a.uid = Text(j, "uid");
    a.playKey = Text(j, "playKey");
    a.connectCode = Text(j, "connectCode");
    a.displayName = Text(j, "displayName");
    a.latestVersion = Text(j, "latestVersion");
    if (a.uid.empty() || a.playKey.empty()) {
        error = "This user.json has no uid or play key. Log in with the Slippi Launcher on a PC first.";
        return false;
    }
    if (a.connectCode.empty()) {
        error = "This account has no connect code yet. Finish setting it up in the Slippi Launcher.";
        return false;
    }
    if (!UrlSafe(a.uid)) {
        error = "The uid in this user.json is not valid.";
        return false;
    }
    if (a.connectCode.find('#') == std::string::npos || a.connectCode.size() > 16) {
        error = "The connect code in this user.json is not valid.";
        return false;
    }
    out = std::move(a);
    return true;
}

Loaded LoadAccount(const std::string& path) {
    Loaded l;
    std::string text;
    if (!ReadFile(path, text, 64 * 1024)) return l;
    l.state = ParseAccount(text, l.account, l.error) ? State::Valid : State::Invalid;
    return l;
}

bool SaveAccountText(const std::string& path, const std::string& text, std::string& error) {
    Account a;
    if (!ParseAccount(text, a, error)) return false;
    if (!WriteFileAtomic(path, text)) {
        error = "Could not write the account to the SD card.";
        return false;
    }
    return true;
}

bool DeleteAccount(const std::string& path) { return std::remove(path.c_str()) == 0; }

bool RefreshProfile(const std::string& path, Account& account, std::string& error) {
    if (!UrlSafe(account.uid)) {
        error = "No valid account to refresh.";
        return false;
    }
    http::Response r = http::Get(kUserApi + account.uid, 64 * 1024);
    if (!r.ok) {
        error = "Could not reach Slippi: " + r.error;
        return false;
    }
    if (r.status == 404) {
        error = "Slippi does not know this account. Log in again with the Slippi Launcher.";
        return false;
    }
    if (r.status != 200) {
        error = "Slippi answered HTTP " + std::to_string(r.status) + ". Try again later.";
        return false;
    }
    const auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object()) {
        error = "Slippi sent an unexpected answer.";
        return false;
    }
    const std::string name = Text(j, "displayName"), code = Text(j, "connectCode"), latest = Text(j, "latestVersion");
    if (!name.empty()) account.displayName = name;
    if (!code.empty()) account.connectCode = code;
    if (!latest.empty()) account.latestVersion = latest;

    // Keep every field the Slippi Launcher wrote; only these three come from the server.
    std::string text;
    if (ReadFile(path, text, 64 * 1024)) {
        auto saved = nlohmann::json::parse(text, nullptr, false);
        if (saved.is_object()) {
            saved["displayName"] = account.displayName;
            saved["connectCode"] = account.connectCode;
            if (!account.latestVersion.empty()) saved["latestVersion"] = account.latestVersion;
            WriteFileAtomic(path, saved.dump(2) + "\n");
        }
    }
    return true;
}

}  // namespace mnx::slippi
