#include "valstream/riot_client.hpp"

#include <algorithm>
#include <charconv>

#include <spdlog/spdlog.h>

namespace valstream {

using nlohmann::json;

namespace {

std::string urlEncode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

/// Retry-After in delta-seconds form (what Riot sends). HTTP-date form is not used by Riot and is ignored.
std::optional<std::chrono::seconds> parseRetryAfter(const Headers& headers) {
    auto it = headers.find("Retry-After");
    if (it == headers.end()) return std::nullopt;
    int seconds = 0;
    const auto& v = it->second;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), seconds);
    if (ec != std::errc{} || seconds < 0) return std::nullopt;
    return std::chrono::seconds(seconds);
}

/// Riot error bodies look like {"status": {"message": "Forbidden", "status_code": 403}}.
std::string errorMessage(const HttpResponse& resp) {
    auto j = json::parse(resp.body, nullptr, false);
    if (j.is_object() && j.contains("status") && j["status"].is_object()) {
        return j["status"].value("message", "");
    }
    return "HTTP " + std::to_string(resp.status);
}

ErrorKind kindFor4xx(int status) {
    switch (status) {
        case 400: return ErrorKind::BadRequest;
        case 401: return ErrorKind::Unauthorized;
        case 403: return ErrorKind::Forbidden;
        case 404: return ErrorKind::NotFound;
        default: return ErrorKind::Http;
    }
}

template <class T>
Result<T> parseAs(Result<json> body) {
    if (!body) return body.error();
    try {
        return body.value().get<T>();
    } catch (const std::exception& e) {
        return ApiError{ErrorKind::Parse, 200, std::string("unexpected response shape: ") + e.what()};
    }
}

}  // namespace

RiotClient::RiotClient(HttpTransport& transport, RateLimiter& limiter, Clock& clock, std::string apiKey,
                       RetryPolicy policy)
    : transport_(transport), limiter_(limiter), clock_(clock), apiKey_(std::move(apiKey)), policy_(policy) {}

Result<Content> RiotClient::getContent(std::string_view region, std::string_view locale) {
    return parseAs<Content>(get(region, kContentMethod, "/val/content/v1/contents?locale=" + urlEncode(locale)));
}

Result<Leaderboard> RiotClient::getLeaderboard(std::string_view region, std::string_view actId, int size,
                                               int startIndex) {
    if (actId.empty() || size < 1 || size > 200 || startIndex < 0) {
        return ApiError{ErrorKind::InvalidArgument, 0, "actId must be set, size in [1,200], startIndex >= 0"};
    }
    return parseAs<Leaderboard>(get(region, kLeaderboardMethod,
                                    "/val/ranked/v1/leaderboards/by-act/" + urlEncode(actId) +
                                        "?size=" + std::to_string(size) + "&startIndex=" + std::to_string(startIndex)));
}

Result<PlatformStatus> RiotClient::getStatus(std::string_view region) {
    auto body = get(region, kStatusMethod, "/val/status/v1/platform-data");
    if (!body) return body.error();
    if (!body->is_object()) return ApiError{ErrorKind::Parse, 200, "platform-data is not an object"};
    return PlatformStatus{body->value("id", ""), body->value("name", ""), std::move(body).value()};
}

Clock::duration RiotClient::backoff(int attempt) {
    auto base = policy_.baseDelay;
    std::chrono::milliseconds jitter;
    {
        std::lock_guard lock(rngMutex_);
        jitter = std::chrono::milliseconds(std::uniform_int_distribution<long long>(0, base.count())(rng_));
    }
    std::chrono::milliseconds exp = base * (1 << std::min(attempt, 20));
    exp = std::min(exp, policy_.maxDelay);
    return exp + jitter;
}

Result<json> RiotClient::get(std::string_view region, std::string_view method, const std::string& pathAndQuery) {
    if (!isValidRegion(region)) {
        return ApiError{ErrorKind::InvalidArgument, 0, "unknown region '" + std::string(region) + "'"};
    }
    HttpRequest request{"https://" + std::string(region) + ".api.riotgames.com" + pathAndQuery,
                        {{"X-Riot-Token", apiKey_}, {"Accept", "application/json"}}};

    ApiError last{ErrorKind::Transport, 0, "no attempts made"};
    for (int attempt = 0; attempt < policy_.maxAttempts; ++attempt) {
        if (!limiter_.acquire(region, method, policy_.acquireTimeout)) {
            return ApiError{ErrorKind::RateLimited, 0, "rate limiter wait exceeded timeout"};
        }
        HttpResponse resp = transport_.get(request);
        if (resp.status != 0) limiter_.syncFromHeaders(region, method, resp.headers);

        if (resp.status >= 200 && resp.status < 300) {
            auto j = json::parse(resp.body, nullptr, false);
            if (j.is_discarded()) return ApiError{ErrorKind::Parse, resp.status, "response body is not valid JSON"};
            return j;
        }

        if (resp.status == 429) {
            auto retryAfter = parseRetryAfter(resp.headers);
            auto typeIt = resp.headers.find("X-Rate-Limit-Type");
            bool appScope = typeIt != resp.headers.end() && typeIt->second == "application";
            spdlog::warn("[{}] {} -> 429 ({} limit), retry-after={}s, attempt {}/{}", region, pathAndQuery,
                         typeIt != resp.headers.end() ? typeIt->second : "unspecified",
                         retryAfter ? retryAfter->count() : -1, attempt + 1, policy_.maxAttempts);
            // Blocking the bucket (rather than just sleeping) also holds back other threads using it.
            // Service-level 429s are charged to the method bucket so we stop hammering that endpoint.
            limiter_.penalize({std::string(region), std::string(appScope ? RateLimiter::kAppScope : method)},
                              retryAfter.value_or(policy_.defaultRetryAfter));
            last = ApiError{ErrorKind::RateLimited, 429, errorMessage(resp)};
            continue;
        }

        if (resp.status == 0 || resp.status >= 500) {
            last = resp.status == 0 ? ApiError{ErrorKind::Transport, 0, resp.error}
                                    : ApiError{ErrorKind::ServerError, resp.status, errorMessage(resp)};
            spdlog::warn("[{}] {} -> {} ({}), attempt {}/{}", region, pathAndQuery, resp.status, last.message,
                         attempt + 1, policy_.maxAttempts);
            if (attempt + 1 < policy_.maxAttempts) clock_.sleepFor(backoff(attempt));
            continue;
        }

        return ApiError{kindFor4xx(resp.status), resp.status, errorMessage(resp)};
    }
    return last;
}

}  // namespace valstream
