// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/hw/bcm/BcmMultiPathNextHop.h"

#include "fboss/agent/hw/bcm/BcmHost.h"
#include "fboss/agent/hw/bcm/BcmIntf.h"
#include "fboss/agent/hw/bcm/BcmNextHop.h"
#include "fboss/agent/hw/bcm/BcmSwitch.h"
#include "fboss/agent/hw/bcm/BcmWarmBootCache.h"

namespace facebook {
namespace fboss {

BcmMultiPathNextHop::BcmMultiPathNextHop(
    const BcmSwitchIf* hw,
    BcmMultiPathNextHopKey key)
    : hw_(hw), vrf_(key.first) {
  auto& fwd = key.second;
  CHECK_GT(fwd.size(), 0);
  BcmEcmpEgress::Paths paths;
  std::vector<std::shared_ptr<BcmNextHop>> nexthops;
  // allocate a NextHop object for each path in this ECMP
  for (const auto& nhop : fwd) {
    auto nexthopSharedPtr = refOrEmplaceNextHop(getNextHopKey(vrf_, nhop));
    auto* nexthop = nexthopSharedPtr.get();
    // TODO:
    // Below comment appplies for L3 Nexthop only
    // Ideally, we should have the nexthop resolved already and programmed in
    // HW. If not, SW can preemptively trigger neighbor discovery and then
    // do the HW programming. For now, we program the egress object to punt
    // to CPU. Any traffic going to CPU will trigger the neighbor discovery.
    if (!nexthop->isProgrammed()) {
      const auto intf = hw->getIntfTable()->getBcmIntf(nhop.intf());
      nexthop->programToCPU(intf->getBcmIfId());
    }
    for (int i = 0; i < nhop.weight(); ++i) {
      paths.insert(nexthop->getEgressId());
    }
    nexthops.push_back(std::move(nexthopSharedPtr));
  }
  if (paths.size() > 1) {
    // BcmEcmpEgress object only for more than 1 paths.
    ecmpEgress_ = std::make_unique<BcmEcmpEgress>(hw, std::move(paths));
  }
  fwd_ = std::move(fwd);
  nexthops_ = std::move(nexthops);
}

std::shared_ptr<BcmNextHop> BcmMultiPathNextHop::refOrEmplaceNextHop(
    const HostKey& key) {
  if (key.hasLabel()) {
    return hw_->writableMplsNextHopTable()->referenceOrEmplaceNextHop(
        folly::poly_cast<facebook::fboss::BcmLabeledHostKey>(key));
  }
  return hw_->writableL3NextHopTable()->referenceOrEmplaceNextHop(
      folly::poly_cast<facebook::fboss::BcmHostKey>(key));
}

opennsl_if_t BcmMultiPathNextHop::getEgressId() const {
  return nexthops_.size() > 1 ? getEcmpEgressId()
                              : nexthops_.front()->getEgressId();
}

BcmMultiPathNextHop::~BcmMultiPathNextHop() {
  // Deref ECMP egress first since the ECMP egress entry holds references
  // to egress entries.
  XLOG(DBG3) << "Removing egress object for " << fwd_;
}

folly::dynamic BcmMultiPathNextHop::toFollyDynamic() const {
  folly::dynamic ecmpHost = folly::dynamic::object;
  ecmpHost[kVrf] = vrf_;
  folly::dynamic nhops = folly::dynamic::array;
  for (const auto& nhop : fwd_) {
    nhops.push_back(nhop.toFollyDynamic());
  }
  ecmpHost[kNextHops] = std::move(nhops);
  ecmpHost[kEgressId] = getEgressId();
  ecmpHost[kEcmpEgressId] = getEcmpEgressId();
  if (ecmpEgress_) {
    ecmpHost[kEcmpEgress] = ecmpEgress_->toFollyDynamic();
  }
  return ecmpHost;
}

long BcmMultiPathNextHopTable::getEcmpEgressCount() const {
  return std::count_if(
      getNextHops().begin(),
      getNextHops().end(),
      [](const auto& entry) -> bool {
        return entry.second.lock()->getEgress();
      });
}

void BcmMultiPathNextHopTable::egressResolutionChangedHwLocked(
    const BcmEcmpEgress::EgressIdSet& affectedEgressIds,
    BcmEcmpEgress::Action action) {
  if (action == BcmEcmpEgress::Action::SKIP) {
    return;
  }

  for (const auto& nextHopsAndEcmpHostInfo : getNextHops()) {
    auto weakPtr = nextHopsAndEcmpHostInfo.second;
    auto ecmpHost = weakPtr.lock();
    auto ecmpEgress = ecmpHost->getEgress();
    if (!ecmpEgress) {
      continue;
    }
    for (auto egrId : affectedEgressIds) {
      switch (action) {
        case BcmEcmpEgress::Action::EXPAND:
          ecmpEgress->pathReachableHwLocked(egrId);
          break;
        case BcmEcmpEgress::Action::SHRINK:
          ecmpEgress->pathUnreachableHwLocked(egrId);
          break;
        case BcmEcmpEgress::Action::SKIP:
          break;
        default:
          XLOG(FATAL) << "BcmEcmpEgress::Action matching not exhaustive";
          break;
      }
    }
  }
  /*
   * We may not have done a FIB sync before ports start coming
   * up or ARP/NDP start getting resolved/unresolved. In this case
   * we won't have BcmMultiPathNextHop entries, so we
   * look through the warm boot cache for ecmp egress entries.
   * Conversely post a FIB sync we won't have any ecmp egress IDs
   * in the warm boot cache
   */

  auto* hw = getBcmSwitch();
  for (const auto& ecmpAndEgressIds :
       hw->getWarmBootCache()->ecmp2EgressIds()) {
    for (auto path : affectedEgressIds) {
      switch (action) {
        case BcmEcmpEgress::Action::EXPAND:
          BcmEcmpEgress::addEgressIdHwLocked(
              hw->getUnit(),
              ecmpAndEgressIds.first,
              ecmpAndEgressIds.second,
              path);
          break;
        case BcmEcmpEgress::Action::SHRINK:
          BcmEcmpEgress::removeEgressIdHwLocked(
              hw->getUnit(), ecmpAndEgressIds.first, path);
          break;
        case BcmEcmpEgress::Action::SKIP:
          break;
        default:
          XLOG(FATAL) << "BcmEcmpEgress::Action matching not exhaustive";
          break;
      }
    }
  }
}

} // namespace fboss
} // namespace facebook
