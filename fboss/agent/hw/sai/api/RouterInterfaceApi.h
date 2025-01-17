/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "fboss/agent/hw/sai/api/SaiApi.h"
#include "fboss/agent/hw/sai/api/SaiAttribute.h"
#include "fboss/agent/hw/sai/api/SaiAttributeDataTypes.h"

#include <folly/logging/xlog.h>

#include <boost/variant.hpp>

extern "C" {
#include <sai.h>
}

namespace facebook {
namespace fboss {

struct RouterInterfaceApiParameters {
  struct Attributes {
    using EnumType = sai_router_interface_attr_t;
    using SrcMac = SaiAttribute<
        EnumType,
        SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS,
        folly::MacAddress>;
    using Type =
        SaiAttribute<EnumType, SAI_ROUTER_INTERFACE_ATTR_TYPE, sai_int32_t>;
    using VirtualRouterId = SaiAttribute<
        EnumType,
        SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID,
        SaiObjectIdT>;
    using VlanId =
        SaiAttribute<EnumType, SAI_ROUTER_INTERFACE_ATTR_VLAN_ID, SaiObjectIdT>;
    using CreateAttributes = SaiAttributeTuple<
        VirtualRouterId,
        Type,
        VlanId,
        SaiAttributeOptional<SrcMac>>;
    Attributes(const CreateAttributes& attrs) {
      std::tie(virtualRouterId, type, vlanId, srcMac) = attrs.value();
    }
    CreateAttributes attrs() const {
      return {virtualRouterId, type, vlanId, srcMac};
    }
    bool operator==(const Attributes& other) const {
      return attrs() == other.attrs();
    }
    bool operator!=(const Attributes& other) const {
      return !(*this == other);
    }
    VirtualRouterId::ValueType virtualRouterId;
    Type::ValueType type;
    VlanId::ValueType vlanId;
    folly::Optional<SrcMac::ValueType> srcMac;
  };
};

class RouterInterfaceApi
    : public SaiApi<RouterInterfaceApi, RouterInterfaceApiParameters> {
 public:
  RouterInterfaceApi() {
    sai_status_t status = sai_api_query(
        SAI_API_ROUTER_INTERFACE, reinterpret_cast<void**>(&api_));
    saiCheckError(status, "Failed to query for router interface api");
  }
  RouterInterfaceApi(const RouterInterfaceApi& other) = delete;

 private:
  sai_status_t _create(
      sai_object_id_t* router_interface_id,
      sai_attribute_t* attr_list,
      size_t count,
      sai_object_id_t switch_id) {
    return api_->create_router_interface(
        router_interface_id, switch_id, count, attr_list);
  }
  sai_status_t _remove(sai_object_id_t router_interface_id) {
    return api_->remove_router_interface(router_interface_id);
  }
  sai_status_t _getAttr(sai_attribute_t* attr, sai_object_id_t handle) const {
    return api_->get_router_interface_attribute(handle, 1, attr);
  }
  sai_status_t _setAttr(const sai_attribute_t* attr, sai_object_id_t handle) {
    return api_->set_router_interface_attribute(handle, attr);
  }
  sai_router_interface_api_t* api_;
  friend class SaiApi<RouterInterfaceApi, RouterInterfaceApiParameters>;
};

} // namespace fboss
} // namespace facebook
