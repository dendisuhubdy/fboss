/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiVlanManager.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/sai/switch/SaiBridgeManager.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"

#include <folly/logging/xlog.h>

#include <algorithm>

namespace facebook {
namespace fboss {

SaiVlanMember::SaiVlanMember(
    SaiApiTable* apiTable,
    const VlanApiParameters::MemberAttributes& attributes,
    const sai_object_id_t& switchId)
    : apiTable_(apiTable), attributes_(attributes) {
  auto& vlanApi = apiTable_->vlanApi();
  id_ = vlanApi.createMember(attributes_.attrs(), switchId);
}

SaiVlanMember::~SaiVlanMember() {
  auto& vlanApi = apiTable_->vlanApi();
  vlanApi.removeMember(id());
}

bool SaiVlanMember::operator==(const SaiVlanMember& other) const {
  return attributes_ == other.attributes_;
}

bool SaiVlanMember::operator!=(const SaiVlanMember& other) const {
  return !(*this == other);
}

SaiVlan::SaiVlan(
    SaiApiTable* apiTable,
    SaiManagerTable* managerTable,
    const VlanApiParameters::Attributes& attributes)
    : apiTable_(apiTable),
      managerTable_(managerTable),
      attributes_(attributes) {
  auto& vlanApi = apiTable_->vlanApi();
  auto switchId = managerTable_->switchManager().getSwitchSaiId(SwitchID(0));
  id_ = vlanApi.create(attributes_.attrs(), switchId);
}

SaiVlan::~SaiVlan() {
  members_.clear();
  auto& vlanApi = apiTable_->vlanApi();
  vlanApi.remove(id());
}

void SaiVlan::addMember(PortID swPortId) {
  VlanApiParameters::MemberAttributes::VlanId vlanIdMemberAttribute{id()};
  SaiPort* port = managerTable_->portManager().getPort(swPortId);
  auto switchId = managerTable_->switchManager().getSwitchSaiId(SwitchID(0));
  if (!port) {
    throw FbossError(
        "Failed to add vlan member: no port matching vlan member port: ",
        swPortId);
  }
  sai_object_id_t bridgePortId = port->getBridgePort()->id();
  port->setPortVlan(managerTable_->vlanManager().getVlanID(id()));
  VlanApiParameters::MemberAttributes::BridgePortId bridgePortIdAttribute{
      bridgePortId};
  VlanApiParameters::MemberAttributes memberAttributes{
      {vlanIdMemberAttribute, bridgePortIdAttribute}};
  auto member =
      std::make_unique<SaiVlanMember>(apiTable_, memberAttributes, switchId);
  sai_object_id_t memberId = member->id();
  members_.insert(std::make_pair(memberId, std::move(member)));
  memberIdMap_.insert(std::make_pair(memberAttributes.bridgePortId, memberId));
}

void SaiVlan::removeMember(PortID swPortId) {
  SaiPort* port = managerTable_->portManager().getPort(swPortId);
  if (!port) {
    throw FbossError(
        "Failed to remove vlan membmer: no port matching vlan member port: ",
        swPortId);
  }
  sai_object_id_t bridgePortId = port->getBridgePort()->id();
  members_.erase(bridgePortId);
  memberIdMap_.erase(bridgePortId);
}

std::vector<sai_object_id_t> SaiVlan::getMemberBridgePortIds() const {
  std::vector<sai_object_id_t> bridgePortIds;
  std::transform(
      memberIdMap_.begin(),
      memberIdMap_.end(),
      std::back_inserter(bridgePortIds),
      [](const std::pair<sai_object_id_t, sai_object_id_t>& idMapping) {
        return idMapping.first;
      });
  return bridgePortIds;
}

bool SaiVlan::operator==(const SaiVlan& other) const {
  if (attributes_ != other.attributes_) {
    return false;
  }
  return members_ == other.members_;
}

bool SaiVlan::operator!=(const SaiVlan& other) const {
  return !(*this == other);
}

SaiVlanManager::SaiVlanManager(
    SaiApiTable* apiTable,
    SaiManagerTable* managerTable,
    const SaiPlatform* platform)
    : apiTable_(apiTable), managerTable_(managerTable), platform_(platform) {}

sai_object_id_t SaiVlanManager::addVlan(const std::shared_ptr<Vlan>& swVlan) {
  VlanID swVlanId = swVlan->getID();
  auto existingVlan = getVlan(swVlanId);
  if (existingVlan) {
    throw FbossError(
        "attempted to add a duplicate vlan with VlanID: ", swVlanId);
  }
  VlanApiParameters::Attributes::VlanId vlanIdAttribute{swVlanId};
  VlanApiParameters::Attributes attributes{{vlanIdAttribute}};
  auto saiVlan =
      std::make_unique<SaiVlan>(apiTable_, managerTable_, attributes);
  sai_object_id_t saiId = saiVlan->id();
  vlanSaiIds_.emplace(std::make_pair(saiVlan->id(), swVlanId));
  for (const auto& memberPort : swVlan->getPorts()) {
    PortID swPortId = memberPort.first;
    saiVlan->addMember(swPortId);
  }
  vlans_.insert(std::make_pair(swVlanId, std::move(saiVlan)));
  return saiId;
}

void SaiVlanManager::removeVlan(const VlanID& swVlanId) {
  const auto citr = vlans_.find(swVlanId);
  if (citr == vlans_.cend()) {
    throw FbossError(
        "attempted to remove a vlan which does not exist: ", swVlanId);
  }
  vlanSaiIds_.erase(citr->second->id());
  vlans_.erase(citr);
}

void SaiVlanManager::changeVlan(
    const std::shared_ptr<Vlan>& swVlanOld,
    const std::shared_ptr<Vlan>& swVlanNew) {
  VlanID swVlanId = swVlanNew->getID();
  SaiVlan* vlan = getVlan(swVlanId);
  if (!vlan) {
    throw FbossError(
        "attempted to change a vlan which does not exist: ", swVlanId);
  }
  const VlanFields::MemberPorts& oldPorts = swVlanOld->getPorts();
  auto compareIds = [](const std::pair<PortID, VlanFields::PortInfo>& p1,
                       const std::pair<PortID, VlanFields::PortInfo>& p2) {
    return p1.first < p2.first;
  };
  const VlanFields::MemberPorts& newPorts = swVlanNew->getPorts();
  VlanFields::MemberPorts removed;
  std::set_difference(
      oldPorts.begin(),
      oldPorts.end(),
      newPorts.begin(),
      newPorts.end(),
      std::inserter(removed, removed.begin()),
      compareIds);
  for (const auto& swPortId : removed) {
    vlan->removeMember(swPortId.first);
  }
  VlanFields::MemberPorts added;
  std::set_difference(
      newPorts.begin(),
      newPorts.end(),
      oldPorts.begin(),
      oldPorts.end(),
      std::inserter(added, added.begin()),
      compareIds);
  for (const auto& swPortId : added) {
    vlan->addMember(swPortId.first);
  }
}

void SaiVlanManager::processVlanDelta(const VlanMapDelta& delta) {
  auto processChanged = [this](const auto& oldVlan, const auto& newVlan) {
    changeVlan(oldVlan, newVlan);
  };
  auto processAdded = [this](const auto& newVlan) { addVlan(newVlan); };
  auto processRemoved = [this](const auto& oldVlan) {
    removeVlan(oldVlan->getID());
  };
  DeltaFunctions::forEachChanged(
      delta, processChanged, processAdded, processRemoved);
}

SaiVlan* SaiVlanManager::getVlan(VlanID swVlanId) {
  return getVlanImpl(swVlanId);
}
const SaiVlan* SaiVlanManager::getVlan(VlanID swVlanId) const {
  return getVlanImpl(swVlanId);
}
SaiVlan* SaiVlanManager::getVlanImpl(VlanID swVlanId) const {
  auto itr = vlans_.find(swVlanId);
  if (itr == vlans_.end()) {
    return nullptr;
  }
  if (!itr->second) {
    XLOG(FATAL) << "invalid null VLAN for VlanID: " << swVlanId;
  }
  return itr->second.get();
}

VlanID SaiVlanManager::getVlanID(sai_object_id_t saiVlanId) {
  auto itr = vlanSaiIds_.find(saiVlanId);
  if (itr == vlanSaiIds_.end()) {
    return VlanID(0);
  }
  return itr->second;
}

} // namespace fboss
} // namespace facebook
