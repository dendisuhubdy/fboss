/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "FakeSaiSwitch.h"
#include "fboss/agent/hw/sai/fake/FakeSai.h"

#include <folly/logging/xlog.h>

using facebook::fboss::FakeSai;

namespace {
static constexpr uint64_t kDefaultVlanId = 0;
static constexpr uint64_t kDefaultVirtualRouterId = 0;
static constexpr uint64_t kCpuPort = 0;
} // namespace

sai_status_t create_switch_fn(
    sai_object_id_t* switch_id,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  auto fs = FakeSai::getInstance();
  *switch_id = fs->swm.create();
  for (int i = 0; i < attr_count; ++i) {
    set_switch_attribute_fn(*switch_id, &attr_list[i]);
  }
  return SAI_STATUS_SUCCESS;
}

sai_status_t remove_switch_fn(sai_object_id_t switch_id) {
  auto fs = FakeSai::getInstance();
  fs->swm.remove(switch_id);
  return SAI_STATUS_SUCCESS;
}

sai_status_t set_switch_attribute_fn(
    sai_object_id_t switch_id,
    const sai_attribute_t* attr) {
  auto fs = FakeSai::getInstance();
  auto& sw = fs->swm.get(switch_id);
  sai_status_t res;
  if (!attr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  switch (attr->id) {
    case SAI_SWITCH_ATTR_SRC_MAC_ADDRESS:
      sw.setSrcMac(attr->value.mac);
      res = SAI_STATUS_SUCCESS;
      break;
    case SAI_SWITCH_ATTR_INIT_SWITCH:
      sw.setInitStatus(attr->value.booldata);
      res = SAI_STATUS_SUCCESS;
      break;
    case SAI_SWITCH_ATTR_PORT_NUMBER:
      // Number of active ports is read only
      res = SAI_STATUS_INVALID_PARAMETER;
      break;
    default:
      res = SAI_STATUS_INVALID_PARAMETER;
      break;
  }
  return res;
}

sai_status_t get_switch_attribute_fn(
    sai_object_id_t switch_id,
    uint32_t attr_count,
    sai_attribute_t* attr) {
  auto fs = FakeSai::getInstance();
  const auto& sw = fs->swm.get(switch_id);
  for (int i = 0; i < attr_count; ++i) {
    switch (attr[i].id) {
      case SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID:
        attr[i].value.oid = kDefaultVirtualRouterId;
        break;
      case SAI_SWITCH_ATTR_DEFAULT_VLAN_ID:
        attr[i].value.oid = kDefaultVlanId;
        break;
      case SAI_SWITCH_ATTR_CPU_PORT:
        attr[i].value.oid = kCpuPort;
        break;
      case SAI_SWITCH_ATTR_PORT_NUMBER:
        attr[i].value.u32 = fs->pm.map().size();
        break;
      case SAI_SWITCH_ATTR_PORT_LIST: {
        attr[i].value.objlist.count = fs->pm.map().size();
        int j = 0;
        for (const auto& p : fs->pm.map()) {
          attr[i].value.objlist.list[j++] = p.first;
        }
      } break;
      case SAI_SWITCH_ATTR_SRC_MAC_ADDRESS:
        std::copy_n(sw.srcMac().bytes(), 6, std::begin(attr[i].value.mac));
        break;
      case SAI_SWITCH_ATTR_INIT_SWITCH:
        attr[i].value.booldata = sw.isInitialized();
        break;
      default:
        return SAI_STATUS_INVALID_PARAMETER;
    }
  }
  return SAI_STATUS_SUCCESS;
}

namespace facebook {
namespace fboss {

static sai_switch_api_t _switch_api;

void populate_switch_api(sai_switch_api_t** switch_api) {
  _switch_api.create_switch = &create_switch_fn;
  _switch_api.remove_switch = &remove_switch_fn;
  _switch_api.set_switch_attribute = &set_switch_attribute_fn;
  _switch_api.get_switch_attribute = &get_switch_attribute_fn;
  *switch_api = &_switch_api;
}

} // namespace fboss
} // namespace facebook
