// Small HTTPS GET over libcurl. On the Switch, devkitPro's curl verifies certificates against the
// console's own trust store, so peer verification stays on.
#pragma once

#include <string>

namespace mnx::http {

struct Response {
    bool ok = false;     // the request completed (any HTTP status)
    long status = 0;
    std::string body;
    std::string error;
};

// Gives up after 15 seconds or when the body passes `maxBytes`.
Response Get(const std::string& url, size_t maxBytes);

}  // namespace mnx::http
