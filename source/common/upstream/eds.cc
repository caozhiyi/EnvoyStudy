#include "common/upstream/eds.h"

#include "envoy/api/v2/eds.pb.validate.h"
#include "envoy/common/exception.h"

#include "common/common/fmt.h"
#include "common/config/metadata.h"
#include "common/config/subscription_factory.h"
#include "common/config/utility.h"
#include "common/config/well_known_names.h"
#include "common/network/address_impl.h"
#include "common/network/resolver_impl.h"
#include "common/network/utility.h"
#include "common/protobuf/utility.h"
#include "common/upstream/sds_subscription.h"

namespace Envoy {
namespace Upstream {

EdsClusterImpl::EdsClusterImpl(const envoy::api::v2::Cluster& cluster, Runtime::Loader& runtime,
                               Stats::Store& stats, Ssl::ContextManager& ssl_context_manager,
                               const LocalInfo::LocalInfo& local_info, ClusterManager& cm,
                               Event::Dispatcher& dispatcher, Runtime::RandomGenerator& random,
                               bool added_via_api)
    : BaseDynamicClusterImpl(cluster, cm.bindConfig(), runtime, stats, ssl_context_manager,
                             cm.clusterManagerFactory().secretManager(), added_via_api),
      cm_(cm), local_info_(local_info),
      cluster_name_(cluster.eds_cluster_config().service_name().empty()
                        ? cluster.name()
                        : cluster.eds_cluster_config().service_name()) {
  Config::Utility::checkLocalInfo("eds", local_info);

  const auto& eds_config = cluster.eds_cluster_config().eds_config();
  subscription_ = Config::SubscriptionFactory::subscriptionFromConfigSource<
      envoy::api::v2::ClusterLoadAssignment>(
      eds_config, local_info.node(), dispatcher, cm, random, info_->statsScope(),
      [this, &eds_config, &cm, &dispatcher,
       &random]() -> Config::Subscription<envoy::api::v2::ClusterLoadAssignment>* {
        return new SdsSubscription(info_->stats(), eds_config, cm, dispatcher, random);
      },
      "envoy.api.v2.EndpointDiscoveryService.FetchEndpoints",
      "envoy.api.v2.EndpointDiscoveryService.StreamEndpoints");
}

void EdsClusterImpl::startPreInit() { subscription_->start({cluster_name_}, *this); }

void EdsClusterImpl::onConfigUpdate(const ResourceVector& resources, const std::string&) {
  if (resources.empty()) {
    ENVOY_LOG(debug, "Missing ClusterLoadAssignment for {} in onConfigUpdate()", cluster_name_);
    info_->stats().update_empty_.inc();
    onPreInitComplete();
    return;
  }
  if (resources.size() != 1) {
    throw EnvoyException(fmt::format("Unexpected EDS resource length: {}", resources.size()));
  }
  const auto& cluster_load_assignment = resources[0];
  MessageUtil::validate(cluster_load_assignment);
  // TODO(PiotrSikora): Remove this hack once fixed internally.
  if (!(cluster_load_assignment.cluster_name() == cluster_name_)) {
    throw EnvoyException(fmt::format("Unexpected EDS cluster (expecting {}): {}", cluster_name_,
                                     cluster_load_assignment.cluster_name()));
  }
  PriorityStateManager priority_state_manager(*this, local_info_);
  for (const auto& locality_lb_endpoint : cluster_load_assignment.endpoints()) {
    const uint32_t priority = locality_lb_endpoint.priority();
    if (priority > 0 && !cluster_name_.empty() && cluster_name_ == cm_.localClusterName()) {
      throw EnvoyException(
          fmt::format("Unexpected non-zero priority for local cluster '{}'.", cluster_name_));
    }
    priority_state_manager.initializePriorityFor(locality_lb_endpoint);

    for (const auto& lb_endpoint : locality_lb_endpoint.lb_endpoints()) {
      priority_state_manager.registerHostForPriority(
          "", resolveProtoAddress(lb_endpoint.endpoint().address()), locality_lb_endpoint,
          lb_endpoint, Host::HealthFlag::FAILED_EDS_HEALTH);
    }
  }

  // Track whether we rebuilt any LB structures.
  bool cluster_rebuilt = false;

  // Loop over existing priorities not present in the config. This will empty out any priorities
  // the config update did not refer to
  auto& priority_state = priority_state_manager.priorityState();
  for (size_t i = 0; i < priority_state.size(); ++i) {
    if (priority_state[i].first != nullptr) {
      if (locality_weights_map_.size() <= i) {
        locality_weights_map_.resize(i + 1);
      }
      cluster_rebuilt |=
          updateHostsPerLocality(i, *priority_state[i].first, locality_weights_map_[i],
                                 priority_state[i].second, priority_state_manager);
    }
  }

  // Loop over all priorities not present in the config that already exists. This will
  // empty out any remaining priority that the config update did not refer to.
  for (size_t i = priority_state.size(); i < priority_set_.hostSetsPerPriority().size(); ++i) {
    const HostVector empty_hosts;
    LocalityWeightsMap empty_locality_map;

    if (locality_weights_map_.size() <= i) {
      locality_weights_map_.resize(i + 1);
    }
    cluster_rebuilt |= updateHostsPerLocality(i, empty_hosts, locality_weights_map_[i],
                                              empty_locality_map, priority_state_manager);
  }

  if (!cluster_rebuilt) {
    info_->stats().update_no_rebuild_.inc();
  }

  // If we didn't setup to initialize when our first round of health checking is complete, just
  // do it now.
  onPreInitComplete();
}

bool EdsClusterImpl::updateHostsPerLocality(const uint32_t priority, const HostVector& new_hosts,
                                            LocalityWeightsMap& locality_weights_map,
                                            LocalityWeightsMap& new_locality_weights_map,
                                            PriorityStateManager& priority_state_manager) {
  const auto& host_set = priority_set_.getOrCreateHostSet(priority);
  HostVectorSharedPtr current_hosts_copy(new HostVector(host_set.hosts()));

  HostVector hosts_added;
  HostVector hosts_removed;
  // We need to trigger updateHosts with the new host vectors if they have changed. We also do this
  // when the locality weight map changes.
  // TODO(htuch): We eagerly update all the host sets here on weight changes, which isn't great,
  // since this has the knock on effect that we rebuild the load balancers and locality scheduler.
  // We could make this happen lazily, as we do for host-level weight updates, where as things age
  // out of the locality scheduler, we discover their new weights. We don't currently have a shared
  // object for locality weights that we can update here, we should add something like this to
  // improve performance and scalability of locality weight updates.
  if (updateDynamicHostList(new_hosts, *current_hosts_copy, hosts_added, hosts_removed) ||
      locality_weights_map != new_locality_weights_map) {
    locality_weights_map = new_locality_weights_map;
    ENVOY_LOG(debug, "EDS hosts or locality weights changed for cluster: {} ({}) priority {}",
              info_->name(), host_set.hosts().size(), host_set.priority());

    priority_state_manager.updateClusterPrioritySet(priority, std::move(current_hosts_copy),
                                                    hosts_added, hosts_removed);
    return true;
  }
  return false;
}

void EdsClusterImpl::onConfigUpdateFailed(const EnvoyException* e) {
  UNREFERENCED_PARAMETER(e);
  // We need to allow server startup to continue, even if we have a bad config.
  onPreInitComplete();
}

} // namespace Upstream
} // namespace Envoy
