#include <gtest/gtest.h>

#include "test_util.hpp"
#include "valstream/act_resolver.hpp"

using namespace valstream;
using namespace valstream::test;
using namespace std::chrono_literals;

namespace {

Act act(std::string id, bool active, std::optional<std::string> type = std::nullopt) {
    return Act{std::move(id), "name", active, std::move(type), std::nullopt};
}

class ActResolverTest : public ::testing::Test {
protected:
    FakeClock clock;
    RateLimiter limiter{clock, {{100, 1s}}};
    ScriptedTransport transport;
    RiotClient client{transport, limiter, clock, "k", RetryPolicy{.maxAttempts = 1}};
    ActResolver resolver{client, clock, "na", "en-US", 1h};
};

}  // namespace

TEST(PickActiveAct, PrefersActiveActOverActiveEpisode) {
    auto picked = ActResolver::pickActiveAct(
        {act("ep9", false, "episode"), act("ep10", true, "episode"), act("a1", false, "act"), act("a2", true, "act")});
    ASSERT_TRUE(picked);
    EXPECT_EQ(picked->id, "a2");
}

TEST(PickActiveAct, FallsBackToLastActiveEntryWithoutTypes) {
    auto picked = ActResolver::pickActiveAct({act("old", false), act("episode", true), act("act", true)});
    ASSERT_TRUE(picked);
    EXPECT_EQ(picked->id, "act");
}

TEST(PickActiveAct, NeverPicksAnEpisode) {
    EXPECT_FALSE(ActResolver::pickActiveAct({act("ep", true, "episode"), act("a", false, "act")}));
}

TEST(PickActiveAct, NothingActive) {
    EXPECT_FALSE(ActResolver::pickActiveAct({}));
    EXPECT_FALSE(ActResolver::pickActiveAct({act("a", false), act("b", false, "act")}));
}

TEST_F(ActResolverTest, ResolvesFromContentFixture) {
    transport.push(response(200, readFixture("content.json")));
    auto id = resolver.currentActId();
    ASSERT_TRUE(id) << id.error().message;
    EXPECT_EQ(id.value(), "4c4b8cff-43eb-13d3-8f14-96b783c90cd2");
    EXPECT_EQ(transport.requests()[0].url, "https://na.api.riotgames.com/val/content/v1/contents?locale=en-US");
}

TEST_F(ActResolverTest, CachesUntilRefreshInterval) {
    auto content = readFixture("content.json");
    transport.push(response(200, content));
    ASSERT_TRUE(resolver.currentActId());
    clock.advance(59min);
    ASSERT_TRUE(resolver.currentActId());
    EXPECT_EQ(transport.requests().size(), 1u);

    // A new act starts: the next refresh picks it up.
    for (auto& a : content["acts"]) a["isActive"] = false;
    content["acts"].push_back({{"id", "next-act"}, {"name", "ACT III"}, {"isActive", true}, {"type", "act"}});
    transport.push(response(200, content));
    clock.advance(2min);
    auto id = resolver.currentActId();
    ASSERT_TRUE(id);
    EXPECT_EQ(id.value(), "next-act");
    EXPECT_EQ(transport.requests().size(), 2u);
}

TEST_F(ActResolverTest, KeepsStaleActWhenRefreshFails) {
    transport.push(response(200, readFixture("content.json")));
    ASSERT_TRUE(resolver.currentActId());
    clock.advance(2h);
    transport.push(response(503));
    auto id = resolver.currentActId();
    ASSERT_TRUE(id);
    EXPECT_EQ(id.value(), "4c4b8cff-43eb-13d3-8f14-96b783c90cd2");
}

TEST_F(ActResolverTest, ReportsErrorWhenNothingCached) {
    transport.push(response(403, {{"status", {{"message", "Forbidden"}}}}));
    auto id = resolver.currentActId();
    ASSERT_FALSE(id);
    EXPECT_EQ(id.error().kind, ErrorKind::Forbidden);

    transport.push(response(200, {{"version", "v"}, {"acts", nlohmann::json::array()}}));
    id = resolver.currentActId();
    ASSERT_FALSE(id);
    EXPECT_EQ(id.error().kind, ErrorKind::Parse);
}
