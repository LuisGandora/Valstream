#pragma once

#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "valstream/http.hpp"

namespace valstream::test {

inline std::string fixturePath(const std::string& relative) { return std::string(VALSTREAM_FIXTURES_DIR) + "/" + relative; }

inline nlohmann::json readFixture(const std::string& relative) {
    std::ifstream in(fixturePath(relative));
    return nlohmann::json::parse(in);
}

inline HttpResponse response(int status, const nlohmann::json& body = nlohmann::json::object(), Headers headers = {}) {
    return HttpResponse{status, std::move(headers), body.dump(), ""};
}

/// Loads a recorded response of the form {"status": 429, "headers": {...}, "body": {...}}.
inline HttpResponse recordedResponse(const std::string& relative) {
    auto j = readFixture(relative);
    Headers headers;
    for (auto& [k, v] : j["headers"].items()) headers[k] = v.get<std::string>();
    return response(j["status"].get<int>(), j["body"], headers);
}

/// Returns queued responses in order and records every request. Thread-safe.
class ScriptedTransport final : public HttpTransport {
public:
    void push(HttpResponse r) {
        std::lock_guard lock(mutex_);
        responses_.push_back(std::move(r));
    }
    HttpResponse get(const HttpRequest& request) override {
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
        if (responses_.empty()) return HttpResponse{0, {}, "", "no scripted response"};
        auto r = std::move(responses_.front());
        responses_.pop_front();
        return r;
    }
    std::vector<HttpRequest> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<HttpResponse> responses_;
    std::vector<HttpRequest> requests_;
};

}  // namespace valstream::test
