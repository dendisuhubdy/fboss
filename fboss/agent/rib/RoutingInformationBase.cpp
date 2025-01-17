/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/rib/RoutingInformationBase.h"
#include "fboss/agent/rib/ConfigApplier.h"
#include "fboss/agent/rib/ForwardingInformationBaseUpdater.h"
#include "fboss/agent/rib/RouteNextHopEntry.h"
#include "fboss/agent/rib/RouteUpdater.h"

#include "fboss/agent/state/ForwardingInformationBaseMap.h"

#include <memory>
#include <utility>

#include "fboss/agent/AddressUtil.h"
#include "fboss/agent/state/ForwardingInformationBase.h"

namespace {
class Timer {
 public:
  explicit Timer(std::chrono::microseconds* duration)
      : duration_(duration), start_(std::chrono::steady_clock::now()) {}

  ~Timer() {
    auto end = std::chrono::steady_clock::now();
    *duration_ =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
  }

 private:
  std::chrono::microseconds* duration_;
  std::chrono::time_point<std::chrono::steady_clock> start_;
};
} // namespace

namespace facebook {
namespace fboss {
namespace rib {

void RoutingInformationBase::reconfigure(
    const std::shared_ptr<SwitchState>& nextState,
    const RouterIDAndNetworkToInterfaceRoutes& configRouterIDToInterfaceRoutes,
    const std::vector<cfg::StaticRouteWithNextHops>& staticRoutesWithNextHops,
    const std::vector<cfg::StaticRouteNoNextHops>& staticRoutesToNull,
    const std::vector<cfg::StaticRouteNoNextHops>& staticRoutesToCpu) {
  auto lockedRouteTables = synchronizedRouteTables_.wlock();

  // Config application is accomplished in the following sequence of steps:
  // 1. Update the VRFs held in RoutingInformationBase's SynchronizedRouteTables
  // data-structure
  //
  // For each VRF specified in config:
  //
  // 2. Update all of RIB's static routes to be only those specified in
  // config
  //
  // 3. Update all of RIB's interface routes to be only those specified in
  // config
  //
  // 4. Re-resolve routes
  //
  // 5. Update FIB
  //
  // Steps 2-5 take place in ConfigApplier.

  *lockedRouteTables =
      constructRouteTables(lockedRouteTables, configRouterIDToInterfaceRoutes);

  // Because of this sequential loop over each VRF, config application scales
  // linearly with the number of VRFs. If FBOSS is run in a multi-VRF routing
  // architecture in the future, this slow-down can be avoided by parallelizing
  // this loop. Converting this loop to use task-level parallelism should be
  // straightfoward because it has been written to avoid dependencies across
  // different iterations of the loop.
  for (auto& vrfAndRouteTable : *lockedRouteTables) {
    auto vrf = vrfAndRouteTable.first;
    const auto& interfaceRoutes = configRouterIDToInterfaceRoutes.at(vrf);

    // A ConfigApplier object should be independent of the VRF whose routes it
    // is processing. However, because interface and static routes for _all_
    // VRFs are passed to ConfigApplier, the vrf argument is needed to identify
    // the subset of those routes which should be processed.

    // ConfigApplier can be made independent of the VRF whose routes it is
    // processing by the use of boost::filter_iterator.
    ConfigApplier configApplier(
        vrf,
        &(vrfAndRouteTable.second.v4NetworkToRoute),
        &(vrfAndRouteTable.second.v6NetworkToRoute),
        folly::range(interfaceRoutes.cbegin(), interfaceRoutes.cend()),
        folly::range(staticRoutesToCpu.cbegin(), staticRoutesToCpu.cend()),
        folly::range(staticRoutesToNull.cbegin(), staticRoutesToNull.cend()),
        folly::range(
            staticRoutesWithNextHops.cbegin(), staticRoutesWithNextHops.cend()),
        nextState);

    configApplier.updateRibAndFib();
  }
}

RoutingInformationBase::UpdateStatistics RoutingInformationBase::update(
    RouterID routerID,
    ClientID clientID,
    AdminDistance adminDistanceFromClientID,
    const std::vector<UnicastRoute>& toAdd,
    const std::vector<IpPrefix>& toDelete,
    bool resetClientsRoutes,
    folly::StringPiece updateType,
    ApplyStateUpdateFunction updateStateBlockingFn) {
  UpdateStatistics stats;

  Timer updateTimer(&stats.duration);

  auto lockedRouteTables = synchronizedRouteTables_.wlock();

  auto it = lockedRouteTables->find(routerID);
  if (it == lockedRouteTables->end()) {
    throw FbossError("VRF ", routerID, " not configured");
  }

  RouteUpdater updater(
      &(it->second.v4NetworkToRoute), &(it->second.v6NetworkToRoute));

  if (resetClientsRoutes) {
    updater.removeAllRoutesForClient(clientID);
  }

  for (const auto& route : toAdd) {
    auto network = facebook::network::toIPAddress(route.dest.ip);
    auto mask = static_cast<uint8_t>(route.dest.prefixLength);

    if (network.isV4()) {
      ++stats.v4RoutesAdded;
    } else {
      ++stats.v6RoutesAdded;
    }

    updater.addRoute(
        network,
        mask,
        clientID,
        RouteNextHopEntry::from(route, adminDistanceFromClientID));
  }

  for (const auto& prefix : toDelete) {
    auto network = facebook::network::toIPAddress(prefix.ip);
    auto mask = static_cast<uint8_t>(prefix.prefixLength);

    if (network.isV4()) {
      ++stats.v4RoutesDeleted;
    } else {
      ++stats.v6RoutesDeleted;
    }

    updater.delRoute(network, mask, clientID);
  }

  updater.updateDone();

  ForwardingInformationBaseUpdater fibUpdater(
      routerID, it->second.v4NetworkToRoute, it->second.v6NetworkToRoute);

  updateStateBlockingFn(updateType, std::move(fibUpdater));

  return stats;
}

RoutingInformationBase::RouterIDToRouteTable
RoutingInformationBase::constructRouteTables(
    const SynchronizedRouteTables::WLockedPtr& lockedRouteTables,
    const RouterIDAndNetworkToInterfaceRoutes& configRouterIDToInterfaceRoutes)
    const {
  RouterIDToRouteTable newRouteTables;

  RouterIDToRouteTable::iterator newRouteTablesIter;
  for (const auto& routerIDAndInterfaceRoutes :
       configRouterIDToInterfaceRoutes) {
    const RouterID configVrf = routerIDAndInterfaceRoutes.first;

    newRouteTablesIter = newRouteTables.emplace_hint(
        newRouteTables.cend(), configVrf, RouteTable());

    auto oldRouteTablesIter = lockedRouteTables->find(configVrf);
    if (oldRouteTablesIter == lockedRouteTables->end()) {
      // configVrf did not exist in the RIB, so it has been added to
      // newRouteTables with an empty set of routes
      continue;
    }

    // configVrf exists in the RIB, so it will be shallow copied into
    // newRouteTables.
    newRouteTablesIter->second = std::move(oldRouteTablesIter->second);
  }

  return newRouteTables;
}

} // namespace rib
} // namespace fboss
} // namespace facebook
