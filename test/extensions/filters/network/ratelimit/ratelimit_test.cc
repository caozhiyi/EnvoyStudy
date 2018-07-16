#include <memory>
#include <string>
#include <vector>

#include "common/buffer/buffer_impl.h"
#include "common/config/filter_json.h"
#include "common/json/json_loader.h"
#include "common/stats/stats_impl.h"

#include "extensions/filters/network/ratelimit/ratelimit.h"

#include "test/mocks/network/mocks.h"
#include "test/mocks/ratelimit/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/tracing/mocks.h"
#include "test/test_common/printers.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::WithArgs;
using testing::_;

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace RateLimitFilter {

class RateLimitFilterTest : public testing::Test {
public:
  RateLimitFilterTest() {
    std::string json = R"EOF(
    {
      "domain": "foo",
      "descriptors": [
         [{"key": "hello", "value": "world"}, {"key": "foo", "value": "bar"}],
         [{"key": "foo2", "value": "bar2"}]
       ],
       "stat_prefix": "name"
    }
    )EOF";

    ON_CALL(runtime_.snapshot_, featureEnabled("ratelimit.tcp_filter_enabled", 100))
        .WillByDefault(Return(true));
    ON_CALL(runtime_.snapshot_, featureEnabled("ratelimit.tcp_filter_enforcing", 100))
        .WillByDefault(Return(true));

    Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json);
    envoy::config::filter::network::rate_limit::v2::RateLimit proto_config{};
    Envoy::Config::FilterJson::translateTcpRateLimitFilter(*json_config, proto_config);
    config_.reset(new Config(proto_config, stats_store_, runtime_));
    client_ = new RateLimit::MockClient();
    filter_.reset(new Filter(config_, RateLimit::ClientPtr{client_}));
    filter_->initializeReadFilterCallbacks(filter_callbacks_);

    // NOP currently.
    filter_->onAboveWriteBufferHighWatermark();
    filter_->onBelowWriteBufferLowWatermark();
  }

  ~RateLimitFilterTest() {
    for (const Stats::GaugeSharedPtr& gauge : stats_store_.gauges()) {
      EXPECT_EQ(0U, gauge->value());
    }
  }

  Stats::IsolatedStoreImpl stats_store_;
  NiceMock<Runtime::MockLoader> runtime_;
  ConfigSharedPtr config_;
  RateLimit::MockClient* client_;
  std::unique_ptr<Filter> filter_;
  NiceMock<Network::MockReadFilterCallbacks> filter_callbacks_;
  RateLimit::RequestCallbacks* request_callbacks_{};
};

TEST_F(RateLimitFilterTest, BadRatelimitConfig) {
  std::string json_string = R"EOF(
  {
    "stat_prefix": "my_stat_prefix",
    "domain" : "fake_domain",
    "descriptors": [[{ "key" : "my_key",  "value" : "my_value" }]],
    "ip_white_list": "12"
  }
  )EOF";

  Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json_string);
  envoy::config::filter::network::rate_limit::v2::RateLimit proto_config{};

  EXPECT_THROW(Envoy::Config::FilterJson::translateTcpRateLimitFilter(*json_config, proto_config),
               Json::Exception);
}

TEST_F(RateLimitFilterTest, OK) {
  InSequence s;

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"hello", "world"}, {"foo", "bar"}}}, {{{"foo2", "bar2"}}}}),
                              testing::A<Tracing::Span&>()))
      .WillOnce(WithArgs<0>(Invoke([&](RateLimit::RequestCallbacks& callbacks) -> void {
        request_callbacks_ = &callbacks;
      })));

  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onNewConnection());
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onData(data, false));
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onData(data, false));

  EXPECT_CALL(filter_callbacks_, continueReading());
  request_callbacks_->complete(RateLimit::LimitStatus::OK);

  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onData(data, false));

  EXPECT_CALL(*client_, cancel()).Times(0);
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::LocalClose);

  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.total").value());
  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.ok").value());
}

TEST_F(RateLimitFilterTest, OverLimit) {
  InSequence s;

  EXPECT_CALL(*client_, limit(_, "foo", _, _))
      .WillOnce(WithArgs<0>(Invoke([&](RateLimit::RequestCallbacks& callbacks) -> void {
        request_callbacks_ = &callbacks;
      })));

  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onNewConnection());
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onData(data, false));

  EXPECT_CALL(filter_callbacks_.connection_, close(Network::ConnectionCloseType::NoFlush));
  EXPECT_CALL(*client_, cancel()).Times(0);
  request_callbacks_->complete(RateLimit::LimitStatus::OverLimit);

  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onData(data, false));

  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.total").value());
  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.over_limit").value());
  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.cx_closed").value());
}

TEST_F(RateLimitFilterTest, OverLimitNotEnforcing) {
  InSequence s;

  EXPECT_CALL(*client_, limit(_, "foo", _, _))
      .WillOnce(WithArgs<0>(Invoke([&](RateLimit::RequestCallbacks& callbacks) -> void {
        request_callbacks_ = &callbacks;
      })));

  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onNewConnection());
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onData(data, false));

  EXPECT_CALL(runtime_.snapshot_, featureEnabled("ratelimit.tcp_filter_enforcing", 100))
      .WillOnce(Return(false));
  EXPECT_CALL(filter_callbacks_.connection_, close(_)).Times(0);
  EXPECT_CALL(*client_, cancel()).Times(0);
  EXPECT_CALL(filter_callbacks_, continueReading());
  request_callbacks_->complete(RateLimit::LimitStatus::OverLimit);

  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onData(data, false));

  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.total").value());
  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.over_limit").value());
  EXPECT_EQ(0U, stats_store_.counter("ratelimit.name.cx_closed").value());
}

TEST_F(RateLimitFilterTest, Error) {
  InSequence s;

  EXPECT_CALL(*client_, limit(_, "foo", _, _))
      .WillOnce(WithArgs<0>(Invoke([&](RateLimit::RequestCallbacks& callbacks) -> void {
        request_callbacks_ = &callbacks;
      })));

  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onNewConnection());
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onData(data, false));

  EXPECT_CALL(filter_callbacks_, continueReading());
  request_callbacks_->complete(RateLimit::LimitStatus::Error);

  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onData(data, false));

  EXPECT_CALL(*client_, cancel()).Times(0);
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.total").value());
  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.error").value());
}

TEST_F(RateLimitFilterTest, Disconnect) {
  InSequence s;

  EXPECT_CALL(*client_, limit(_, "foo", _, _))
      .WillOnce(WithArgs<0>(Invoke([&](RateLimit::RequestCallbacks& callbacks) -> void {
        request_callbacks_ = &callbacks;
      })));

  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onNewConnection());
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Network::FilterStatus::StopIteration, filter_->onData(data, false));

  EXPECT_CALL(*client_, cancel());
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.total").value());
}

TEST_F(RateLimitFilterTest, ImmediateOK) {
  InSequence s;

  EXPECT_CALL(filter_callbacks_, continueReading()).Times(0);
  EXPECT_CALL(*client_, limit(_, "foo", _, _))
      .WillOnce(WithArgs<0>(Invoke([&](RateLimit::RequestCallbacks& callbacks) -> void {
        callbacks.complete(RateLimit::LimitStatus::OK);
      })));

  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onNewConnection());
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onData(data, false));
  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onData(data, false));

  EXPECT_CALL(*client_, cancel()).Times(0);
  filter_callbacks_.connection_.raiseEvent(Network::ConnectionEvent::RemoteClose);

  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.total").value());
  EXPECT_EQ(1U, stats_store_.counter("ratelimit.name.ok").value());
}

TEST_F(RateLimitFilterTest, RuntimeDisable) {
  InSequence s;

  EXPECT_CALL(runtime_.snapshot_, featureEnabled("ratelimit.tcp_filter_enabled", 100))
      .WillOnce(Return(false));
  EXPECT_CALL(*client_, limit(_, _, _, _)).Times(0);

  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onNewConnection());
  Buffer::OwnedImpl data("hello");
  EXPECT_EQ(Network::FilterStatus::Continue, filter_->onData(data, false));
}

} // namespace RateLimitFilter
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
