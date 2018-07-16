#include "envoy/http/message.h"

#include "common/http/message_impl.h"

#include "server/config_validation/async_client.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/upstream/mocks.h"

namespace Envoy {
namespace Http {

TEST(ValidationAsyncClientTest, MockedMethods) {
  MessagePtr message{new RequestMessageImpl()};
  MockAsyncClientCallbacks callbacks;
  MockAsyncClientStreamCallbacks stream_callbacks;

  ValidationAsyncClient client;
  EXPECT_EQ(nullptr, client.send(std::move(message), callbacks,
                                 absl::optional<std::chrono::milliseconds>()));
  EXPECT_EQ(nullptr,
            client.start(stream_callbacks, absl::optional<std::chrono::milliseconds>(), false));
}

} // namespace Http
} // namespace Envoy
