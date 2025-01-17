/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"

#include "fboss/agent/hw/sai/api/SaiApiTable.h"
#include "fboss/agent/hw/sai/switch/SaiBridgeManager.h"
#include "fboss/agent/hw/sai/switch/SaiFdbManager.h"
#include "fboss/agent/hw/sai/switch/SaiNeighborManager.h"
#include "fboss/agent/hw/sai/switch/SaiNextHopGroupManager.h"
#include "fboss/agent/hw/sai/switch/SaiNextHopManager.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"
#include "fboss/agent/hw/sai/switch/SaiRouteManager.h"
#include "fboss/agent/hw/sai/switch/SaiRouterInterfaceManager.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"
#include "fboss/agent/hw/sai/switch/SaiVirtualRouterManager.h"
#include "fboss/agent/hw/sai/switch/SaiVlanManager.h"

namespace facebook {
namespace fboss {

SaiManagerTable::SaiManagerTable(
    SaiApiTable* apiTable,
    const SaiPlatform* platform)
    : apiTable_(apiTable) {
  switchManager_ =
      std::make_unique<SaiSwitchManager>(apiTable_, this, platform);
  bridgeManager_ =
      std::make_unique<SaiBridgeManager>(apiTable_, this, platform);
  fdbManager_ = std::make_unique<SaiFdbManager>(apiTable_, this, platform);
  portManager_ = std::make_unique<SaiPortManager>(apiTable_, this, platform);
  virtualRouterManager_ =
      std::make_unique<SaiVirtualRouterManager>(apiTable_, this, platform);
  vlanManager_ = std::make_unique<SaiVlanManager>(apiTable_, this, platform);
  routeManager_ = std::make_unique<SaiRouteManager>(apiTable_, this, platform);
  routerInterfaceManager_ =
      std::make_unique<SaiRouterInterfaceManager>(apiTable_, this, platform);
  nextHopManager_ =
      std::make_unique<SaiNextHopManager>(apiTable_, this, platform);
  nextHopGroupManager_ =
      std::make_unique<SaiNextHopGroupManager>(apiTable_, this, platform);
  neighborManager_ =
      std::make_unique<SaiNeighborManager>(apiTable_, this, platform);
}

SaiManagerTable::~SaiManagerTable() {
  // Need to destroy routes before destroying other managers, as the
  // route destructor will trigger calls in those managers
  routeManager().clear();
  routerInterfaceManager_.reset();
  portManager_.reset();
  bridgeManager_.reset();
  vlanManager_.reset();
  switchManager_.reset();
}

SaiBridgeManager& SaiManagerTable::bridgeManager() {
  return *bridgeManager_;
}
const SaiBridgeManager& SaiManagerTable::bridgeManager() const {
  return *bridgeManager_;
}

SaiFdbManager& SaiManagerTable::fdbManager() {
  return *fdbManager_;
}
const SaiFdbManager& SaiManagerTable::fdbManager() const {
  return *fdbManager_;
}

SaiNeighborManager& SaiManagerTable::neighborManager() {
  return *neighborManager_;
}
const SaiNeighborManager& SaiManagerTable::neighborManager() const {
  return *neighborManager_;
}

SaiNextHopManager& SaiManagerTable::nextHopManager() {
  return *nextHopManager_;
}
const SaiNextHopManager& SaiManagerTable::nextHopManager() const {
  return *nextHopManager_;
}

SaiNextHopGroupManager& SaiManagerTable::nextHopGroupManager() {
  return *nextHopGroupManager_;
}
const SaiNextHopGroupManager& SaiManagerTable::nextHopGroupManager() const {
  return *nextHopGroupManager_;
}

SaiPortManager& SaiManagerTable::portManager() {
  return *portManager_;
}
const SaiPortManager& SaiManagerTable::portManager() const {
  return *portManager_;
}

SaiRouteManager& SaiManagerTable::routeManager() {
  return *routeManager_;
}
const SaiRouteManager& SaiManagerTable::routeManager() const {
  return *routeManager_;
}

SaiRouterInterfaceManager& SaiManagerTable::routerInterfaceManager() {
  return *routerInterfaceManager_;
}
const SaiRouterInterfaceManager& SaiManagerTable::routerInterfaceManager()
    const {
  return *routerInterfaceManager_;
}

SaiSwitchManager& SaiManagerTable::switchManager() {
  return *switchManager_;
}
const SaiSwitchManager& SaiManagerTable::switchManager() const {
  return *switchManager_;
}

SaiVirtualRouterManager& SaiManagerTable::virtualRouterManager() {
  return *virtualRouterManager_;
}
const SaiVirtualRouterManager& SaiManagerTable::virtualRouterManager() const {
  return *virtualRouterManager_;
}

SaiVlanManager& SaiManagerTable::vlanManager() {
  return *vlanManager_;
}
const SaiVlanManager& SaiManagerTable::vlanManager() const {
  return *vlanManager_;
}

} // namespace fboss
} // namespace facebook
