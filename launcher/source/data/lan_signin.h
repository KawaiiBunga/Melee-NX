// Sign in to Slippi from a phone or PC on the same Wi-Fi.
//
// Slippi has no device-code login a console can use, so the Switch cannot log in by itself. The
// account is the user.json the Slippi Launcher writes when you log in on a PC. This server lets you
// hand that file over without touching the SD card: the launcher shows an address and a 6-digit
// code, you open the address in a browser, pick (or paste) user.json and type the code. The server
// takes one valid upload and stops; it also stops after 10 minutes or 5 wrong codes.
//
// It is plain HTTP on your local network, so only use it on a network you trust.
#pragma once

#include <map>
#include <string>

#include "data/job.h"

namespace mnx::signin {

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;  // names lower-cased
    std::string body;
};

enum class Parse { Incomplete, Ok, Bad, TooLarge };
// `raw` is everything read from the connection so far.
Parse ParseRequest(const std::string& raw, Request& out, size_t maxBody);

struct Outcome {
    std::string response;     // full HTTP response to send
    bool accepted = false;    // a valid account arrived; `accountText` holds it
    bool wrongCode = false;   // counts toward the lockout
    std::string accountText;
    std::string summary;      // "Name (CODE#123)" for the launcher, never the key
};
// Routes one request. `save` stores a validated account and returns false (with error) if it cannot.
using SaveFn = bool (*)(const std::string& text, std::string& error);
Outcome Handle(const Request& req, const std::string& pairCode, SaveFn save);

// A random 6-digit code.
std::string NewPairCode();

// The listening socket and its address. Open() binds the first free port in 8080..8089.
class Server {
public:
    ~Server();
    bool Open(std::string& error);
    const std::string& Url() const { return url_; }
    // Serves until one account is saved, cancel, timeout or lockout. Runs on the job thread.
    Job::Result Serve(Job::Reporter& r, const std::string& pairCode, SaveFn save);
    void Close();

private:
    int fd_ = -1;
    std::string url_;
};

}  // namespace mnx::signin
