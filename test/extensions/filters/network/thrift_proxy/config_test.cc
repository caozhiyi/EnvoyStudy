#include "envoy/extensions/filters/network/thrift_proxy/v2alpha1/thrift_proxy.pb.validate.h"

#include "extensions/filters/network/thrift_proxy/config.h"

#include "test/mocks/server/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ThriftProxy {

TEST(ThriftFilterConfigTest, ValidateFail) {
  NiceMock<Server::Configuration::MockFactoryContext> context;
  EXPECT_THROW(
      ThriftProxyFilterConfigFactory().createFilterFactoryFromProto(
          envoy::extensions::filters::network::thrift_proxy::v2alpha1::ThriftProxy(), context),
      ProtoValidationException);
}

TEST(ThriftFilterConfigTest, ValidProtoConfiguration) {
  envoy::extensions::filters::network::thrift_proxy::v2alpha1::ThriftProxy config{};

  config.set_stat_prefix("my_stat_prefix");

  NiceMock<Server::Configuration::MockFactoryContext> context;
  ThriftProxyFilterConfigFactory factory;
  Network::FilterFactoryCb cb = factory.createFilterFactoryFromProto(config, context);
  Network::MockConnection connection;
  EXPECT_CALL(connection, addFilter(_));
  cb(connection);
}

TEST(ThriftFilterConfigTest, ThriftProxyWithEmptyProto) {
  NiceMock<Server::Configuration::MockFactoryContext> context;
  ThriftProxyFilterConfigFactory factory;
  envoy::extensions::filters::network::thrift_proxy::v2alpha1::ThriftProxy config =
      *dynamic_cast<envoy::extensions::filters::network::thrift_proxy::v2alpha1::ThriftProxy*>(
          factory.createEmptyConfigProto().get());
  config.set_stat_prefix("my_stat_prefix");

  Network::FilterFactoryCb cb = factory.createFilterFactoryFromProto(config, context);
  Network::MockConnection connection;
  EXPECT_CALL(connection, addFilter(_));
  cb(connection);
}

} // namespace ThriftProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
