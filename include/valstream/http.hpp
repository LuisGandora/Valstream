#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace valstream {

/// HTTP header names are case-insensitive (RFC 9110 §5.1).
struct CaseInsensitiveLess {
    bool operator()(std::string_view a, std::string_view b) const {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
    }
};

using Headers = std::map<std::string, std::string, CaseInsensitiveLess>;

struct HttpRequest {
    std::string url;
    Headers headers;
};

struct HttpResponse {
    int status = 0;     // 0 = no response (connection error, timeout, ...)
    Headers headers;
    std::string body;
    std::string error;  // transport-level error message when status == 0
};

/// The seam between RiotClient and the network. Implemented by CprTransport (live),
/// FixtureTransport (mock mode) and scripted fakes in tests.
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse get(const HttpRequest& request) = 0;
};

/// Live transport backed by libcurl via cpr. Never logs request headers (they carry the API key).
class CprTransport final : public HttpTransport {
public:
    HttpResponse get(const HttpRequest& request) override;
};

/// Mock-mode transport that answers from recorded JSON fixtures.
///
///   /val/content/v1/contents                 -> fixtures/content.json
///   /val/status/v1/platform-data             -> fixtures/status.json (id/name patched per region)
///   /val/ranked/v1/leaderboards/by-act/{id}  -> fixtures/leaderboard/{region}_{a,b}.json
///
/// Leaderboards alternate a, b, a, b, ... each time a region's first page (startIndex=0) is requested,
/// so a running service emits a steady stream of events. size/startIndex are honored by slicing.
class FixtureTransport final : public HttpTransport {
public:
    explicit FixtureTransport(std::filesystem::path dir);
    HttpResponse get(const HttpRequest& request) override;

private:
    std::filesystem::path dir_;
    std::mutex mutex_;
    std::map<std::string, int> cycle_;  // region -> number of first-page requests served
};

}  // namespace valstream
