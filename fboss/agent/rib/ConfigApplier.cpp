/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/rib/ConfigApplier.h"
#include "fboss/agent/rib/ForwardingInformationBaseUpdater.h"
#include "fboss/agent/rib/RouteUpdater.h"
#include "fboss/agent/rib/RoutingInformationBase.h"
#include "fboss/agent/state/ForwardingInformationBaseContainer.h"

#include <algorithm>

namespace facebook {
namespace fboss {
namespace rib {

ConfigApplier::ConfigApplier(
    RouterID vrf,
    IPv4NetworkToRouteMap* v4NetworkToRoute,
    IPv6NetworkToRouteMap* v6NetworkToRoute,
    folly::Range<DirectlyConnectedRouteIterator> directlyConnectedRouteRange,
    folly::Range<StaticRouteNoNextHopsIterator> staticCpuRouteRange,
    folly::Range<StaticRouteNoNextHopsIterator> staticDropRouteRange,
    folly::Range<StaticRouteWithNextHopsIterator> staticRouteRange,
    const std::shared_ptr<facebook::fboss::SwitchState>& nextState)
    : vrf_(vrf),
      v4NetworkToRoute_(v4NetworkToRoute),
      v6NetworkToRoute_(v6NetworkToRoute),
      directlyConnectedRouteRange_(directlyConnectedRouteRange),
      staticCpuRouteRange_(staticCpuRouteRange),
      staticDropRouteRange_(staticDropRouteRange),
      staticRouteRange_(staticRouteRange),
      nextState_(nextState) {
  CHECK_NOTNULL(v4NetworkToRoute_);
  CHECK_NOTNULL(v6NetworkToRoute_);
}

void ConfigApplier::updateRibAndFib() {
  RouteUpdater updater(v4NetworkToRoute_, v6NetworkToRoute_);

  // Update static routes
  updater.removeAllRoutesForClient(
      StdClientIds2ClientID(StdClientIds::STATIC_ROUTE));

  for (const auto& staticRoute : staticCpuRouteRange_) {
    if (RouterID(staticRoute.routerID) != vrf_) {
      continue;
    }

    auto prefix = folly::IPAddress::createNetwork(staticRoute.prefix);
    updater.addRoute(
        prefix.first,
        prefix.second,
        StdClientIds2ClientID(StdClientIds::STATIC_ROUTE),
        RouteNextHopEntry::createToCpu());
  }
  for (const auto& staticRoute : staticDropRouteRange_) {
    if (RouterID(staticRoute.routerID) != vrf_) {
      continue;
    }

    auto prefix = folly::IPAddress::createNetwork(staticRoute.prefix);
    updater.addRoute(
        prefix.first,
        prefix.second,
        StdClientIds2ClientID(StdClientIds::STATIC_ROUTE),
        RouteNextHopEntry::createDrop());
  }
  for (const auto& staticRoute : staticRouteRange_) {
    if (RouterID(staticRoute.routerID) != vrf_) {
      continue;
    }

    auto prefix = folly::IPAddress::createNetwork(staticRoute.prefix);
    updater.addRoute(
        prefix.first,
        prefix.second,
        StdClientIds2ClientID(StdClientIds::STATIC_ROUTE),
        RouteNextHopEntry::fromStaticRoute(staticRoute));
  }

  // Update interface routes
  updater.removeAllRoutesForClient(
      StdClientIds2ClientID(StdClientIds::INTERFACE_ROUTE));
  addInterfaceRoutes(&updater, directlyConnectedRouteRange_);

  // Add link-local routes
  updater.addLinkLocalRoutes();

  // Trigger recrusive resolution
  updater.updateDone();

  ForwardingInformationBaseUpdater fibUpdater(
      vrf_, *v4NetworkToRoute_, *v6NetworkToRoute_);

  fibUpdater(nextState_);
}

void ConfigApplier::addInterfaceRoutes(
    RouteUpdater* updater,
    folly::Range<DirectlyConnectedRouteIterator> directlyConnectedRoutesRange) {
  for (const auto& directlyConnectedRoute : directlyConnectedRoutesRange) {
    auto network = directlyConnectedRoute.first;
    auto interfaceID = directlyConnectedRoute.second.first;
    auto endpoint = directlyConnectedRoute.second.second;
    updater->addInterfaceRoute(
        network.first, network.second, endpoint, interfaceID);
  }
}

} // namespace rib
} // namespace fboss
} // namespace facebook
