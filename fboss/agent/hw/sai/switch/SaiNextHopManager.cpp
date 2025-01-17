/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiNextHopManager.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"

#include <folly/logging/xlog.h>

namespace facebook {
namespace fboss {

SaiNextHop::SaiNextHop(
    SaiApiTable* apiTable,
    const NextHopApiParameters::Attributes& attributes,
    const sai_object_id_t& switchId)
    : apiTable_(apiTable), attributes_(attributes) {
  auto& nextHopApi = apiTable_->nextHopApi();
  id_ = nextHopApi.create(attributes_.attrs(), switchId);
}

SaiNextHop::~SaiNextHop() {
  auto& nextHopApi = apiTable_->nextHopApi();
  nextHopApi.remove(id_);
}

bool SaiNextHop::operator==(const SaiNextHop& other) const {
  return attributes_ == other.attributes();
}
bool SaiNextHop::operator!=(const SaiNextHop& other) const {
  return !(*this == other);
}

std::unique_ptr<SaiNextHop> SaiNextHopManager::addNextHop(
    sai_object_id_t routerInterfaceId,
    const folly::IPAddress& ip) {
  NextHopApiParameters::Attributes attributes{
      {SAI_NEXT_HOP_TYPE_IP, routerInterfaceId, ip}};
  auto switchId = managerTable_->switchManager().getSwitchSaiId(SwitchID(0));
  return std::make_unique<SaiNextHop>(apiTable_, attributes, switchId);
}

SaiNextHopManager::SaiNextHopManager(
    SaiApiTable* apiTable,
    SaiManagerTable* managerTable,
    const SaiPlatform* platform)
    : apiTable_(apiTable), managerTable_(managerTable), platform_(platform) {}
} // namespace fboss
} // namespace facebook
