#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace valstream {

enum class ErrorKind {
    InvalidArgument,  // bad region / parameters, never sent
    Transport,        // network failure, timeout, TLS, ...
    RateLimited,      // 429 retries exhausted or limiter wait exceeded the timeout
    BadRequest,       // 400
    Unauthorized,     // 401: missing/invalid key
    Forbidden,        // 403: key expired or endpoint not enabled for this key
    NotFound,         // 404
    ServerError,      // 5xx after retries exhausted
    Http,             // any other non-2xx
    Parse,            // body was not the JSON shape we expected
};

const char* toString(ErrorKind kind);

struct ApiError {
    ErrorKind kind;
    int httpStatus = 0;  // 0 when no HTTP response was received
    std::string message;
};

/// Minimal expected-style result (std::expected is C++23; this project targets C++20).
template <class T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}           // NOLINT(google-explicit-constructor)
    Result(ApiError error) : v_(std::move(error)) {}    // NOLINT(google-explicit-constructor)

    bool ok() const { return v_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() & { return std::get<0>(v_); }
    const T& value() const& { return std::get<0>(v_); }
    T&& value() && { return std::get<0>(std::move(v_)); }
    const ApiError& error() const { return std::get<1>(v_); }

    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    std::variant<T, ApiError> v_;
};

}  // namespace valstream
