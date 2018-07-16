#include "envoy/api/v2/core/base.pb.h"

#include "common/http/headers.h"
#include "common/http/message_impl.h"
#include "common/protobuf/protobuf.h"
#include "common/tracing/http_tracer_impl.h"

#include "extensions/filters/common/ext_authz/ext_authz_http_impl.h"

#include "test/extensions/filters/common/ext_authz/mocks.h"
#include "test/extensions/filters/common/ext_authz/test_common.h"
#include "test/mocks/upstream/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Invoke;
using testing::Ref;
using testing::Return;
using testing::ReturnPointee;
using testing::ReturnRef;
using testing::WhenDynamicCastTo;
using testing::WithArg;
using testing::_;

namespace Envoy {
namespace Extensions {
namespace Filters {
namespace Common {
namespace ExtAuthz {

typedef std::vector<envoy::api::v2::core::HeaderValueOption> HeaderValueOptionVector;

class ExtAuthzHttpClientTest : public testing::Test {
public:
  ExtAuthzHttpClientTest()
      : cluster_name_{"foo"}, cluster_manager_{}, timeout_{}, path_prefix_{"/bar"},
        response_headers_to_remove_{Http::LowerCaseString{"bar"}}, async_client_{},
        async_request_{&async_client_}, client_(cluster_name_, cluster_manager_, timeout_,
                                                path_prefix_, response_headers_to_remove_) {
    ON_CALL(cluster_manager_, httpAsyncClientForCluster(cluster_name_))
        .WillByDefault(ReturnRef(async_client_));
  }

  std::string cluster_name_;
  NiceMock<Upstream::MockClusterManager> cluster_manager_;
  MockRequestCallbacks request_callbacks_;
  absl::optional<std::chrono::milliseconds> timeout_;
  std::string path_prefix_;
  std::vector<Http::LowerCaseString> response_headers_to_remove_;
  NiceMock<Http::MockAsyncClient> async_client_;
  NiceMock<Http::MockAsyncClientRequest> async_request_;
  RawHttpClientImpl client_;
};

// Test the client when an ok response is received.
TEST_F(ExtAuthzHttpClientTest, AuthorizationOk) {
  const auto expected_headers = TestCommon::makeHeaderValueOption({{":status", "200", false}});
  const auto authz_response = TestCommon::makeAuthzResponse(CheckStatus::OK);
  auto check_response = TestCommon::makeMessageResponse(expected_headers);
  envoy::service::auth::v2alpha::CheckRequest request;

  client_.check(request_callbacks_, request, Tracing::NullSpan::instance());
  EXPECT_CALL(request_callbacks_,
              onComplete_(WhenDynamicCastTo<ResponsePtr&>(AuthzOkResponse(authz_response))));

  client_.onSuccess(std::move(check_response));
}

// Test the client when an request contains path to be re-written and ok response is received.
TEST_F(ExtAuthzHttpClientTest, AuthorizationOkWithPathRewrite) {
  const auto expected_headers = TestCommon::makeHeaderValueOption({{":status", "200", false}});
  const auto authz_response = TestCommon::makeAuthzResponse(CheckStatus::OK);
  auto check_response = TestCommon::makeMessageResponse(expected_headers);

  envoy::service::auth::v2alpha::CheckRequest request{};
  auto mutable_headers =
      request.mutable_attributes()->mutable_request()->mutable_http()->mutable_headers();
  (*mutable_headers)[std::string{":path"}] = std::string{"foo"};
  (*mutable_headers)[std::string{"foo"}] = std::string{"bar"};
  client_.check(request_callbacks_, request, Tracing::NullSpan::instance());
  EXPECT_CALL(request_callbacks_,
              onComplete_(WhenDynamicCastTo<ResponsePtr&>(AuthzOkResponse(authz_response))));

  client_.onSuccess(std::move(check_response));
}

// Test that the client removes certain response headers.
TEST_F(ExtAuthzHttpClientTest, AuthorizationOkWithRemovedHeader) {
  const auto expected_headers = TestCommon::makeHeaderValueOption({{"foobar", "foo", false}});
  const std::string empty_body{};
  const auto authz_response =
      TestCommon::makeAuthzResponse(CheckStatus::OK, Http::Code::OK, empty_body, expected_headers);
  const auto check_response_headers =
      TestCommon::makeHeaderValueOption({{":status", "200", false},
                                         {":path", "/bar", false},
                                         {":method", "post", false},
                                         {"content-length", "post", false},
                                         {"bar", "foo", false},
                                         {"foobar", "foo", false}});
  auto message_response = TestCommon::makeMessageResponse(check_response_headers);

  envoy::service::auth::v2alpha::CheckRequest request;
  client_.check(request_callbacks_, request, Tracing::NullSpan::instance());
  EXPECT_CALL(request_callbacks_,
              onComplete_(WhenDynamicCastTo<ResponsePtr&>(AuthzOkResponse(authz_response))));

  client_.onSuccess(std::move(message_response));
}

// Test the client when a denied response is received due to an unknown status code.
TEST_F(ExtAuthzHttpClientTest, AuthorizationDeniedWithInvalidStatusCode) {
  const auto expected_headers = TestCommon::makeHeaderValueOption({{":status", "error", false}});
  const auto authz_response = TestCommon::makeAuthzResponse(
      CheckStatus::Denied, Http::Code::Forbidden, "", expected_headers);
  Http::MessagePtr check_response(new Http::ResponseMessageImpl(
      Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "error"}}}));
  envoy::service::auth::v2alpha::CheckRequest request;
  client_.check(request_callbacks_, request, Tracing::NullSpan::instance());

  EXPECT_CALL(request_callbacks_,
              onComplete_(WhenDynamicCastTo<ResponsePtr&>(AuthzDeniedResponse(authz_response))));

  client_.onSuccess(std::move(check_response));
}

// Test the client when a denied response is received.
TEST_F(ExtAuthzHttpClientTest, AuthorizationDenied) {
  const auto expected_headers = TestCommon::makeHeaderValueOption({{":status", "403", false}});
  const auto authz_response = TestCommon::makeAuthzResponse(
      CheckStatus::Denied, Http::Code::Forbidden, "", expected_headers);
  auto check_response = TestCommon::makeMessageResponse(expected_headers);

  envoy::service::auth::v2alpha::CheckRequest request;
  client_.check(request_callbacks_, request, Tracing::NullSpan::instance());

  EXPECT_CALL(request_callbacks_,
              onComplete_(WhenDynamicCastTo<ResponsePtr&>(AuthzDeniedResponse(authz_response))));

  client_.onSuccess(std::move(check_response));
}

// Test the client when a denied response is received and it contains additional HTTP attributes.
TEST_F(ExtAuthzHttpClientTest, AuthorizationDeniedWithAllAttributes) {
  const auto expected_body = std::string{"test"};
  const auto expected_headers = TestCommon::makeHeaderValueOption({{":status", "401", false}});
  const auto authz_response = TestCommon::makeAuthzResponse(
      CheckStatus::Denied, Http::Code::Unauthorized, expected_body, expected_headers);
  auto check_response = TestCommon::makeMessageResponse(expected_headers, expected_body);

  envoy::service::auth::v2alpha::CheckRequest request;
  client_.check(request_callbacks_, request, Tracing::NullSpan::instance());

  EXPECT_CALL(request_callbacks_,
              onComplete_(WhenDynamicCastTo<ResponsePtr&>(AuthzDeniedResponse(authz_response))));

  client_.onSuccess(std::move(check_response));
}

// Test the client when an unknown error occurs.
TEST_F(ExtAuthzHttpClientTest, AuthorizationRequestError) {
  envoy::service::auth::v2alpha::CheckRequest request;
  client_.check(request_callbacks_, request, Tracing::NullSpan::instance());

  EXPECT_CALL(request_callbacks_,
              onComplete_(WhenDynamicCastTo<ResponsePtr&>(AuthzErrorResponse(CheckStatus::Error))));
  client_.onFailure(Http::AsyncClient::FailureReason::Reset);
}

// Test the client when the request is canceled.
TEST_F(ExtAuthzHttpClientTest, CancelledAuthorizationRequest) {
  envoy::service::auth::v2alpha::CheckRequest request;
  EXPECT_CALL(async_client_, send_(_, _, _)).WillOnce(Return(&async_request_));
  client_.check(request_callbacks_, request, Tracing::NullSpan::instance());

  EXPECT_CALL(async_request_, cancel());
  client_.cancel();
}

} // namespace ExtAuthz
} // namespace Common
} // namespace Filters
} // namespace Extensions
} // namespace Envoy
