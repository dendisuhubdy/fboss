/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "BcmRoute.h"

extern "C" {
#include <opennsl/l3.h>
}

#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/logging/xlog.h>
#include "fboss/agent/Constants.h"
#include "fboss/agent/hw/bcm/BcmError.h"
#include "fboss/agent/hw/bcm/BcmHost.h"
#include "fboss/agent/hw/bcm/BcmIntf.h"
#include "fboss/agent/hw/bcm/BcmMultiPathNextHop.h"
#include "fboss/agent/hw/bcm/BcmPlatform.h"
#include "fboss/agent/hw/bcm/BcmSwitch.h"
#include "fboss/agent/hw/bcm/BcmWarmBootCache.h"
#include "fboss/agent/if/gen-cpp2/ctrl_types.h"

#include "fboss/agent/state/RouteTypes.h"

namespace facebook { namespace fboss {

namespace {
auto constexpr kAction = "action";
auto constexpr kEcmp = "ecmp";
auto constexpr kForwardInfo = "forwardInfo";
auto constexpr kMaskLen = "maskLen";
auto constexpr kNetwork = "network";
auto constexpr kRoutes = "routes";

// TODO: Assumes we have only one VRF
auto constexpr kDefaultVrf = 0;
}

BcmRoute::BcmRoute(
    BcmSwitch* hw,
    opennsl_vrf_t vrf,
    const folly::IPAddress& addr,
    uint8_t len)
    : hw_(hw), vrf_(vrf), prefix_(addr), len_(len) {}

void BcmRoute::initL3RouteFromArgs(opennsl_l3_route_t* rt,
                                   opennsl_vrf_t vrf,
                                   const folly::IPAddress& prefix,
                                   uint8_t prefixLength) {
  opennsl_l3_route_t_init(rt);
  rt->l3a_vrf = vrf;
  if (prefix.isV4()) {
    // both l3a_subnet and l3a_ip_mask for IPv4 are in host order
    rt->l3a_subnet = prefix.asV4().toLongHBO();
    rt->l3a_ip_mask =
        folly::IPAddressV4(folly::IPAddressV4::fetchMask(prefixLength))
            .toLongHBO();
  } else {
    memcpy(&rt->l3a_ip6_net, prefix.asV6().toByteArray().data(),
           sizeof(rt->l3a_ip6_net));
    memcpy(&rt->l3a_ip6_mask,
           folly::IPAddressV6::fetchMask(prefixLength).data(),
           sizeof(rt->l3a_ip6_mask));
    rt->l3a_flags |= OPENNSL_L3_IP6;
  }
}

void BcmRoute::initL3RouteT(opennsl_l3_route_t* rt) const {
  initL3RouteFromArgs(rt, vrf_, prefix_, len_);
}

bool BcmRoute::isHostRoute() const {
  return prefix_.isV6() ? len_ == 128 : len_ == 32;
}

bool BcmRoute::canUseHostTable() const {
  return isHostRoute() && hw_->getPlatform()->canUseHostTableForHostRoutes();
}

void BcmRoute::program(const RouteNextHopEntry& fwd) {
  // if the route has been programmed to the HW, check if the forward info is
  // changed or not. If not, nothing to do.
  if (added_ && fwd == fwd_) {
    return;
  }
  std::shared_ptr<BcmMultiPathNextHop> nexthopReference;

  auto action = fwd.getAction();
  // find out the egress object ID
  opennsl_if_t egressId;
  if (action == RouteForwardAction::DROP) {
    egressId = hw_->getDropEgressId();
  } else if (action == RouteForwardAction::TO_CPU) {
    egressId = hw_->getToCPUEgressId();
  } else {
    CHECK(action == RouteForwardAction::NEXTHOPS);
    const auto& nhops = fwd.getNextHopSet();
    CHECK_GT(nhops.size(), 0);
    // need to get an entry from the host table for the forward info
    nexthopReference =
        hw_->writableMultiPathNextHopTable()->referenceOrEmplaceNextHop(
            BcmMultiPathNextHopKey(vrf_, fwd.getNextHopSet()));
    egressId = nexthopReference->getEgressId();
  }

  // At this point host and egress objects for next hops have been
  // created, what remains to be done is to program route into the
  // route table or host table (if this is a host route and use of
  // host table for host routes is allowed by the chip).
  if (canUseHostTable()) {
    auto warmBootCache = hw_->getWarmBootCache();
    auto vrfAndIP2RouteCitr =
        warmBootCache->findHostRouteFromRouteTable(vrf_, prefix_);
    bool entryExistsInRouteTable =
        vrfAndIP2RouteCitr != warmBootCache->vrfAndIP2Route_end();
    if (hostRouteEntry_) {
      XLOG(DBG3) << "Dereferencing host prefix for " << prefix_ << "/"
                 << static_cast<int>(len_) << " host egress Id : "
                 << hostRouteEntry_->getEgressId();
      hostRouteEntry_.reset();
    }
    hostRouteEntry_ =
        programHostRoute(egressId, fwd, entryExistsInRouteTable);
    if (entryExistsInRouteTable) {
      // If the entry already exists in the route table, programHostRoute()
      // removes it as well.
      DCHECK(!BcmRoute::deleteLpmRoute(hw_->getUnit(), vrf_, prefix_, len_));
      warmBootCache->programmed(vrfAndIP2RouteCitr);
    }
  } else {
    programLpmRoute(egressId, fwd);
  }
  nextHopHostReference_ = std::move(nexthopReference);
  egressId_ = egressId;
  fwd_ = fwd;

  // new nexthop has been stored in fwd_. From now on, it is up to
  // ~BcmRoute() to clean up such nexthop.
  added_ = true;
}

std::shared_ptr<BcmHost> BcmRoute::programHostRoute(
    opennsl_if_t egressId,
    const RouteNextHopEntry& fwd,
    bool replace) {
  XLOG(DBG3) << "creating a host route entry for " << prefix_.str()
             << " @egress " << egressId << " with " << fwd;
  auto prefixHost =
      hw_->writableHostTable()->refOrEmplace(BcmHostKey(vrf_, prefix_));
  prefixHost->setEgressId(egressId);
  prefixHost->addToBcmHostTable(fwd.getNextHopSet().size() > 1, replace);
  return prefixHost;
}

void BcmRoute::programLpmRoute(opennsl_if_t egressId,
    const RouteNextHopEntry& fwd) {
  opennsl_l3_route_t rt;
  initL3RouteT(&rt);
  rt.l3a_intf = egressId;
  if (fwd.getNextHopSet().size() > 1) {         // multipath
    rt.l3a_flags |= OPENNSL_L3_MULTIPATH;
  } else if (fwd.getAction() == RouteForwardAction::DROP) {
    rt.l3a_flags |= OPENNSL_L3_DST_DISCARD;
  }

  bool addRoute = false;
  const auto warmBootCache = hw_->getWarmBootCache();
  auto vrfAndPfx2RouteCitr = warmBootCache->findRoute(vrf_, prefix_, len_);
  if (vrfAndPfx2RouteCitr != warmBootCache->vrfAndPrefix2Route_end()) {
    // Lambda to compare if the routes are equivalent and thus we need to
    // do nothing
    auto equivalent =
      [=] (const opennsl_l3_route_t& newRoute,
           const opennsl_l3_route_t& existingRoute) {
      // Compare flags (primarily MULTIPATH vs non MULTIPATH
      // and egress id.
      return existingRoute.l3a_flags == newRoute.l3a_flags &&
      existingRoute.l3a_intf == newRoute.l3a_intf;
    };
    if (!equivalent(rt, vrfAndPfx2RouteCitr->second)) {
      XLOG(DBG3) << "Updating route for : " << prefix_ << "/"
                 << static_cast<int>(len_) << " in vrf : " << vrf_;
      // This is a change
      rt.l3a_flags |= OPENNSL_L3_REPLACE;
      addRoute = true;
    } else {
      XLOG(DBG3) << " Route for : " << prefix_ << "/" << static_cast<int>(len_)
                 << " in vrf : " << vrf_ << " already exists";
    }
  } else {
    addRoute = true;
  }
  if (addRoute) {
    if (vrfAndPfx2RouteCitr == warmBootCache->vrfAndPrefix2Route_end()) {
      XLOG(DBG3) << "Adding route for : " << prefix_ << "/"
                 << static_cast<int>(len_) << " in vrf : " << vrf_;
    }
    if (added_) {
      rt.l3a_flags |= OPENNSL_L3_REPLACE;
    }
    auto rc = opennsl_l3_route_add(hw_->getUnit(), &rt);
    bcmCheckError(rc, "failed to create a route entry for ", prefix_, "/",
        static_cast<int>(len_), " @ ", fwd, " @egress ", egressId);
    XLOG(DBG3) << "created a route entry for " << prefix_.str() << "/"
               << static_cast<int>(len_) << " @egress " << egressId << " with "
               << fwd;
  }
  if (vrfAndPfx2RouteCitr != warmBootCache->vrfAndPrefix2Route_end()) {
    warmBootCache->programmed(vrfAndPfx2RouteCitr);
  }
}

bool BcmRoute::deleteLpmRoute(int unitNumber,
                              opennsl_vrf_t vrf,
                              const folly::IPAddress& prefix,
                              uint8_t prefixLength) {
  opennsl_l3_route_t rt;
  initL3RouteFromArgs(&rt, vrf, prefix, prefixLength);
  auto rc = opennsl_l3_route_delete(unitNumber, &rt);
  if (OPENNSL_FAILURE(rc)) {
    XLOG(ERR) << "Failed to delete a route entry for " << prefix << "/"
              << static_cast<int>(prefixLength)
              << " Error: " << opennsl_errmsg(rc);
    return false;
  } else {
    XLOG(DBG3) << "deleted a route entry for " << prefix.str() << "/"
               << static_cast<int>(prefixLength);
  }
  return true;
}

folly::dynamic BcmRoute::toFollyDynamic() const {
  folly::dynamic route = folly::dynamic::object;
  route[kNetwork] = prefix_.str();
  route[kMaskLen] = len_;
  route[kAction] = forwardActionStr(fwd_.getAction());
  // if many next hops, put ecmpEgressId = egressId
  // else put egressId = egressId
  if (fwd_.getNextHopSet().size() > 1) {
    route[kEcmp] = true;
    route[kEcmpEgressId] = egressId_;
  } else {
    route[kEcmp] = false;
    route[kEgressId] = egressId_;
  }
  return route;
}

BcmRoute::~BcmRoute() {
  if (!added_) {
    return;
  }
  if (canUseHostTable()) {
    auto* host = hostRouteEntry_.get();
    CHECK(host);
    XLOG(DBG3) << "Deleting host route; derefrence host prefix for : "
               << prefix_ << "/" << static_cast<int>(len_) << " host: " << host;
  } else {
    deleteLpmRoute(hw_->getUnit(), vrf_, prefix_, len_);
  }
}

bool BcmRouteTable::Key::operator<(const Key& k2) const {
  if (vrf < k2.vrf) {
    return true;
  } else if (vrf > k2.vrf) {
    return false;
  }
  if (mask < k2.mask) {
    return true;
  } else if (mask > k2.mask) {
    return false;
  }
  return network < k2.network;
}

BcmRouteTable::BcmRouteTable(BcmSwitch* hw) : hw_(hw) {}

BcmRouteTable::~BcmRouteTable() {

}

BcmRoute* BcmRouteTable::getBcmRouteIf(
    opennsl_vrf_t vrf, const folly::IPAddress& network, uint8_t mask) const {
  Key key{network, mask, vrf};
  auto iter = fib_.find(key);
  if (iter == fib_.end()) {
    return nullptr;
  }
  return iter->second.get();
}

BcmRoute* BcmRouteTable::getBcmRoute(
    opennsl_vrf_t vrf, const folly::IPAddress& network, uint8_t mask) const {
  auto rt = getBcmRouteIf(vrf, network, mask);
  if (!rt) {
    throw FbossError("Cannot find route for ", network,
                     "/", static_cast<uint32_t>(mask), " @ vrf ", vrf);
  }
  return rt;
}

template<typename RouteT>
void BcmRouteTable::addRoute(opennsl_vrf_t vrf, const RouteT *route) {
  const auto& prefix = route->prefix();

  Key key{folly::IPAddress(prefix.network), prefix.mask, vrf};
  auto ret = fib_.emplace(key, nullptr);
  if (ret.second) {
    SCOPE_FAIL {
      fib_.erase(ret.first);
    };
    ret.first->second.reset(new BcmRoute(hw_, vrf,
                                        folly::IPAddress(prefix.network),
                                        prefix.mask));
  }
  CHECK(route->isResolved());
  RouteNextHopEntry fwd(route->getForwardInfo());
  if (fwd.getAction() == RouteForwardAction::NEXTHOPS) {
    fwd = RouteNextHopEntry(fwd.normalizedNextHops(), fwd.getAdminDistance());
  }
  ret.first->second->program(fwd);
}

template<typename RouteT>
void BcmRouteTable::deleteRoute(opennsl_vrf_t vrf, const RouteT *route) {
  const auto& prefix = route->prefix();
  Key key{folly::IPAddress(prefix.network), prefix.mask, vrf};
  auto iter = fib_.find(key);
  if (iter == fib_.end()) {
    throw FbossError("Failed to delete a non-existing route ", route->str());
  }
  fib_.erase(iter);
}

folly::dynamic BcmRouteTable::toFollyDynamic() const {
  folly::dynamic routesJson = folly::dynamic::array;
  for (const auto& route : fib_) {
    routesJson.push_back(route.second->toFollyDynamic());
  }
  folly::dynamic routeTable = folly::dynamic::object;
  routeTable[kRoutes] = std::move(routesJson);
  return routeTable;
}

template void BcmRouteTable::addRoute(opennsl_vrf_t, const RouteV4 *);
template void BcmRouteTable::addRoute(opennsl_vrf_t, const RouteV6 *);
template void BcmRouteTable::deleteRoute(opennsl_vrf_t, const RouteV4 *);
template void BcmRouteTable::deleteRoute(opennsl_vrf_t, const RouteV6 *);
}}
