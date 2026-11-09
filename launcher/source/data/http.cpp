#include "data/http.h"

#include <curl/curl.h>

namespace mnx::http {
namespace {

struct Sink {
    std::string* body;
    size_t max;
    bool overflow = false;
};

size_t Write(char* data, size_t size, size_t count, void* user) {
    auto* s = static_cast<Sink*>(user);
    const size_t n = size * count;
    if (s->body->size() + n > s->max) {
        s->overflow = true;
        return 0;  // aborts the transfer
    }
    s->body->append(data, n);
    return n;
}

}  // namespace

Response Get(const std::string& url, size_t maxBytes) {
    static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    Response r;
    if (!ready) {
        r.error = "network library failed to start";
        return r;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.error = "network library failed to start";
        return r;
    }
    Sink sink{&r.body, maxBytes};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MeleeNX-Launcher/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        r.ok = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    } else {
        r.error = sink.overflow ? "the answer was too large" : curl_easy_strerror(rc);
    }
    curl_easy_cleanup(curl);
    return r;
}

}  // namespace mnx::http
