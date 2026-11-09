// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/lan_signin.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <random>

#include "data/slippi_account.h"

#ifndef MNX_NO_SOCKETS
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mnx::signin {
namespace {

constexpr size_t kMaxHeader = 8 * 1024;
[[maybe_unused]] constexpr size_t kMaxBody = 16 * 1024;
[[maybe_unused]] constexpr int kMaxWrongCodes = 5;
[[maybe_unused]] constexpr int kServeSeconds = 600;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Reply(int status, const char* reason, const char* type, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + type +
           "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}

const char kPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Melee NX</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#120f2a;color:#f3f1ff;display:flex;justify-content:center}
main{max-width:460px;width:100%;padding:28px 18px}
h1{font-size:22px;margin:0 0 20px}
ol{padding-left:20px;margin:0 0 22px;line-height:1.6;color:#b4add9}
code{background:#262050;color:#f3f1ff;padding:2px 6px;border-radius:6px;font-size:13px;word-break:break-all}
input{width:100%;box-sizing:border-box;background:#262050;color:#f3f1ff;border:1px solid #3e3675;border-radius:10px;padding:14px;font-size:17px;margin-bottom:12px}
#c{text-align:center;letter-spacing:8px;font-size:24px}
button{width:100%;padding:15px;border:0;border-radius:12px;background:#b6f23d;color:#120f2a;font-size:17px;font-weight:700}
details{margin:-2px 0 14px;color:#b4add9}textarea{width:100%;box-sizing:border-box;height:90px;margin-top:8px;background:#262050;color:#f3f1ff;border:1px solid #3e3675;border-radius:10px}
#msg{margin-top:16px;text-align:center;font-weight:600}
</style></head><body><main>
<h1>Sign in to Slippi</h1>
<ol>
<li>Log in to the Slippi Launcher on your PC.</li>
<li>Find <b>user.json</b>:<br>
Windows: press Win+R and paste<br><code>%AppData%\Slippi Launcher\netplay\User\Slippi</code><br>
Linux: <code>~/.config/Slippi Launcher/netplay/User/Slippi</code><br>
Mac: search Finder for <code>user.json</code></li>
<li>Pick it below and type the code from your Switch.</li>
</ol>
<input id="f" type="file" accept=".json,application/json">
<details><summary>Paste it instead</summary><textarea id="t" spellcheck="false"></textarea></details>
<input id="c" inputmode="numeric" maxlength="6" autocomplete="off" placeholder="000000">
<button id="go">Sign in</button><div id="msg"></div>
<script>
const m=document.getElementById('msg');
document.getElementById('go').onclick=async()=>{
 const f=document.getElementById('f').files[0];
 const text=f?await f.text():document.getElementById('t').value.trim();
 if(!text){m.textContent='Pick user.json first.';return;}
 m.textContent='Sending...';
 try{const r=await fetch('/account',{method:'POST',headers:{'Content-Type':'application/json','X-Pair-Code':document.getElementById('c').value.trim()},body:text});
 m.textContent=await r.text();}catch(e){m.textContent='Could not reach the Switch.';}
};
</script></main></body></html>
)HTML";

bool CodesEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

}  // namespace

Parse ParseRequest(const std::string& raw, Request& out, size_t maxBody) {
    const size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos) return raw.size() > kMaxHeader ? Parse::TooLarge : Parse::Incomplete;
    if (end > kMaxHeader) return Parse::TooLarge;
    Request req;
    size_t lineEnd = raw.find("\r\n");
    const std::string first = raw.substr(0, lineEnd);
    const size_t s1 = first.find(' '), s2 = first.find(' ', s1 == std::string::npos ? 0 : s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) return Parse::Bad;
    req.method = first.substr(0, s1);
    req.path = first.substr(s1 + 1, s2 - s1 - 1);
    if (first.compare(s2 + 1, 5, "HTTP/") != 0) return Parse::Bad;
    size_t pos = lineEnd + 2;
    while (pos < end) {
        const size_t e = raw.find("\r\n", pos);
        const std::string line = raw.substr(pos, e - pos);
        pos = e + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return Parse::Bad;
        size_t v = colon + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
        req.headers[Lower(line.substr(0, colon))] = line.substr(v);
    }
    size_t length = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
        if (it->second.empty() || it->second.size() > 9) return Parse::Bad;
        for (char c : it->second) {
            if (c < '0' || c > '9') return Parse::Bad;
        }
        length = static_cast<size_t>(std::stoul(it->second));
    }
    if (req.headers.count("transfer-encoding")) return Parse::Bad;  // chunked uploads are not needed
    if (length > maxBody) return Parse::TooLarge;
    if (raw.size() - (end + 4) < length) return Parse::Incomplete;
    req.body = raw.substr(end + 4, length);
    out = std::move(req);
    return Parse::Ok;
}

Outcome Handle(const Request& req, const std::string& pairCode, SaveFn save) {
    Outcome o;
    if (req.method == "GET" && (req.path == "/" || req.path.rfind("/?", 0) == 0)) {
        o.response = Reply(200, "OK", "text/html; charset=utf-8", kPage);
        return o;
    }
    if (req.method != "POST" || req.path != "/account") {
        o.response = Reply(404, "Not Found", "text/plain; charset=utf-8", "Not found.");
        return o;
    }
    const auto code = req.headers.find("x-pair-code");
    if (code == req.headers.end() || !CodesEqual(code->second, pairCode)) {
        o.wrongCode = true;
        o.response = Reply(403, "Forbidden", "text/plain; charset=utf-8",
                           "Wrong code.");
        return o;
    }
    slippi::Account account;
    std::string error;
    if (!slippi::ParseAccount(req.body, account, error)) {
        o.response = Reply(400, "Bad Request", "text/plain; charset=utf-8", error);
        return o;
    }
    if (save && !save(req.body, error)) {
        o.response = Reply(500, "Internal Server Error", "text/plain; charset=utf-8", error);
        return o;
    }
    o.accepted = true;
    o.accountText = req.body;
    o.summary = (account.displayName.empty() ? std::string("Slippi user") : account.displayName) + " (" +
                account.connectCode + ")";
    o.response = Reply(200, "OK", "text/plain; charset=utf-8",
                       "Signed in as " + o.summary + ". You can close this page.");
    return o;
}

std::string NewPairCode() {
    std::random_device rd;
    std::uniform_int_distribution<int> digit(0, 9);
    std::string code;
    for (int i = 0; i < 6; ++i) code.push_back(static_cast<char>('0' + digit(rd)));
    return code;
}

#ifndef MNX_NO_SOCKETS

Server::~Server() { Close(); }

void Server::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Server::Open(std::string& error) {
    Close();
    const long host = gethostid();  // libnx: the console's IPv4 address, network byte order
    if (host == 0 || host == 0x7F000001L || host == 0x0100007FL) {
        error = "The Switch is not connected to a network.";
        return false;
    }
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        error = "Could not open a network socket.";
        return false;
    }
    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    for (int port = 8080; port < 8090; ++port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && ::listen(fd_, 4) == 0) {
            in_addr ip{};
            ip.s_addr = static_cast<in_addr_t>(host);
            url_ = std::string("http://") + inet_ntoa(ip) + ":" + std::to_string(port);
            return true;
        }
    }
    Close();
    error = "Could not listen on ports 8080-8089.";
    return false;
}

Job::Result Server::Serve(Job::Reporter& r, const std::string& pairCode, SaveFn save) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(kServeSeconds);
    int wrong = 0;
    r.Stage("Waiting for your phone or PC...");
    while (!r.Cancelled()) {
        if (clock::now() > deadline) return {false, "Sign-in timed out after 10 minutes."};
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd_, &set);
        timeval tv{0, 250000};
        if (::select(fd_ + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
        const int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) continue;
        timeval rto{5, 0};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof rto);
        std::string raw;
        Request req;
        Parse p = Parse::Incomplete;
        char buf[2048];
        while (p == Parse::Incomplete) {
            const ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
            p = ParseRequest(raw, req, kMaxBody);
        }
        Outcome o;
        if (p == Parse::Ok) {
            o = Handle(req, pairCode, save);
        } else {
            o.response = Reply(p == Parse::TooLarge ? 413 : 400, "Bad Request", "text/plain; charset=utf-8",
                               "Bad request.");
        }
        size_t sent = 0;
        while (sent < o.response.size()) {
            const ssize_t n = ::send(c, o.response.data() + sent, o.response.size() - sent, 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        ::close(c);
        if (o.accepted) return {true, "Signed in as " + o.summary + "."};
        if (o.wrongCode && ++wrong >= kMaxWrongCodes) return {false, "Stopped after 5 wrong codes."};
        if (o.wrongCode) r.Stage("Wrong code entered (" + std::to_string(wrong) + " of 5). Waiting...");
    }
    return {false, "Sign-in cancelled."};
}

#endif  // MNX_NO_SOCKETS

}  // namespace mnx::signin
