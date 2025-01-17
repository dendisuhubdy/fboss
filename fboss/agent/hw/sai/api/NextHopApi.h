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

struct NextHopApiParameters {
  struct Attributes {
    using EnumType = sai_next_hop_attr_t;
    using Ip = SaiAttribute<EnumType, SAI_NEXT_HOP_ATTR_IP, folly::IPAddress>;
    using RouterInterfaceId = SaiAttribute<
        EnumType,
        SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID,
        SaiObjectIdT>;
    using Type = SaiAttribute<EnumType, SAI_NEXT_HOP_ATTR_TYPE, sai_int32_t>;
    using CreateAttributes = SaiAttributeTuple<Type, RouterInterfaceId, Ip>;
    Attributes(const CreateAttributes& attrs) {
      std::tie(type, routerInterfaceId, ip) = attrs.value();
    }
    CreateAttributes attrs() const {
      return {type, routerInterfaceId, ip};
    }
    bool operator==(const Attributes& other) const {
      return attrs() == other.attrs();
    }
    bool operator!=(const Attributes& other) const {
      return !(*this == other);
    }
    Type::ValueType type;
    RouterInterfaceId::ValueType routerInterfaceId;
    Ip::ValueType ip;
  };
};

class NextHopApi : public SaiApi<NextHopApi, NextHopApiParameters> {
 public:
  NextHopApi() {
    sai_status_t status =
        sai_api_query(SAI_API_NEXT_HOP, reinterpret_cast<void**>(&api_));
    saiCheckError(status, "Failed to query for next hop api");
  }

 private:
  sai_status_t _create(
      sai_object_id_t* next_hop_id,
      sai_attribute_t* attr_list,
      size_t count,
      sai_object_id_t switch_id) {
    return api_->create_next_hop(next_hop_id, switch_id, count, attr_list);
  }
  sai_status_t _remove(sai_object_id_t next_hop_id) {
    return api_->remove_next_hop(next_hop_id);
  }
  sai_status_t _getAttr(sai_attribute_t* attr, sai_object_id_t id) const {
    return api_->get_next_hop_attribute(id, 1, attr);
  }
  sai_status_t _setAttr(const sai_attribute_t* attr, sai_object_id_t id) {
    return api_->set_next_hop_attribute(id, attr);
  }
  sai_next_hop_api_t* api_;
  friend class SaiApi<NextHopApi, NextHopApiParameters>;
};

} // namespace fboss
} // namespace facebook
