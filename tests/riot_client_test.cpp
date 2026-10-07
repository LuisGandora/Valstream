#include <gtest/gtest.h>

#include "test_util.hpp"
#include "valstream/riot_client.hpp"

using namespace valstream;
using namespace valstream::test;
using namespace std::chrono_literals;
using nlohmann::json;

namespace {

class RiotClientTest : public ::testing::Test {
protected:
    FakeClock clock;
    RateLimiter limiter{clock, {{20, 1s}, {100, 120s}}};
    ScriptedTransport transport;
    RiotClient client{transport, limiter, clock, "RGAPI-test-key"};

    json leaderboardBody() { return readFixture("leaderboard/na_a.json"); }
};

}  // namespace

TEST_F(RiotClientTest, BuildsUrlAndSendsTokenHeader) {
    transport.push(response(200, leaderboardBody()));
    ASSERT_TRUE(client.getLeaderboard("na", "4c4b8cff-43eb-13d3-8f14-96b783c90cd2", 200, 400));

    auto requests = transport.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].url,
              "https://na.api.riotgames.com/val/ranked/v1/leaderboards/by-act/"
              "4c4b8cff-43eb-13d3-8f14-96b783c90cd2?size=200&startIndex=400");
    EXPECT_EQ(requests[0].headers.at("X-Riot-Token"), "RGAPI-test-key");
}

TEST_F(RiotClientTest, ContentAndStatusEndpoints) {
    transport.push(response(200, readFixture("content.json")));
    transport.push(response(200, readFixture("status.json")));

    auto content = client.getContent("eu", "en-US");
    ASSERT_TRUE(content) << content.error().message;
    EXPECT_EQ(content->version, "release-11.08");
    EXPECT_EQ(content->acts.size(), 5u);

    auto status = client.getStatus("kr");
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status->name, "VALORANT");
    EXPECT_EQ(status->raw["incidents"].size(), 1u);

    auto requests = transport.requests();
    EXPECT_EQ(requests[0].url, "https://eu.api.riotgames.com/val/content/v1/contents?locale=en-US");
    EXPECT_EQ(requests[1].url, "https://kr.api.riotgames.com/val/status/v1/platform-data");
}

TEST_F(RiotClientTest, ParsesLeaderboardIncludingAnonymizedPlayer) {
    transport.push(response(200, leaderboardBody()));
    auto board = client.getLeaderboard("na", "act", 200, 0);
    ASSERT_TRUE(board) << board.error().message;
    EXPECT_EQ(board->shard, "na");
    EXPECT_EQ(board->totalPlayers, 18452);
    ASSERT_EQ(board->players.size(), 30u);
    EXPECT_EQ(board->players[0].leaderboardRank, 1);
    EXPECT_TRUE(board->players[0].puuid);

    const auto& anon = board->players[11];
    EXPECT_FALSE(anon.puuid);
    EXPECT_EQ(anon.gameName, "");
    EXPECT_EQ(anon.leaderboardRank, 12);
}

TEST_F(RiotClientTest, Honors429RetryAfterThenSucceeds) {
    transport.push(recordedResponse("rate_limited_429.json"));  // Retry-After: 7, type: application
    transport.push(response(200, leaderboardBody()));

    auto board = client.getLeaderboard("na", "act", 200, 0);
    ASSERT_TRUE(board) << board.error().message;
    EXPECT_EQ(transport.requests().size(), 2u);
    EXPECT_GE(clock.totalSlept(), 7s);
    EXPECT_LT(clock.totalSlept(), 8s);
}

TEST_F(RiotClientTest, RetryAfterPenaltyAlsoHoldsBackOtherRequests) {
    transport.push(recordedResponse("rate_limited_429.json"));
    RiotClient oneShot{transport, limiter, clock, "k", RetryPolicy{.maxAttempts = 1}};
    auto r = oneShot.getLeaderboard("na", "act", 200, 0);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::RateLimited);
    EXPECT_EQ(r.error().httpStatus, 429);
    EXPECT_EQ(r.error().message, "Rate limit exceeded");
    // An application-scope 429 blocks the whole region, not just this method.
    EXPECT_TRUE(limiter.tryAcquire("na", "status"));
    EXPECT_FALSE(limiter.tryAcquire("eu", "status"));
}

TEST_F(RiotClientTest, SyncsCountsFromRecorded429) {
    transport.push(recordedResponse("rate_limited_429.json"));  // X-Method-Rate-Limit(-Count): 250:10 / 12:10
    RiotClient oneShot{transport, limiter, clock, "k", RetryPolicy{.maxAttempts = 1}};
    (void)oneShot.getLeaderboard("na", "act", 200, 0);
    EXPECT_EQ(limiter.limits({"na", "leaderboard"}), (std::vector<RateLimit>{{250, 10s}}));
    EXPECT_EQ(limiter.remaining({"na", "leaderboard"}), 250 - 12);
    EXPECT_EQ(limiter.remaining({"na", "app"}), 0);  // Riot counted 21 in the 1s window
}

TEST_F(RiotClientTest, MissingRetryAfterUsesDefault) {
    transport.push(response(429, {}, {{"X-Rate-Limit-Type", "service"}}));
    transport.push(response(200, leaderboardBody()));
    ASSERT_TRUE(client.getLeaderboard("na", "act", 200, 0));
    EXPECT_GE(clock.totalSlept(), 1s);
}

TEST_F(RiotClientTest, Retries5xxWithExponentialBackoff) {
    transport.push(response(503));
    transport.push(response(502));
    transport.push(response(200, leaderboardBody()));

    ASSERT_TRUE(client.getLeaderboard("na", "act", 200, 0));
    EXPECT_EQ(transport.requests().size(), 3u);
    // 500ms * 2^0 + 500ms * 2^1, each plus jitter in [0, 500ms]
    EXPECT_GE(clock.totalSlept(), 1500ms);
    EXPECT_LE(clock.totalSlept(), 2500ms);
}

TEST_F(RiotClientTest, GivesUpAfterMaxAttempts) {
    for (int i = 0; i < 4; ++i) transport.push(response(500, {{"status", {{"message", "Internal error"}}}}));
    auto r = client.getLeaderboard("na", "act", 200, 0);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::ServerError);
    EXPECT_EQ(r.error().httpStatus, 500);
    EXPECT_EQ(r.error().message, "Internal error");
    EXPECT_EQ(transport.requests().size(), 4u);
}

TEST_F(RiotClientTest, RetriesTransportErrors) {
    transport.push(HttpResponse{0, {}, "", "Connection timed out"});
    transport.push(response(200, leaderboardBody()));
    ASSERT_TRUE(client.getLeaderboard("na", "act", 200, 0));
    EXPECT_EQ(transport.requests().size(), 2u);
}

TEST_F(RiotClientTest, MapsClientErrorsWithoutRetrying) {
    const std::pair<int, ErrorKind> cases[] = {{400, ErrorKind::BadRequest},
                                               {401, ErrorKind::Unauthorized},
                                               {403, ErrorKind::Forbidden},
                                               {404, ErrorKind::NotFound},
                                               {415, ErrorKind::Http}};
    for (auto [status, kind] : cases) {
        ScriptedTransport t;
        t.push(response(status, {{"status", {{"message", "Forbidden"}, {"status_code", status}}}}));
        RiotClient c{t, limiter, clock, "k"};
        auto r = c.getStatus("na");
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().kind, kind) << status;
        EXPECT_EQ(r.error().httpStatus, status);
        EXPECT_EQ(r.error().message, "Forbidden");
        EXPECT_EQ(t.requests().size(), 1u) << status;
    }
}

TEST_F(RiotClientTest, RejectsInvalidArgumentsWithoutSending) {
    EXPECT_EQ(client.getStatus("euw").error().kind, ErrorKind::InvalidArgument);
    EXPECT_EQ(client.getLeaderboard("na", "act", 0, 0).error().kind, ErrorKind::InvalidArgument);
    EXPECT_EQ(client.getLeaderboard("na", "act", 201, 0).error().kind, ErrorKind::InvalidArgument);
    EXPECT_EQ(client.getLeaderboard("na", "act", 200, -1).error().kind, ErrorKind::InvalidArgument);
    EXPECT_EQ(client.getLeaderboard("na", "", 200, 0).error().kind, ErrorKind::InvalidArgument);
    EXPECT_TRUE(transport.requests().empty());
}

TEST_F(RiotClientTest, MalformedBodiesAreParseErrors) {
    transport.push(HttpResponse{200, {}, "<html>oops</html>", ""});
    transport.push(response(200, {{"shard", "na"}, {"players", json::array()}}));  // no actId
    transport.push(response(200, {{"actId", "a"}, {"players", {{{"puuid", "p"}}}}}));  // player without rank
    for (int i = 0; i < 3; ++i) {
        auto r = client.getLeaderboard("na", "act", 200, 0);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().kind, ErrorKind::Parse) << i;
    }
}

TEST_F(RiotClientTest, OwnRateLimiterTimeoutIsReported) {
    transport.push(response(200, leaderboardBody(), {{"X-Method-Rate-Limit", "1:60"}}));
    RiotClient impatient{transport, limiter, clock, "k", RetryPolicy{.acquireTimeout = 5s}};
    ASSERT_TRUE(impatient.getLeaderboard("na", "act", 200, 0));
    auto r = impatient.getLeaderboard("na", "act", 200, 0);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::RateLimited);
    EXPECT_EQ(transport.requests().size(), 1u);  // second request never sent
}
