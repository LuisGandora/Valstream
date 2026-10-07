#include <cpr/cpr.h>

#include "valstream/http.hpp"

namespace valstream {

HttpResponse CprTransport::get(const HttpRequest& request) {
    cpr::Header headers;
    for (const auto& [k, v] : request.headers) headers[k] = v;

    cpr::Response r = cpr::Get(cpr::Url{request.url}, headers, cpr::Timeout{std::chrono::seconds(10)});

    HttpResponse out;
    if (r.error) {
        out.error = r.error.message;
        return out;
    }
    out.status = static_cast<int>(r.status_code);
    for (const auto& [k, v] : r.header) out.headers[k] = v;
    out.body = std::move(r.text);
    return out;
}

}  // namespace valstream
