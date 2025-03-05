#include "source/extensions/clusters/dns/dns_cluster.h"

#include <chrono>

#include "envoy/common/exception.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/endpoint/v3/endpoint.pb.h"
#include "envoy/config/endpoint/v3/endpoint_components.pb.h"
#include "envoy/extensions/clusters/dns/v3/dns_cluster.pb.h"

#include "source/common/common/dns_utils.h"
#include "source/common/network/dns_resolver/dns_factory_util.h"
#include "source/extensions/clusters/common/dns_cluster_backcompat.h"

namespace Envoy {
namespace Upstream {

absl::StatusOr<std::pair<ClusterImplBaseSharedPtr, ThreadAwareLoadBalancerPtr>>
DnsClusterFactory::createClusterWithConfig(
    const envoy::config::cluster::v3::Cluster& cluster,
    const envoy::extensions::clusters::dns::v3::DnsCluster& proto_config,
    Upstream::ClusterFactoryContext& context) {
  absl::StatusOr<Network::DnsResolverSharedPtr> dns_resolver_or_error =
      selectDnsResolver(cluster, context);
  RETURN_IF_NOT_OK(dns_resolver_or_error.status());

  absl::StatusOr<std::unique_ptr<ClusterImplBase>> cluster_or_error;
  cluster_or_error =
      DnsClusterImpl::create(cluster, proto_config, context, std::move(*dns_resolver_or_error));

  RETURN_IF_NOT_OK(cluster_or_error.status());
  return std::make_pair(ClusterImplBaseSharedPtr(std::move(*cluster_or_error)), nullptr);
}

REGISTER_FACTORY(DnsClusterFactory, ClusterFactory);

/**
 * LogicalDNSFactory: making it back compatible with ClusterFactoryImplBase
 */

class LogicalDNSFactory : public ClusterFactoryImplBase {
public:
  LogicalDNSFactory() : ClusterFactoryImplBase("envoy.cluster.logical_dns") {}
  virtual absl::StatusOr<std::pair<ClusterImplBaseSharedPtr, ThreadAwareLoadBalancerPtr>>
  createClusterImpl(const envoy::config::cluster::v3::Cluster& cluster,
                    ClusterFactoryContext& context) override {
    absl::StatusOr<Network::DnsResolverSharedPtr> dns_resolver_or_error =
        selectDnsResolver(cluster, context);
    RETURN_IF_NOT_OK(dns_resolver_or_error.status());

    envoy::extensions::clusters::dns::v3::DnsCluster typed_config;
    createDnsClusterFromLegacyFields(cluster, typed_config);

    typed_config.set_all_addresses_in_single_endpoint(true);

    absl::StatusOr<std::unique_ptr<ClusterImplBase>> cluster_or_error;
    cluster_or_error =
        DnsClusterImpl::create(cluster, typed_config, context, std::move(*dns_resolver_or_error));

    RETURN_IF_NOT_OK(cluster_or_error.status());
    return std::make_pair(ClusterImplBaseSharedPtr(std::move(*cluster_or_error)), nullptr);
  }
};

REGISTER_FACTORY(LogicalDNSFactory, ClusterFactory);

/**
 * StrictDNSFactory: making it back compatible with ClusterFactoryImplBase
 */

class StrictDNSFactory : public Upstream::ConfigurableClusterFactoryBase<
                             envoy::extensions::clusters::dns::v3::DnsCluster> {
public:
  StrictDNSFactory() : ConfigurableClusterFactoryBase("envoy.cluster.strict_dns") {}
  absl::StatusOr<std::pair<ClusterImplBaseSharedPtr, ThreadAwareLoadBalancerPtr>>
  createClusterWithConfig(const envoy::config::cluster::v3::Cluster& cluster,
                          const envoy::extensions::clusters::dns::v3::DnsCluster& proto_config,
                          Upstream::ClusterFactoryContext& context) override {
    absl::StatusOr<Network::DnsResolverSharedPtr> dns_resolver_or_error =
        selectDnsResolver(cluster, context);
    RETURN_IF_NOT_OK(dns_resolver_or_error.status());

    absl::StatusOr<std::unique_ptr<ClusterImplBase>> cluster_or_error;
    cluster_or_error =
        DnsClusterImpl::create(cluster, proto_config, context, std::move(*dns_resolver_or_error));

    RETURN_IF_NOT_OK(cluster_or_error.status());
    return std::make_pair(ClusterImplBaseSharedPtr(std::move(*cluster_or_error)), nullptr);
  }
};

REGISTER_FACTORY(StrictDNSFactory, ClusterFactory);

/**
 * DnsClusterImpl: implementation for both logical and strict DNS.
 */

absl::StatusOr<std::unique_ptr<DnsClusterImpl>>
DnsClusterImpl::create(const envoy::config::cluster::v3::Cluster& cluster,
                       const envoy::extensions::clusters::dns::v3::DnsCluster& dns_cluster,
                       ClusterFactoryContext& context, Network::DnsResolverSharedPtr dns_resolver) {
  absl::Status creation_status = absl::OkStatus();
  auto ret = std::unique_ptr<DnsClusterImpl>(
      new DnsClusterImpl(cluster, dns_cluster, context, std::move(dns_resolver), creation_status));

  RETURN_IF_NOT_OK(creation_status);
  return ret;
}

DnsClusterImpl::DnsClusterImpl(const envoy::config::cluster::v3::Cluster& cluster,
                               const envoy::extensions::clusters::dns::v3::DnsCluster& dns_cluster,
                               ClusterFactoryContext& context,
                               Network::DnsResolverSharedPtr dns_resolver,
                               absl::Status& creation_status)
    : BaseDynamicClusterImpl(cluster, context, creation_status),
      load_assignment_(cluster.load_assignment()),
      local_info_(context.serverFactoryContext().localInfo()), dns_resolver_(dns_resolver),
      dns_refresh_rate_ms_(std::chrono::milliseconds(
          PROTOBUF_GET_MS_OR_DEFAULT(dns_cluster, dns_refresh_rate, 5000))),
      dns_jitter_ms_(PROTOBUF_GET_MS_OR_DEFAULT(dns_cluster, dns_jitter, 0)),
      respect_dns_ttl_(dns_cluster.respect_dns_ttl()),
      dns_lookup_family_(
          Envoy::DnsUtils::getDnsLookupFamilyFromEnum(dns_cluster.dns_lookup_family())),
      all_addresses_in_single_endpoint_(dns_cluster.all_addresses_in_single_endpoint()) {
  failure_backoff_strategy_ = Config::Utility::prepareDnsRefreshStrategy(
      dns_cluster, dns_refresh_rate_ms_.count(),
      context.serverFactoryContext().api().randomGenerator());

  std::list<ResolveTargetPtr> resolve_targets;
  const auto& locality_lb_endpoints = load_assignment_.endpoints();

  if (all_addresses_in_single_endpoint_) {
    if (locality_lb_endpoints.size() != 1 || locality_lb_endpoints[0].lb_endpoints().size() != 1) {
      creation_status = absl::InvalidArgumentError(
          "LOGICAL_DNS clusters must have a single locality_lb_endpoint and a single lb_endpoint");
      return;
    }
  }

  for (const auto& locality_lb_endpoint : locality_lb_endpoints) {
    const auto validation = validateEndpointsForZoneAwareRouting(locality_lb_endpoint);
    if (!all_addresses_in_single_endpoint_ && !validation.ok()) {
      creation_status = validation;
      return;
    }

    for (const auto& lb_endpoint : locality_lb_endpoint.lb_endpoints()) {
      const auto& socket_address = lb_endpoint.endpoint().address().socket_address();
      if (!socket_address.resolver_name().empty()) {
        creation_status =
            absl::InvalidArgumentError("DNS clusters must NOT have a custom resolver name set");
        return;
      }

      resolve_targets.emplace_back(new ResolveTarget(
          *this, context.serverFactoryContext().mainThreadDispatcher(), socket_address.address(),
          socket_address.port_value(), locality_lb_endpoint, lb_endpoint));
    }
  }
  resolve_targets_ = std::move(resolve_targets);

  overprovisioning_factor_ = PROTOBUF_GET_WRAPPED_OR_DEFAULT(
      load_assignment_.policy(), overprovisioning_factor, kDefaultOverProvisioningFactor);
  weighted_priority_health_ = load_assignment_.policy().weighted_priority_health();
}

void DnsClusterImpl::startPreInit() {
  for (const ResolveTargetPtr& target : resolve_targets_) {
    target->startResolve();
  }
  // If the config provides no endpoints, the cluster is initialized immediately as if all hosts are
  // resolved in failure.
  if (resolve_targets_.empty() || !wait_for_warm_on_init_) {
    onPreInitComplete();
  }
}

void DnsClusterImpl::updateAllHosts(const HostVector& hosts_added, const HostVector& hosts_removed,
                                    uint32_t current_priority) {
  PriorityStateManager priority_state_manager(*this, local_info_, nullptr, random_);
  // At this point we know that we are different so make a new host list and notify.
  //
  // TODO(dio): The uniqueness of a host address resolved in STRICT_DNS cluster per priority is not
  // guaranteed. Need a clear agreement on the behavior here, whether it is allowable to have
  // duplicated hosts inside a priority. And if we want to enforce this behavior, it should be done
  // inside the priority state manager.
  for (const ResolveTargetPtr& target : resolve_targets_) {
    priority_state_manager.initializePriorityFor(target->locality_lb_endpoints_);
    for (const HostSharedPtr& host : target->hosts_) {
      if (target->locality_lb_endpoints_.priority() == current_priority) {
        priority_state_manager.registerHostForPriority(host, target->locality_lb_endpoints_);
      }
    }
  }

  // TODO(dio): Add assertion in here.
  priority_state_manager.updateClusterPrioritySet(
      current_priority, std::move(priority_state_manager.priorityState()[current_priority].first),
      hosts_added, hosts_removed, absl::nullopt, weighted_priority_health_,
      overprovisioning_factor_);
}

DnsClusterImpl::ResolveTarget::ResolveTarget(
    DnsClusterImpl& parent, Event::Dispatcher& dispatcher, const std::string& dns_address,
    const uint32_t dns_port,
    const envoy::config::endpoint::v3::LocalityLbEndpoints& locality_lb_endpoint,
    const envoy::config::endpoint::v3::LbEndpoint& lb_endpoint)
    : parent_(parent), locality_lb_endpoints_(locality_lb_endpoint), lb_endpoint_(lb_endpoint),
      dns_address_(dns_address),
      hostname_(lb_endpoint_.endpoint().hostname().empty() ? dns_address_
                                                           : lb_endpoint_.endpoint().hostname()),
      port_(dns_port),
      resolve_timer_(dispatcher.createTimer([this]() -> void { startResolve(); })) {}

DnsClusterImpl::ResolveTarget::~ResolveTarget() {
  if (active_query_) {
    active_query_->cancel(Network::ActiveDnsQuery::CancelReason::QueryAbandoned);
  }
}

bool DnsClusterImpl::ResolveTarget::isSuccessfulResponse(
    const std::list<Network::DnsResponse>& response,
    const Network::DnsResolver::ResolutionStatus& status) {
  return status == Network::DnsResolver::ResolutionStatus::Completed &&
         (!parent_.all_addresses_in_single_endpoint_ || /* strict DNS accepts empty responses */
          (parent_.all_addresses_in_single_endpoint_ &&
           !response.empty())); /* logical DNS doesn't */
}

void DnsClusterImpl::ResolveTarget::startResolve() {
  ENVOY_LOG(trace, "starting async DNS resolution for {}", dns_address_);
  parent_.info_->configUpdateStats().update_attempt_.inc();

  active_query_ = parent_.dns_resolver_->resolve(
      dns_address_, parent_.dns_lookup_family_,
      [this](Network::DnsResolver::ResolutionStatus status, absl::string_view details,
             std::list<Network::DnsResponse>&& response) -> void {
        active_query_ = nullptr;
        ENVOY_LOG(trace, "async DNS resolution complete for {} details {}", dns_address_, details);

        std::chrono::milliseconds final_refresh_rate = parent_.dns_refresh_rate_ms_;

        if (isSuccessfulResponse(response, status)) {
          parent_.info_->configUpdateStats().update_success_.inc();

          HostVector new_hosts;
          std::chrono::seconds ttl_refresh_rate = std::chrono::seconds::max();
          absl::flat_hash_set<std::string> all_new_hosts;

          for (const auto& resp : response) {
            const auto& addrinfo = resp.addrInfo();
            // TODO(mattklein123): Currently the DNS interface does not consider port. We need to
            // make a new address that has port in it. We need to both support IPv6 as well as
            // potentially move port handling into the DNS interface itself, which would work better
            // for SRV.
            ASSERT(addrinfo.address_ != nullptr);
            auto address = Network::Utility::getAddressWithPort(*(addrinfo.address_), port_);
            if (all_new_hosts.count(address->asString()) > 0) {
              continue;
            }

            auto host_or_error = HostImpl::create(
                parent_.info_, hostname_, address,
                // TODO(zyfjeff): Created through metadata shared pool
                std::make_shared<const envoy::config::core::v3::Metadata>(lb_endpoint_.metadata()),
                std::make_shared<const envoy::config::core::v3::Metadata>(
                    locality_lb_endpoints_.metadata()),
                lb_endpoint_.load_balancing_weight().value(), locality_lb_endpoints_.locality(),
                lb_endpoint_.endpoint().health_check_config(), locality_lb_endpoints_.priority(),
                lb_endpoint_.health_status(), parent_.time_source_);
            if (!host_or_error.ok()) {
              ENVOY_LOG(error, "Failed to create host {} with error: {}", address->asString(),
                        host_or_error.status().message());
              parent_.info_->configUpdateStats().update_failure_.inc();
              return;
            }
            new_hosts.emplace_back(std::move(host_or_error.value()));
            all_new_hosts.emplace(address->asString());
            ttl_refresh_rate = min(ttl_refresh_rate, addrinfo.ttl_);

            // We only need a single address for logical DNS.
            if (parent_.all_addresses_in_single_endpoint_) {
              break;
            }
          }

          HostVector hosts_added;
          HostVector hosts_removed;
          if (parent_.updateDynamicHostList(new_hosts, hosts_, hosts_added, hosts_removed,
                                            all_hosts_, all_new_hosts)) {
            ENVOY_LOG(debug, "DNS hosts have changed for {}", dns_address_);
            ASSERT(std::all_of(hosts_.begin(), hosts_.end(), [&](const auto& host) {
              return host->priority() == locality_lb_endpoints_.priority();
            }));

            // Update host map for current resolve target.
            for (const auto& host : hosts_removed) {
              all_hosts_.erase(host->address()->asString());
            }
            for (const auto& host : hosts_added) {
              all_hosts_.insert({host->address()->asString(), host});
            }

            parent_.updateAllHosts(hosts_added, hosts_removed, locality_lb_endpoints_.priority());
          } else {
            parent_.info_->configUpdateStats().update_no_rebuild_.inc();
          }

          // reset failure backoff strategy because there was a success.
          parent_.failure_backoff_strategy_->reset();

          if (!response.empty() && parent_.respect_dns_ttl_ &&
              ttl_refresh_rate != std::chrono::seconds(0)) {
            final_refresh_rate = ttl_refresh_rate;
            ASSERT(ttl_refresh_rate != std::chrono::seconds::max() &&
                   final_refresh_rate.count() > 0);
          }
          if (parent_.dns_jitter_ms_.count() > 0) {
            // Note that `parent_.random_.random()` returns a uint64 while
            // `parent_.dns_jitter_ms_.count()` returns a signed long that gets cast into a uint64.
            // Thus, the modulo of the two will be a positive as long as
            // `parent_dns_jitter_ms_.count()` is positive.
            // It is important that this be positive, otherwise `final_refresh_rate` could be
            // negative causing Envoy to crash.
            final_refresh_rate += std::chrono::milliseconds(parent_.random_.random() %
                                                            parent_.dns_jitter_ms_.count());
          }

          ENVOY_LOG(debug, "DNS refresh rate reset for {}, refresh rate {} ms", dns_address_,
                    final_refresh_rate.count());
        } else {
          parent_.info_->configUpdateStats().update_failure_.inc();

          final_refresh_rate =
              std::chrono::milliseconds(parent_.failure_backoff_strategy_->nextBackOffMs());
          ENVOY_LOG(debug, "DNS refresh rate reset for {}, (failure) refresh rate {} ms",
                    dns_address_, final_refresh_rate.count());
        }

        // If there is an initialize callback, fire it now. Note that if the cluster refers to
        // multiple DNS names, this will return initialized after a single DNS resolution
        // completes. This is not perfect but is easier to code and unclear if the extra
        // complexity is needed so will start with this.
        parent_.onPreInitComplete();
        resolve_timer_->enableTimer(final_refresh_rate);
      });
}

} // namespace Upstream
} // namespace Envoy
