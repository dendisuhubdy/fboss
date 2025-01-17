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

#include "fboss/agent/hw/sai/api/AddressUtil.h"
#include "fboss/agent/hw/sai/api/SaiAttribute.h"
#include "fboss/agent/hw/sai/api/SaiAttributeDataTypes.h"

#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <folly/logging/xlog.h>

#include <iterator>
#include <vector>

extern "C" {
#include <sai.h>
}

namespace facebook {
namespace fboss {

struct FdbApiParameters {
  struct Attributes {
    using EnumType = sai_fdb_entry_attr_t;
    using Type = SaiAttribute<EnumType, SAI_FDB_ENTRY_ATTR_TYPE, sai_int32_t>;
    using BridgePortId =
        SaiAttribute<EnumType, SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID, SaiObjectIdT>;

    using CreateAttributes = SaiAttributeTuple<Type, BridgePortId>;
    /* implicit */ Attributes(const CreateAttributes& create) {
      std::tie(type, bridgePortId) = create.value();
    }
    CreateAttributes attrs() const {
      return {type, bridgePortId};
    }
    bool operator==(const Attributes& other) const {
      return attrs() == other.attrs();
    }
    bool operator!=(const Attributes& other) const {
      return !(*this == other);
    }
    Type::ValueType type;
    BridgePortId::ValueType bridgePortId;
  };

  class FdbEntry {
   public:
    FdbEntry(
        sai_object_id_t switchId,
        sai_object_id_t bridgeId,
        const folly::MacAddress& mac) {
      fdb_entry.switch_id = switchId;
      fdb_entry.bv_id = bridgeId;
      toSaiMacAddress(mac, fdb_entry.mac_address);
    }

    folly::MacAddress mac() const {
      return fromSaiMacAddress(fdb_entry.mac_address);
    }
    sai_object_id_t switchId() const {
      return fdb_entry.switch_id;
    }
    sai_object_id_t bridgeId() const {
      return fdb_entry.bv_id;
    }
    const sai_fdb_entry_t* entry() const {
      return &fdb_entry;
    }
    bool operator==(const FdbEntry& other) const {
      return (
          switchId() == other.switchId() && bridgeId() == other.bridgeId() &&
          mac() == other.mac());
    }

   private:
    sai_fdb_entry_t fdb_entry;
  };
  using EntryType = FdbEntry;
};

class FdbApi : public SaiApi<FdbApi, FdbApiParameters> {
 public:
  FdbApi() {
    sai_status_t status =
        sai_api_query(SAI_API_FDB, reinterpret_cast<void**>(&api_));
    saiCheckError(status, "Failed to query for fdb api");
  }

 private:
  sai_status_t _create(
      const FdbApiParameters::FdbEntry& fdbEntry,
      sai_attribute_t* attr_list,
      size_t count) {
    return api_->create_fdb_entry(fdbEntry.entry(), count, attr_list);
  }
  sai_status_t _remove(const FdbApiParameters::FdbEntry& fdbEntry) {
    return api_->remove_fdb_entry(fdbEntry.entry());
  }
  sai_status_t _getAttr(
      sai_attribute_t* attr,
      const FdbApiParameters::FdbEntry& fdbEntry) const {
    return api_->get_fdb_entry_attribute(fdbEntry.entry(), 1, attr);
  }
  sai_status_t _setAttr(
      const sai_attribute_t* attr,
      const FdbApiParameters::FdbEntry& fdbEntry) {
    return api_->set_fdb_entry_attribute(fdbEntry.entry(), attr);
  }
  sai_fdb_api_t* api_;
  friend class SaiApi<FdbApi, FdbApiParameters>;
};

} // namespace fboss
} // namespace facebook

namespace std {
template <>
struct hash<facebook::fboss::FdbApiParameters::FdbEntry> {
  size_t operator()(
      const facebook::fboss::FdbApiParameters::FdbEntry& FdbEntry) const;
};
} // namespace std
