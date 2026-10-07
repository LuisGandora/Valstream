#include "valstream/act_resolver.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace valstream {

ActResolver::ActResolver(RiotClient& client, Clock& clock, std::string region, std::string locale,
                         Clock::duration refreshInterval)
    : client_(client),
      clock_(clock),
      region_(std::move(region)),
      locale_(std::move(locale)),
      refreshInterval_(refreshInterval) {}

std::optional<Act> ActResolver::pickActiveAct(const std::vector<Act>& acts) {
    std::optional<Act> fallback;
    for (const auto& act : acts) {
        if (!act.isActive) continue;
        if (act.type == "act") return act;
        if (!act.type) fallback = act;  // keep the last untyped active entry
    }
    return fallback;
}

Result<std::string> ActResolver::currentActId() {
    std::lock_guard lock(mutex_);
    auto now = clock_.now();
    if (actId_ && now - fetchedAt_ < refreshInterval_) return *actId_;

    auto content = client_.getContent(region_, locale_);
    if (!content) {
        if (actId_) {
            spdlog::warn("act refresh failed ({}), keeping act {}", content.error().message, *actId_);
            return *actId_;
        }
        return content.error();
    }
    auto act = pickActiveAct(content->acts);
    if (!act) {
        if (actId_) return *actId_;
        return ApiError{ErrorKind::Parse, 200, "content lists no active act"};
    }
    if (actId_ != act->id) spdlog::info("active act: {} ({})", act->name, act->id);
    actId_ = act->id;
    fetchedAt_ = now;
    return *actId_;
}

}  // namespace valstream
