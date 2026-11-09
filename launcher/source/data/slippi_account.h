// The Slippi account on the Switch: the same user.json the Slippi Launcher writes on a PC
// (uid, playKey, connectCode, displayName, latestVersion), kept at
// sdmc:/switch/melee-nx/slippi/user.json. Same rules as Melee Unlocked's account reader
// (port/runtime/hle/slippi_account.h): a profile needs a nonempty uid, playKey and connectCode.
//
// The play key is a secret. Nothing here logs it, shows it or puts it in a URL.
#pragma once

#include <string>

namespace mnx::slippi {

struct Account {
    std::string uid;
    std::string playKey;
    std::string connectCode;
    std::string displayName;
    std::string latestVersion;
};

// Parses user.json text. False with a user-facing `error` when it is not a usable profile.
bool ParseAccount(const std::string& text, Account& out, std::string& error);

enum class State { None, Valid, Invalid };
struct Loaded {
    State state = State::None;
    Account account;
    std::string error;
};
Loaded LoadAccount(const std::string& path);

// Saves validated user.json text as-is (other fields the Slippi Launcher wrote are kept).
bool SaveAccountText(const std::string& path, const std::string& text, std::string& error);
bool DeleteAccount(const std::string& path);

// Asks Slippi's public user service for the account's current name, code and the Slippi version
// online play needs, and writes them back into user.json. This proves the uid exists; it does not
// check the play key (only the matchmaking server does that).
bool RefreshProfile(const std::string& path, Account& account, std::string& error);

}  // namespace mnx::slippi
