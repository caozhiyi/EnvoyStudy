#include <chrono>

#include "common/common/token_bucket_impl.h"

#include "test/mocks/common.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::Mock;
using ::testing::Return;
using testing::NiceMock;

namespace Envoy {

class TokenBucketImplTest : public testing::Test {
public:
  using time_point = std::chrono::steady_clock::time_point;

protected:
  NiceMock<MockMonotonicTimeSource> time_source_;
};

// Verifies TokenBucket initialization.
TEST_F(TokenBucketImplTest, Initialization) {
  TokenBucketImpl token_bucket{1, -1.0, time_source_};

  EXPECT_TRUE(token_bucket.consume());
  EXPECT_CALL(time_source_, currentTime()).WillOnce(Return(time_point{}));
  EXPECT_FALSE(token_bucket.consume());
}

// Verifies TokenBucket's maximum capacity.
TEST_F(TokenBucketImplTest, MaxBucketSize) {
  TokenBucketImpl token_bucket{3, 1, time_source_};

  EXPECT_TRUE(token_bucket.consume(3));
  EXPECT_CALL(time_source_, currentTime()).WillOnce(Return(time_point(std::chrono::seconds(10))));

  EXPECT_FALSE(token_bucket.consume(4));
  EXPECT_TRUE(token_bucket.consume(3));
}

// Verifies that TokenBucket can consume and refill tokens.
TEST_F(TokenBucketImplTest, ConsumeAndRefill) {
  {
    TokenBucketImpl token_bucket{10, 1, time_source_};

    EXPECT_FALSE(token_bucket.consume(20));
    EXPECT_TRUE(token_bucket.consume(9));

    EXPECT_CALL(time_source_, currentTime()).WillOnce(Return(time_point{}));
    EXPECT_TRUE(token_bucket.consume());

    EXPECT_CALL(time_source_, currentTime())
        .WillOnce(Return(time_point(std::chrono::milliseconds(999))));
    EXPECT_FALSE(token_bucket.consume());

    EXPECT_CALL(time_source_, currentTime())
        .WillOnce(Return(time_point(std::chrono::milliseconds(5999))));
    EXPECT_FALSE(token_bucket.consume(6));

    EXPECT_CALL(time_source_, currentTime())
        .WillRepeatedly(Return(time_point(std::chrono::milliseconds(6000))));
    EXPECT_TRUE(token_bucket.consume(6));
    EXPECT_FALSE(token_bucket.consume());
  }

  ASSERT_TRUE(Mock::VerifyAndClear(&time_source_));

  {
    TokenBucketImpl token_bucket{1, 0.5, time_source_};

    EXPECT_TRUE(token_bucket.consume());

    EXPECT_CALL(time_source_, currentTime())
        .Times(3)
        .WillOnce(Return(time_point(std::chrono::milliseconds(500))))
        .WillOnce(Return(time_point(std::chrono::milliseconds(1500))))
        .WillOnce(Return(time_point(std::chrono::milliseconds(2000))));

    EXPECT_FALSE(token_bucket.consume());
    EXPECT_FALSE(token_bucket.consume());
    EXPECT_TRUE(token_bucket.consume());

    ASSERT_TRUE(Mock::VerifyAndClear(&time_source_));
  }
}

} // namespace Envoy
