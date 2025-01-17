/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiVirtualRouterManager.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/sai/api/SwitchApi.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"

#include <folly/logging/xlog.h>

namespace facebook {
namespace fboss {

SaiVirtualRouter::SaiVirtualRouter(
    SaiApiTable* apiTable,
    sai_object_id_t& switchId)
    : apiTable_(apiTable), attributes_({}) {
  default_ = true;
  try {
    // TODO(borisb): implement attributes_ via get apis for default VR
    id_ = apiTable_->switchApi().getAttribute(
        SwitchApiParameters::Attributes::DefaultVirtualRouterId(), switchId);
  } catch (const facebook::fboss::SaiApiError& ex) {
    // Create a default vritaul router if the SAI adapter
    // did not create a default virtual router and throws an exception.
    auto& virtualRouterApi = apiTable_->virtualRouterApi();
    id_ = virtualRouterApi.create(attributes_.attrs(), switchId);
  }
}

SaiVirtualRouter::SaiVirtualRouter(
    SaiApiTable* apiTable,
    const VirtualRouterApiParameters::Attributes& attributes,
    const sai_object_id_t& switchId)
    : apiTable_(apiTable), attributes_(attributes) {
  auto& virtualRouterApi = apiTable_->virtualRouterApi();
  id_ = virtualRouterApi.create(attributes_.attrs(), switchId);
}

SaiVirtualRouter::~SaiVirtualRouter() {
  if (default_) {
    return;
  }
  auto& virtualRouterApi = apiTable_->virtualRouterApi();
  virtualRouterApi.remove(id());
}

bool SaiVirtualRouter::operator==(const SaiVirtualRouter& other) const {
  return attributes_ == other.attributes();
}
bool SaiVirtualRouter::operator!=(const SaiVirtualRouter& other) const {
  return !(*this == other);
}

SaiVirtualRouterManager::SaiVirtualRouterManager(
    SaiApiTable* apiTable,
    SaiManagerTable* managerTable,
    const SaiPlatform* platform)
    : apiTable_(apiTable), managerTable_(managerTable), platform_(platform) {
  auto switchId = managerTable_->switchManager().getSwitchSaiId(SwitchID(0));
  auto defaultVirtualRouter =
      std::make_unique<SaiVirtualRouter>(apiTable_, switchId);
  virtualRouters_.emplace(
      std::make_pair(RouterID(0), std::move(defaultVirtualRouter)));
}

sai_object_id_t SaiVirtualRouterManager::addVirtualRouter(
    const RouterID& /* routerId */) {
  throw FbossError("Adding new virtual routers is not supported");
  folly::assume_unreachable();
}

SaiVirtualRouter* SaiVirtualRouterManager::getVirtualRouter(
    const RouterID& routerId) {
  auto itr = virtualRouters_.find(routerId);
  if (itr == virtualRouters_.end()) {
    return nullptr;
  }
  if (!itr->second) {
    XLOG(FATAL) << "invalid null virtual router for routerID: " << routerId;
  }
  return itr->second.get();
}

} // namespace fboss
} // namespace facebook
