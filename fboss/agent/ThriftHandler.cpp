/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/ThriftHandler.h"

#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include "common/logging/logging.h"
#include "common/stats/ServiceData.h"
#include "fboss/agent/AddressUtil.h"
#include "fboss/agent/ArpHandler.h"
#include "fboss/agent/state/AclMap.h"
#include "fboss/agent/IPv6Handler.h"
#include "fboss/agent/LinkAggregationManager.h"
#include "fboss/agent/LldpManager.h"
#include "fboss/agent/NeighborUpdater.h"
#include "fboss/agent/RouteUpdateLogger.h"
#include "fboss/agent/SwSwitch.h"
#include "fboss/agent/SwitchStats.h"
#include "fboss/agent/TxPacket.h"
#include "fboss/agent/Utils.h"
#include "fboss/agent/capture/PktCapture.h"
#include "fboss/agent/capture/PktCaptureManager.h"
#include "fboss/agent/hw/mock/MockRxPacket.h"
#include "fboss/agent/if/gen-cpp2/NeighborListenerClient.h"
#include "fboss/agent/state/AggregatePort.h"
#include "fboss/agent/state/AggregatePortMap.h"
#include "fboss/agent/state/ArpEntry.h"
#include "fboss/agent/state/ArpTable.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/state/InterfaceMap.h"
#include "fboss/agent/state/LabelForwardingEntry.h"
#include "fboss/agent/state/NdpEntry.h"
#include "fboss/agent/state/NdpTable.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/PortQueue.h"
#include "fboss/agent/state/Route.h"
#include "fboss/agent/state/RouteTable.h"
#include "fboss/agent/state/RouteTableRib.h"
#include "fboss/agent/state/RouteUpdater.h"
#include "fboss/agent/state/StateUtils.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/state/Vlan.h"
#include "fboss/agent/state/VlanMap.h"

#include <folly/MoveWrapper.h>
#include <folly/Range.h>
#include <folly/functional/Partial.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/json_pointer.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/async/DuplexChannel.h>

#include <limits>

using apache::thrift::ClientReceiveState;
using facebook::fb303::cpp2::fb_status;
using folly::fbstring;
using folly::IOBuf;
using std::make_unique;
using folly::StringPiece;
using folly::IPAddress;
using folly::IPAddressV4;
using folly::IPAddressV6;
using folly::MacAddress;
using folly::io::RWPrivateCursor;
using std::chrono::duration_cast;
using std::chrono::seconds;
using std::chrono::steady_clock;
using std::map;
using std::shared_ptr;
using std::string;
using std::vector;
using std::unique_ptr;
using apache::thrift::server::TConnectionContext;

using facebook::network::toBinaryAddress;
using facebook::network::toAddress;
using facebook::network::toIPAddress;

DEFINE_bool(
    enable_running_config_mutations,
    false,
    "Allow external mutations of running config");

namespace facebook { namespace fboss {

namespace util {

/**
 * Utility function to convert `Nexthops` (resolved ones) to list<BinaryAddress>
 */
std::vector<network::thrift::BinaryAddress>
fromFwdNextHops(RouteNextHopSet const& nexthops) {
  std::vector<network::thrift::BinaryAddress> nhs;
  nhs.reserve(nexthops.size());
  for (auto const& nexthop : nexthops) {
    auto addr = network::toBinaryAddress(nexthop.addr());
    addr.ifName_ref() = util::createTunIntfName(nexthop.intf());
    nhs.emplace_back(std::move(addr));
  }
  return nhs;
}

std::vector<NextHopThrift> thriftNextHopsFromAddresses(
    const std::vector<network::thrift::BinaryAddress>& addrs) {
  std::vector<NextHopThrift> nhs;
  nhs.reserve(addrs.size());
  for (const auto& addr : addrs) {

    NextHopThrift nh;
    nh.address = addr;
    nh.weight = 0;
    nhs.emplace_back(std::move(nh));
  }
  return nhs;
}
}

class RouteUpdateStats {
 public:
  RouteUpdateStats(SwSwitch *sw, const std::string& func, uint32_t routes)
      : sw_(sw),
        func_(func),
        routes_(routes),
        start_(std::chrono::steady_clock::now()) {
  }
  ~RouteUpdateStats() {
    auto end = std::chrono::steady_clock::now();
    auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
    sw_->stats()->routeUpdate(duration, routes_);
    XLOG(DBG0) << func_ << " " << routes_ << " routes took " << duration.count()
               << "us";
  }
 private:
  SwSwitch* sw_;
  const std::string func_;
  uint32_t routes_;
  std::chrono::time_point<std::chrono::steady_clock> start_;
};

ThriftHandler::ThriftHandler(SwSwitch* sw) : FacebookBase2("FBOSS"), sw_(sw) {
  sw->registerNeighborListener(
    [=](const std::vector<std::string>& added,
        const std::vector<std::string>& deleted) {
      for (auto& listener : listeners_.accessAllThreads()) {
        XLOG(INFO) << "Sending notification to bgpD";
        auto listenerPtr = &listener;
        listener.eventBase->runInEventBaseThread([=] {
          XLOG(INFO) << "firing off notification";
          invokeNeighborListeners(listenerPtr, added, deleted);
        });
      }
  });
}

fb_status ThriftHandler::getStatus() {
  if (sw_->isFullyInitialized()) {
    return fb_status::ALIVE;
  } else if (sw_->isExiting()) {
    return fb_status::STOPPING;
  } else {
    return fb_status::STARTING;
  }
}

void ThriftHandler::async_tm_getStatus(ThriftCallback<fb_status> callback) {
  callback->result(getStatus());
}

void ThriftHandler::flushCountersNow() {
  // Currently SwSwitch only contains thread local stats.
  //
  // Depending on how we design the HW-specific stats interface,
  // we may also need to make a separate call to force immediate collection of
  // hardware stats.
  stats::ThreadCachedServiceData::get()->publishStats();
}

void ThriftHandler::addUnicastRoute(
    int16_t client, std::unique_ptr<UnicastRoute> route) {
  auto routes = std::make_unique<std::vector<UnicastRoute>>();
  routes->emplace_back(std::move(*route));
  addUnicastRoutes(client, std::move(routes));
}

void ThriftHandler::deleteUnicastRoute(
    int16_t client, std::unique_ptr<IpPrefix> prefix) {
  auto prefixes = std::make_unique<std::vector<IpPrefix>>();
  prefixes->emplace_back(std::move(*prefix));
  deleteUnicastRoutes(client, std::move(prefixes));
}

void ThriftHandler::addUnicastRoutes(
    int16_t client, std::unique_ptr<std::vector<UnicastRoute>> routes) {
  ensureConfigured("addUnicastRoutes");
  ensureFibSynced("addUnicastRoutes");
  updateUnicastRoutesImpl(client, routes, "addUnicastRoutes", false);
}

void ThriftHandler::getProductInfo(ProductInfo& productInfo) {
  sw_->getProductInfo(productInfo);
}

void ThriftHandler::deleteUnicastRoutes(
    int16_t client,
    std::unique_ptr<std::vector<IpPrefix>> prefixes) {
  ensureConfigured("deleteUnicastRoutes");
  ensureFibSynced("deleteUnicastRoutes");

  if (sw_->isStandaloneRibEnabled()) {
    auto defaultVrf = RouterID(0);
    auto clientID = ClientID(client);
    auto defaultAdminDistance = sw_->clientIdToAdminDistance(client);

    auto stats = sw_->rib()->update(
        defaultVrf,
        clientID,
        defaultAdminDistance,
        {} /* routes to add */,
        *prefixes /* prefixes to delete */,
        false /* reset routes for client */,
        "delete unicast route",
        folly::partial(&SwSwitch::updateStateBlocking, sw_));

    sw_->stats()->delRoutesV4(stats.v4RoutesDeleted);
    sw_->stats()->delRoutesV6(stats.v6RoutesDeleted);

    auto totalRouteCount = stats.v4RoutesDeleted + stats.v6RoutesDeleted;
    sw_->stats()->routeUpdate(stats.duration, totalRouteCount);
    XLOG(DBG0) << "Delete " << totalRouteCount << " routes took "
               << stats.duration.count() << "us";

    return;
  }

  RouteUpdateStats stats(sw_, "Delete", prefixes->size());
  // Perform the update
  auto updateFn = [&](const shared_ptr<SwitchState>& state) {
    RouteUpdater updater(state->getRouteTables());
    RouterID routerId = RouterID(0); // TODO, default vrf for now
    for (const auto& prefix : *prefixes) {
      auto network = toIPAddress(prefix.ip);
      auto mask = static_cast<uint8_t>(prefix.prefixLength);
      if (network.isV4()) {
        sw_->stats()->delRouteV4();
      } else {
        sw_->stats()->delRouteV6();
      }
      updater.delRoute(routerId, network, mask, ClientID(client));
    }
    auto newRt = updater.updateDone();
    if (!newRt) {
      return shared_ptr<SwitchState>();
    }
    auto newState = state->clone();
    newState->resetRouteTables(std::move(newRt));
    return newState;
  };
  sw_->updateStateBlocking("delete unicast route", updateFn);
}

void ThriftHandler::syncFib(
    int16_t client, std::unique_ptr<std::vector<UnicastRoute>> routes) {
  ensureConfigured("syncFib");
  updateUnicastRoutesImpl(client, routes, "syncFib", true);
  if (!sw_->isFibSynced()) {
    sw_->fibSynced();
  }
}

void ThriftHandler::updateUnicastRoutesImpl(
  int16_t client, const std::unique_ptr<std::vector<UnicastRoute>>& routes,
  const std::string& updType, bool sync) {
  if (sw_->isStandaloneRibEnabled()) {
    auto defaultVrf = RouterID(0);
    auto clientID = ClientID(client);
    auto defaultAdminDistance = sw_->clientIdToAdminDistance(client);

    auto stats = sw_->rib()->update(
        defaultVrf,
        clientID,
        defaultAdminDistance,
        *routes /* routes to add */,
        {} /* prefixes to delete */,
        sync,
        updType,
        folly::partial(&SwSwitch::updateStateBlocking, sw_));

    sw_->stats()->addRoutesV4(stats.v4RoutesAdded);
    sw_->stats()->addRoutesV6(stats.v6RoutesAdded);

    auto totalRouteCount = stats.v4RoutesAdded + stats.v6RoutesAdded;
    sw_->stats()->routeUpdate(stats.duration, totalRouteCount);
    XLOG(DBG0) << updType << " " << totalRouteCount << " routes took "
               << stats.duration.count() << "us";

    return;
  }

  RouteUpdateStats stats(sw_, updType, routes->size());

  // Note that we capture routes by reference here, since it is a unique_ptr.
  // This is safe since we use updateStateBlocking(), so routes will still
  // be valid in our scope when updateFn() is called.
  // We could use folly::MoveWrapper if we did need to capture routes by value.
  auto updateFn = [&](const shared_ptr<SwitchState>& state) {
    // create an update object starting from empty
    RouteUpdater updater(state->getRouteTables());
    RouterID routerId = RouterID(0); // TODO, default vrf for now
    auto clientIdToAdmin = sw_->clientIdToAdminDistance(client);
    if (sync) {
      updater.removeAllRoutesForClient(routerId, ClientID(client));
    }
    for (const auto& route : *routes) {
      folly::IPAddress network = toIPAddress(route.dest.ip);
      uint8_t mask = static_cast<uint8_t>(route.dest.prefixLength);
      auto adminDistance = route.__isset.adminDistance
          ? route.adminDistance_ref().value_unchecked()
          : clientIdToAdmin;
      std::vector<NextHopThrift> nhts;
      if (route.nextHops.empty() && !route.nextHopAddrs.empty()) {
        nhts = util::thriftNextHopsFromAddresses(route.nextHopAddrs);
      } else {
        nhts = route.nextHops;
      }
      RouteNextHopSet nexthops = util::toRouteNextHopSet(nhts);
      if (nexthops.size()) {
        updater.addRoute(routerId, network, mask, ClientID(client),
                         RouteNextHopEntry(std::move(nexthops), adminDistance));
      } else {
        XLOG(DBG3) << "Blackhole route:" << network << "/"
                   << static_cast<int>(mask);
        updater.addRoute(routerId, network, mask, ClientID(client),
                         RouteNextHopEntry(RouteForwardAction::DROP,
                           adminDistance));
      }
      if (network.isV4()) {
        sw_->stats()->addRouteV4();
      } else {
        sw_->stats()->addRouteV6();
      }
    }
    auto newRt = updater.updateDone();
    if (!newRt) {
      return shared_ptr<SwitchState>();
    }
    auto newState = state->clone();
    newState->resetRouteTables(std::move(newRt));
    return newState;
  };
  sw_->updateStateBlocking(updType, updateFn);
}

static void populateInterfaceDetail(InterfaceDetail& interfaceDetail,
                                    const std::shared_ptr<Interface> intf) {
    interfaceDetail.interfaceName = intf->getName();
    interfaceDetail.interfaceId = intf->getID();
    interfaceDetail.vlanId = intf->getVlanID();
    interfaceDetail.routerId = intf->getRouterID();
    interfaceDetail.mtu = intf->getMtu();
    interfaceDetail.mac = intf->getMac().toString();
    interfaceDetail.address.clear();
    interfaceDetail.address.reserve(intf->getAddresses().size());
    for (const auto& addrAndMask: intf->getAddresses()) {
      IpPrefix temp;
      temp.ip = toBinaryAddress(addrAndMask.first);
      temp.prefixLength = addrAndMask.second;
      interfaceDetail.address.push_back(temp);
    }
}

void ThriftHandler::getAllInterfaces(
    std::map<int32_t, InterfaceDetail>& interfaces) {
  ensureConfigured();
  for (const auto& intf : (*sw_->getState()->getInterfaces())) {
    auto& interfaceDetail = interfaces[intf->getID()];
    populateInterfaceDetail(interfaceDetail, intf);
  }
}

void ThriftHandler::getInterfaceList(std::vector<std::string>& interfaceList) {
  ensureConfigured();
  for (const auto& intf : (*sw_->getState()->getInterfaces())) {
    interfaceList.push_back(intf->getName());
  }
}

void ThriftHandler::getInterfaceDetail(InterfaceDetail& interfaceDetail,
                                                        int32_t interfaceId) {
  ensureConfigured();
  const auto& intf = sw_->getState()->getInterfaces()->getInterfaceIf(
      InterfaceID(interfaceId));

  if (!intf) {
    throw FbossError("no such interface ", interfaceId);
  }
  populateInterfaceDetail(interfaceDetail, intf);
}

void ThriftHandler::getNdpTable(std::vector<NdpEntryThrift>& ndpTable) {
  ensureConfigured();
  sw_->getNeighborUpdater()->getNdpCacheData(ndpTable);
}

void ThriftHandler::getArpTable(std::vector<ArpEntryThrift>& arpTable) {
  ensureConfigured();
  sw_->getNeighborUpdater()->getArpCacheData(arpTable);
}

void ThriftHandler::getL2Table(std::vector<L2EntryThrift>& l2Table) {
  ensureConfigured();
  sw_->getHw()->fetchL2Table(&l2Table);
  XLOG(DBG6) << "L2 Table size:" << l2Table.size();
}

AclEntryThrift ThriftHandler::populateAclEntryThrift(
    const AclEntry& aclEntry) {
    AclEntryThrift aclEntryThrift;
    aclEntryThrift.priority = aclEntry.getPriority();
    aclEntryThrift.name = aclEntry.getID();
    aclEntryThrift.srcIp = toBinaryAddress(aclEntry.getSrcIp().first);
    aclEntryThrift.srcIpPrefixLength = aclEntry.getSrcIp().second;
    aclEntryThrift.dstIp = toBinaryAddress(aclEntry.getDstIp().first);
    aclEntryThrift.dstIpPrefixLength = aclEntry.getDstIp().second;
    aclEntryThrift.actionType =
        aclEntry.getActionType() == cfg::AclActionType::DENY ?
        "deny" : "permit";
    if (aclEntry.getProto()) {
      aclEntryThrift.proto_ref() = aclEntry.getProto().value();
    }
    if (aclEntry.getSrcPort()) {
      aclEntryThrift.srcPort_ref() = aclEntry.getSrcPort().value();
    }
    if (aclEntry.getDstPort()) {
      aclEntryThrift.dstPort_ref() = aclEntry.getDstPort().value();
    }
    if (aclEntry.getIcmpCode()) {
      aclEntryThrift.icmpCode_ref() = aclEntry.getIcmpCode().value();
    }
    if (aclEntry.getIcmpType()) {
      aclEntryThrift.icmpType_ref() = aclEntry.getIcmpType().value();
    }
    if (aclEntry.getDscp()) {
      aclEntryThrift.dscp_ref() = aclEntry.getDscp().value();
    }
    if (aclEntry.getTtl()) {
      aclEntryThrift.ttl_ref() = aclEntry.getTtl().value().getValue();
    }
    if (aclEntry.getL4SrcPort()) {
      aclEntryThrift.l4SrcPort_ref() = aclEntry.getL4SrcPort().value();
    }
    if (aclEntry.getL4DstPort()) {
      aclEntryThrift.l4DstPort_ref() = aclEntry.getL4DstPort().value();
    }
    if (aclEntry.getDstMac()) {
      aclEntryThrift.dstMac_ref() = aclEntry.getDstMac().value().toString();
    }
    return aclEntryThrift;
}

void ThriftHandler::getAclTable(std::vector<AclEntryThrift>& aclTable) {
  ensureConfigured();
  aclTable.reserve(sw_->getState()->getAcls()->numEntries());
  for (const auto& aclEntry : *(sw_->getState()->getAcls())) {
    aclTable.push_back(populateAclEntryThrift(*aclEntry));
  }
}

LacpPortRateThrift ThriftHandler::fromLacpPortRate(cfg::LacpPortRate rate) {
  switch (rate) {
    case cfg::LacpPortRate::SLOW:
      return LacpPortRateThrift::SLOW;
    case cfg::LacpPortRate::FAST:
      return LacpPortRateThrift::FAST;
  }
  throw FbossError("Unknown LACP port rate: ", rate);
}

LacpPortActivityThrift ThriftHandler::fromLacpPortActivity(
    cfg::LacpPortActivity activity) {
  switch (activity) {
    case cfg::LacpPortActivity::ACTIVE:
      return LacpPortActivityThrift::ACTIVE;
    case cfg::LacpPortActivity::PASSIVE:
      return LacpPortActivityThrift::PASSIVE;
  }
  throw FbossError("Unknown LACP port activity: ", activity);
}

void ThriftHandler::populateAggregatePortThrift(
    const std::shared_ptr<AggregatePort>& aggregatePort,
    AggregatePortThrift& aggregatePortThrift) {
  aggregatePortThrift.key = static_cast<uint32_t>(aggregatePort->getID());
  aggregatePortThrift.name = aggregatePort->getName();
  aggregatePortThrift.description = aggregatePort->getDescription();
  aggregatePortThrift.systemPriority = aggregatePort->getSystemPriority();
  aggregatePortThrift.systemID = aggregatePort->getSystemID().toString();
  aggregatePortThrift.minimumLinkCount = aggregatePort->getMinimumLinkCount();

  // Since aggregatePortThrift.memberPorts is being push_back'ed to, but is an
  // out parameter, make sure it's clear() first
  aggregatePortThrift.memberPorts.clear();

  aggregatePortThrift.memberPorts.reserve(aggregatePort->subportsCount());

  for (const auto& subport : aggregatePort->sortedSubports()) {
    bool isEnabled = aggregatePort->getForwardingState(subport.portID) ==
        AggregatePort::Forwarding::ENABLED;

    aggregatePortThrift.memberPorts.push_back(
        {apache::thrift::FragileConstructor::FRAGILE,
         static_cast<int32_t>(subport.portID),
         isEnabled,
         static_cast<int32_t>(subport.priority),
         fromLacpPortRate(subport.rate),
         fromLacpPortActivity(subport.activity)});
  }
}

void ThriftHandler::getAggregatePort(
    AggregatePortThrift& aggregatePortThrift,
    int32_t aggregatePortIDThrift) {
  ensureConfigured();

  if (aggregatePortIDThrift < 0 ||
      aggregatePortIDThrift > std::numeric_limits<uint16_t>::max()) {
    throw FbossError(
        "AggregatePort ID ", aggregatePortIDThrift, " is out of range");
  }
  auto aggregatePortID = static_cast<AggregatePortID>(aggregatePortIDThrift);

  auto aggregatePort =
      sw_->getState()->getAggregatePorts()->getAggregatePortIf(aggregatePortID);

  if (!aggregatePort) {
    throw FbossError(
        "AggregatePort with ID ", aggregatePortIDThrift, " not found");
  }

  populateAggregatePortThrift(aggregatePort, aggregatePortThrift);
}

void ThriftHandler::getAggregatePortTable(
    std::vector<AggregatePortThrift>& aggregatePortsThrift) {
  ensureConfigured();

  // Since aggregatePortsThrift is being push_back'ed to, but is an out
  // parameter, make sure it's clear() first
  aggregatePortsThrift.clear();

  aggregatePortsThrift.reserve(sw_->getState()->getAggregatePorts()->size());

  for (const auto& aggregatePort : *(sw_->getState()->getAggregatePorts())) {
    aggregatePortsThrift.emplace_back();

    populateAggregatePortThrift(aggregatePort, aggregatePortsThrift.back());
  }
}

void ThriftHandler::fillPortStats(PortInfoThrift& portInfo, int numPortQs) {
  auto portId = portInfo.portId;
  auto statMap = fbData->getStatMap();

  auto getSumStat = [&] (StringPiece prefix, StringPiece name) {
    auto portName =  portInfo.name.empty() ?
      folly::to<std::string>("port", portId) : portInfo.name;
    auto statName = folly::to<std::string>(portName, ".", prefix, name);
    auto statPtr = statMap->getLockedStatPtr(statName);
    auto numLevels = statPtr->numLevels();
    // Cumulative (ALLTIME) counters are at (numLevels - 1)
    return statPtr->sum(numLevels - 1);
  };

  auto fillPortCounters = [&] (PortCounters& ctr, StringPiece prefix) {
    ctr.bytes = getSumStat(prefix, "bytes");
    ctr.ucastPkts = getSumStat(prefix, "unicast_pkts");
    ctr.multicastPkts = getSumStat(prefix, "multicast_pkts");
    ctr.broadcastPkts = getSumStat(prefix, "broadcast_pkts");
    ctr.errors.errors = getSumStat(prefix, "errors");
    ctr.errors.discards = getSumStat(prefix, "discards");
  };

  fillPortCounters(portInfo.output, "out_");
  fillPortCounters(portInfo.input, "in_");
  for (int i=0; i<numPortQs; i++) {
    auto queue = folly::to<std::string>("queue", i, ".");
    QueueStats stats;
    stats.congestionDiscards =
        getSumStat(queue, "out_congestion_discards_bytes");
    stats.outBytes = getSumStat(queue, "out_bytes");
    portInfo.output.unicast.push_back(stats);
  }
}

void ThriftHandler::getPortInfoHelper(
    PortInfoThrift& portInfo,
    const std::shared_ptr<Port> port) {
  portInfo.portId = port->getID();
  portInfo.name = port->getName();
  portInfo.description = port->getDescription();
  portInfo.speedMbps = static_cast<int>(port->getSpeed());
  for (auto entry : port->getVlans()) {
    portInfo.vlans.push_back(entry.first);
  }

  for (const auto& queue : port->getPortQueues()) {
    PortQueueThrift pq;
    pq.id = queue->getID();
    pq.mode = cfg::_QueueScheduling_VALUES_TO_NAMES.find(
        queue->getScheduling())->second;
    if (queue->getScheduling() == cfg::QueueScheduling::WEIGHTED_ROUND_ROBIN) {
      pq.weight_ref() = queue->getWeight();
    }
    if (queue->getReservedBytes()) {
      pq.reservedBytes_ref() = queue->getReservedBytes().value();
    }
    if (queue->getScalingFactor()) {
      pq.scalingFactor_ref() = cfg::_MMUScalingFactor_VALUES_TO_NAMES
                                   .find(queue->getScalingFactor().value())
                                   ->second;
    }
    if (!queue->getAqms().empty()) {
      std::vector<ActiveQueueManagement> aqms;
      for (const auto& aqm: queue->getAqms()) {
        ActiveQueueManagement aqmThrift;
        switch (aqm.second.detection.getType()) {
          case cfg::QueueCongestionDetection::Type::linear:
            aqmThrift.detection.linear_ref().value_unchecked().minimumLength =
                aqm.second.detection.get_linear().minimumLength;
            aqmThrift.detection.linear_ref().value_unchecked().maximumLength =
                aqm.second.detection.get_linear().maximumLength;
            aqmThrift.detection.__isset.linear = true;
            break;
          case cfg::QueueCongestionDetection::Type::__EMPTY__:
            XLOG(WARNING) << "Invalid queue congestion detection config";
            break;
        }
        aqmThrift.behavior = QueueCongestionBehavior(aqm.first);
        aqms.push_back(aqmThrift);
      }
      pq.aqms_ref().value_unchecked().swap(aqms);
      pq.__isset.aqms = true;
    }
    if (queue->getName()) {
      pq.name = queue->getName().value();
    }
    portInfo.portQueues.push_back(pq);
  }

  portInfo.adminState =
      PortAdminState(port->getAdminState() == cfg::PortState::ENABLED);
  portInfo.operState =
      PortOperState(port->getOperState() == Port::OperState::UP);
  portInfo.fecEnabled = sw_->getHw()->getPortFECEnabled(port->getID());

  auto pause = port->getPause();
  portInfo.txPause = pause.tx;
  portInfo.rxPause = pause.rx;

  fillPortStats(portInfo, portInfo.portQueues.size());
}

void ThriftHandler::getPortInfo(PortInfoThrift &portInfo, int32_t portId) {
  ensureConfigured();

  const auto port =
      sw_->getState()->getPorts()->getPortIf(PortID(portId));
  if (!port) {
    throw FbossError("no such port ", portId);
  }

  getPortInfoHelper(portInfo, port);
}

void ThriftHandler::getAllPortInfo(map<int32_t, PortInfoThrift>& portInfoMap) {
  ensureConfigured();

  // NOTE: important to take pointer to switch state before iterating over
  // list of ports
  std::shared_ptr<SwitchState> swState = sw_->getState();
  for (const auto& port : *(swState->getPorts())) {
    auto portId = port->getID();
    auto& portInfo = portInfoMap[portId];
    getPortInfoHelper(portInfo, port);
  }
}

void ThriftHandler::clearPortStats(unique_ptr<vector<int32_t>> ports) {
  ensureConfigured();
  sw_->clearPortStats(ports);
}

void ThriftHandler::getPortStats(PortInfoThrift& portInfo, int32_t portId) {
  getPortInfo(portInfo, portId);
}

void ThriftHandler::getAllPortStats(map<int32_t, PortInfoThrift>& portInfoMap) {
  getAllPortInfo(portInfoMap);
}

void ThriftHandler::getRunningConfig(std::string& configStr) {
  ensureConfigured();
  configStr = sw_->getConfigStr();
}

void ThriftHandler::getCurrentStateJSON(
    std::string& ret,
    std::unique_ptr<std::string> jsonPointerStr) {
  if (!jsonPointerStr) {
    return;
  }
  ensureConfigured();
  auto const jsonPtr = folly::json_pointer::try_parse(*jsonPointerStr);
  if (!jsonPtr) {
    throw FbossError("Malformed JSON Pointer");
  }
  auto swState = sw_->getState()->toFollyDynamic();
  auto dyn = swState.get_ptr(jsonPtr.value());
  ret = folly::json::serialize(*dyn, folly::json::serialization_opts{});
}

void ThriftHandler::patchCurrentStateJSON(
    std::unique_ptr<std::string> jsonPointerStr,
    std::unique_ptr<std::string> jsonPatchStr) {
  if (!FLAGS_enable_running_config_mutations) {
    throw FbossError( "Running config mutations are not allowed");
  }
  ensureConfigured();
  auto const jsonPtr = folly::json_pointer::try_parse(*jsonPointerStr);
  if (!jsonPtr) {
    throw FbossError("Malformed JSON Pointer");
  }
  // OK to capture by reference because the update call below is blocking
  auto updateFn = [&](const shared_ptr<SwitchState>& oldState) {
    auto fullDynamic = oldState->toFollyDynamic();
    auto* partialDynamic = fullDynamic.get_ptr(jsonPtr.value());
    if (!partialDynamic) {
      throw FbossError("JSON Pointer does not address proper object");
    }
    // mutates in place, i.e. modifies fullDynamic too
    partialDynamic->merge_patch(folly::parseJson(*jsonPatchStr));
    return SwitchState::fromFollyDynamic(fullDynamic);
  };
  sw_->updateStateBlocking("JSON patch", std::move(updateFn));
}

void ThriftHandler::getPortStatus(map<int32_t, PortStatus>& statusMap,
                                  unique_ptr<vector<int32_t>> ports) {
  ensureConfigured();
  if (ports->empty()) {
    statusMap = sw_->getPortStatus();
  } else {
    for (auto port : *ports) {
      statusMap[port] = sw_->getPortStatus(PortID(port));
    }
  }
}

void ThriftHandler::setPortState(int32_t portNum, bool enable) {
  ensureConfigured();
  PortID portId = PortID(portNum);
  const auto port = sw_->getState()->getPorts()->getPortIf(portId);
  if (!port) {
    throw FbossError("no such port ", portNum);
  }

  cfg::PortState newPortState =
      enable ? cfg::PortState::ENABLED : cfg::PortState::DISABLED;

  if (port->getAdminState() == newPortState) {
    XLOG(DBG2) << "setPortState: port already in state "
               << (enable ? "ENABLED" : "DISABLED");
    return;
  }

  auto updateFn = [=](const shared_ptr<SwitchState>& state) {
    shared_ptr<SwitchState> newState{state};
    auto newPort = port->modify(&newState);
    newPort->setAdminState(newPortState);
    return newState;
  };
  sw_->updateStateBlocking("set port state", updateFn);
}

void ThriftHandler::getRouteTable(std::vector<UnicastRoute>& routes) {
  ensureConfigured();
  auto appliedState = sw_->getAppliedState();
  for (const auto& routeTable : (*appliedState->getRouteTables())) {
    for (const auto& ipv4 : *(routeTable->getRibV4()->routes())) {
      UnicastRoute tempRoute;
      if (!ipv4->isResolved()) {
        XLOG(INFO) << "Skipping unresolved route: " << ipv4->toFollyDynamic();
        continue;
      }
      auto fwdInfo = ipv4->getForwardInfo();
      tempRoute.dest.ip = toBinaryAddress(ipv4->prefix().network);
      tempRoute.dest.prefixLength = ipv4->prefix().mask;
      tempRoute.nextHopAddrs = util::fromFwdNextHops(fwdInfo.getNextHopSet());
      tempRoute.nextHops = util::fromRouteNextHopSet(fwdInfo.getNextHopSet());
      routes.emplace_back(std::move(tempRoute));
    }
    for (const auto& ipv6 : *(routeTable->getRibV6()->routes())) {
      UnicastRoute tempRoute;
      if (!ipv6->isResolved()) {
        XLOG(INFO) << "Skipping unresolved route: " << ipv6->toFollyDynamic();
        continue;
      }
      auto fwdInfo = ipv6->getForwardInfo();
      tempRoute.dest.ip = toBinaryAddress(ipv6->prefix().network);
      tempRoute.dest.prefixLength = ipv6->prefix().mask;
      tempRoute.nextHopAddrs = util::fromFwdNextHops(fwdInfo.getNextHopSet());
      tempRoute.nextHops = util::fromRouteNextHopSet(fwdInfo.getNextHopSet());
      routes.emplace_back(std::move(tempRoute));
    }
  }
}

void ThriftHandler::getRouteTableByClient(
    std::vector<UnicastRoute>& routes, int16_t client) {
  ensureConfigured();
  auto state = sw_->getState();
  for (const auto& routeTable : (*state->getRouteTables())) {
    for (const auto& ipv4 : *(routeTable->getRibV4()->routes())) {
      auto entry = ipv4->getEntryForClient(ClientID(client));
      if (not entry) {
        continue;
      }

      UnicastRoute tempRoute;
      tempRoute.dest.ip = toBinaryAddress(ipv4->prefix().network);
      tempRoute.dest.prefixLength = ipv4->prefix().mask;
      tempRoute.nextHops = util::fromRouteNextHopSet(entry->getNextHopSet());
      for (const auto& nh : tempRoute.nextHops) {
        tempRoute.nextHopAddrs.emplace_back(nh.address);
      }
      routes.emplace_back(std::move(tempRoute));
    }

    for (const auto& ipv6 : *(routeTable->getRibV6()->routes())) {
      auto entry = ipv6->getEntryForClient(ClientID(client));
      if (not entry) {
        continue;
      }

      UnicastRoute tempRoute;
      tempRoute.dest.ip = toBinaryAddress(ipv6->prefix().network);
      tempRoute.dest.prefixLength = ipv6->prefix().mask;
      tempRoute.nextHops = util::fromRouteNextHopSet(entry->getNextHopSet());
      for (const auto& nh : tempRoute.nextHops) {
        tempRoute.nextHopAddrs.emplace_back(nh.address);
      }
      routes.emplace_back(std::move(tempRoute));
    }
  }
}

void ThriftHandler::getRouteTableDetails(std::vector<RouteDetails>& routes) {
  ensureConfigured();
  auto state = sw_->getState();
  for (const auto& routeTable : *(state->getRouteTables())) {
    for (const auto& ipv4 : *(routeTable->getRibV4()->routes())) {
      RouteDetails rd = ipv4->toRouteDetails();
      routes.emplace_back(std::move(rd));
    }
    for (const auto& ipv6 : *(routeTable->getRibV6()->routes())) {
      RouteDetails rd = ipv6->toRouteDetails();
      routes.emplace_back(std::move(rd));
    }
  }
}

void ThriftHandler::getIpRoute(UnicastRoute& route,
                                std::unique_ptr<Address> addr, int32_t vrfId) {
  ensureConfigured();
  folly::IPAddress ipAddr = toIPAddress(*addr);
  auto routeTable = sw_->getState()->getRouteTables()->getRouteTableIf(
      RouterID(vrfId));
  if (!routeTable) {
    throw FbossError("No Such VRF ", vrfId);
  }

  if (ipAddr.isV4()) {
    auto ripV4Rib = routeTable->getRibV4();
    auto match = ripV4Rib->longestMatch(ipAddr.asV4());
    if (!match || !match->isResolved()) {
      route.dest.ip = toBinaryAddress(IPAddressV4("0.0.0.0"));
      route.dest.prefixLength = 0;
      return;
    }
    const auto fwdInfo = match->getForwardInfo();
    route.dest.ip = toBinaryAddress(match->prefix().network);
    route.dest.prefixLength = match->prefix().mask;
    route.nextHopAddrs = util::fromFwdNextHops(fwdInfo.getNextHopSet());
  } else {
    auto ripV6Rib = routeTable->getRibV6();
    auto match = ripV6Rib->longestMatch(ipAddr.asV6());
    if (!match || !match->isResolved()) {
      route.dest.ip = toBinaryAddress(IPAddressV6("::0"));
      route.dest.prefixLength = 0;
      return;
    }
    const auto fwdInfo = match->getForwardInfo();
    route.dest.ip = toBinaryAddress(match->prefix().network);
    route.dest.prefixLength = match->prefix().mask;
    route.nextHopAddrs = util::fromFwdNextHops(fwdInfo.getNextHopSet());
  }
}

void ThriftHandler::getIpRouteDetails(
  RouteDetails& route, std::unique_ptr<Address> addr, int32_t vrfId) {
  ensureConfigured();
  folly::IPAddress ipAddr = toIPAddress(*addr);
  auto routeTable = sw_->getState()->getRouteTables()->getRouteTableIf(
      RouterID(vrfId));
  if (!routeTable) {
    throw FbossError("No Such VRF ", vrfId);
  }

  if (ipAddr.isV4()) {
    auto ripV4Rib = routeTable->getRibV4();
    auto match = ripV4Rib->longestMatch(ipAddr.asV4());
    if (match && match->isResolved()) {
      route = match->toRouteDetails();
    }
  } else {
    auto ripV6Rib = routeTable->getRibV6();
    auto match = ripV6Rib->longestMatch(ipAddr.asV6());
    if (match && match->isResolved()) {
      route = match->toRouteDetails();
    }
  }
}

static LinkNeighborThrift thriftLinkNeighbor(const LinkNeighbor& n,
                                             steady_clock::time_point now) {
  LinkNeighborThrift tn;
  tn.localPort = n.getLocalPort();
  tn.localVlan = n.getLocalVlan();
  tn.srcMac = n.getMac().toString();
  tn.chassisIdType = static_cast<int32_t>(n.getChassisIdType());
  tn.chassisId = n.getChassisId();
  tn.printableChassisId = n.humanReadableChassisId();
  tn.portIdType = static_cast<int32_t>(n.getPortIdType());
  tn.portId = n.getPortId();
  tn.printablePortId = n.humanReadablePortId();
  tn.originalTTL = duration_cast<seconds>(n.getTTL()).count();
  tn.ttlSecondsLeft =
    duration_cast<seconds>(n.getExpirationTime() - now).count();
  if (!n.getSystemName().empty()) {
    tn.systemName_ref() = n.getSystemName();
  }
  if (!n.getSystemDescription().empty()) {
    tn.systemDescription_ref() = n.getSystemDescription();
  }
  if (!n.getPortDescription().empty()) {
    tn.portDescription_ref() = n.getPortDescription();
  }
  return tn;
}

void ThriftHandler::getLldpNeighbors(vector<LinkNeighborThrift>& results) {
  ensureConfigured();
  auto lldpMgr = sw_->getLldpMgr();
  if (lldpMgr == nullptr) {
    throw std::runtime_error("lldpMgr is not configured");
  }

  auto* db = lldpMgr->getDB();
  // Do an immediate check for expired neighbors
  db->pruneExpiredNeighbors();
  auto neighbors = db->getNeighbors();
  results.reserve(neighbors.size());
  auto now = steady_clock::now();
  for (const auto& entry : db->getNeighbors()) {
    results.push_back(thriftLinkNeighbor(entry, now));
  }
}

void ThriftHandler::invokeNeighborListeners(ThreadLocalListener* listener,
                                             std::vector<std::string> added,
                                             std::vector<std::string> removed) {
  // Collect the iterators to avoid erasing and potentially reordering
  // the iterators in the list.
  for (const auto& ctx : brokenClients_) {
    listener->clients.erase(ctx);
  }
  brokenClients_.clear();
  for (auto& client : listener->clients) {
    auto clientDone = [&](ClientReceiveState&& state) {
      try {
        NeighborListenerClientAsyncClient::recv_neighborsChanged(state);
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Exception in neighbor listener: " << ex.what();
        brokenClients_.push_back(client.first);
      }
    };
    client.second->neighborsChanged(clientDone, added, removed);
  }
}

void ThriftHandler::async_eb_registerForNeighborChanged(
    ThriftCallback<void> cb) {
  auto ctx = cb->getConnectionContext()->getConnectionContext();
  auto client = ctx->getDuplexClient<NeighborListenerClientAsyncClient>();
  auto info = listeners_.get();
  CHECK(cb->getEventBase()->isInEventBaseThread());
  if (!info) {
    info = new ThreadLocalListener(cb->getEventBase());
    listeners_.reset(info);
  }
  DCHECK_EQ(info->eventBase, cb->getEventBase());
  if (!info->eventBase) {
    info->eventBase = cb->getEventBase();
  }
  info->clients.emplace(ctx, client);
  cb->done();
}

void ThriftHandler::startPktCapture(unique_ptr<CaptureInfo> info) {
  ensureConfigured();
  auto* mgr = sw_->getCaptureMgr();
  auto capture = make_unique<PktCapture>(
       info->name, info->maxPackets, info->direction, info->filter);
  mgr->startCapture(std::move(capture));
}

void ThriftHandler::stopPktCapture(unique_ptr<std::string> name) {
  ensureConfigured();
  auto* mgr = sw_->getCaptureMgr();
  mgr->forgetCapture(*name);
}

void ThriftHandler::stopAllPktCaptures() {
  ensureConfigured();
  auto* mgr = sw_->getCaptureMgr();
  mgr->forgetAllCaptures();
}

void ThriftHandler::startLoggingRouteUpdates(
    std::unique_ptr<RouteUpdateLoggingInfo> info) {
  auto* routeUpdateLogger = sw_->getRouteUpdateLogger();
  folly::IPAddress addr = toIPAddress(info->prefix.ip);
  uint8_t mask = static_cast<uint8_t>(info->prefix.prefixLength);
  RouteUpdateLoggingInstance loggingInstance{
      RoutePrefix<folly::IPAddress>{addr, mask}, info->identifier, info->exact};
  routeUpdateLogger->startLoggingForPrefix(loggingInstance);
}

void ThriftHandler::stopLoggingRouteUpdates(
    std::unique_ptr<IpPrefix> prefix,
    std::unique_ptr<std::string> identifier) {
  auto* routeUpdateLogger = sw_->getRouteUpdateLogger();
  folly::IPAddress addr = toIPAddress(prefix->ip);
  uint8_t mask = static_cast<uint8_t>(prefix->prefixLength);
  routeUpdateLogger->stopLoggingForPrefix(addr, mask, *identifier);
}

void ThriftHandler::stopLoggingAnyRouteUpdates(
    std::unique_ptr<std::string> identifier) {
  auto* routeUpdateLogger = sw_->getRouteUpdateLogger();
  routeUpdateLogger->stopLoggingForIdentifier(*identifier);
}

void ThriftHandler::getRouteUpdateLoggingTrackedPrefixes(
    std::vector<RouteUpdateLoggingInfo>& infos) {
  auto* routeUpdateLogger = sw_->getRouteUpdateLogger();
  for (const auto& tracked : routeUpdateLogger->getTrackedPrefixes()) {
    RouteUpdateLoggingInfo info;
    IpPrefix prefix;
    prefix.ip = toBinaryAddress(tracked.prefix.network);
    prefix.prefixLength = tracked.prefix.mask;
    info.prefix = prefix;
    info.identifier = tracked.identifier;
    info.exact = tracked.exact;
    infos.push_back(info);
  }
}

void ThriftHandler::beginPacketDump(int32_t port) {
  // Client construction is serialized via SwSwitch event base
  sw_->constructPushClient(port);
}

void ThriftHandler::killDistributionProcess(){
  sw_->killDistributionProcess();
}

void ThriftHandler::sendPkt(int32_t port, int32_t vlan,
                            unique_ptr<fbstring> data) {
  ensureConfigured("sendPkt");
  auto buf = IOBuf::copyBuffer(reinterpret_cast<const uint8_t*>(data->data()),
                               data->size());
  auto pkt = make_unique<MockRxPacket>(std::move(buf));
  pkt->setSrcPort(PortID(port));
  pkt->setSrcVlan(VlanID(vlan));
  sw_->packetReceived(std::move(pkt));
}

void ThriftHandler::sendPktHex(int32_t port, int32_t vlan,
                               unique_ptr<fbstring> hex) {
  ensureConfigured("sendPktHex");
  auto pkt = MockRxPacket::fromHex(StringPiece(*hex));
  pkt->setSrcPort(PortID(port));
  pkt->setSrcVlan(VlanID(vlan));
  sw_->packetReceived(std::move(pkt));
}

void ThriftHandler::txPkt(int32_t port, unique_ptr<fbstring> data) {
  ensureConfigured("txPkt");

  unique_ptr<TxPacket> pkt = sw_->allocatePacket(data->size());
  RWPrivateCursor cursor(pkt->buf());
  cursor.push(StringPiece(*data));

  sw_->sendPacketOutOfPortAsync(std::move(pkt), PortID(port));
}

void ThriftHandler::txPktL2(unique_ptr<fbstring> data) {
  ensureConfigured("txPktL2");

  unique_ptr<TxPacket> pkt = sw_->allocatePacket(data->size());
  RWPrivateCursor cursor(pkt->buf());
  cursor.push(StringPiece(*data));

  sw_->sendPacketSwitchedAsync(std::move(pkt));
}

void ThriftHandler::txPktL3(unique_ptr<fbstring> payload) {
  ensureConfigured("txPktL3");

  unique_ptr<TxPacket> pkt = sw_->allocateL3TxPacket(payload->size());
  RWPrivateCursor cursor(pkt->buf());
  cursor.push(StringPiece(*payload));

  sw_->sendL3Packet(std::move(pkt));
}

Vlan* ThriftHandler::getVlan(int32_t vlanId) {
  ensureConfigured();
  return sw_->getState()->getVlans()->getVlan(VlanID(vlanId)).get();
}

Vlan* ThriftHandler::getVlan(const std::string& vlanName) {
  ensureConfigured();
  return sw_->getState()->getVlans()->getVlanSlow(vlanName).get();
}

int32_t ThriftHandler::flushNeighborEntry(unique_ptr<BinaryAddress> ip,
                                          int32_t vlan) {
  ensureConfigured("flushNeighborEntry");

  auto parsedIP = toIPAddress(*ip);
  VlanID vlanID(vlan);
  return sw_->getNeighborUpdater()->flushEntry(vlanID, parsedIP);
}

void ThriftHandler::getVlanAddresses(Addresses& addrs, int32_t vlan) {
  getVlanAddresses(getVlan(vlan), addrs, toAddress);
}

void ThriftHandler::getVlanAddressesByName(Addresses& addrs,
    unique_ptr<string> vlan) {
  getVlanAddresses(getVlan(*vlan), addrs, toAddress);
}

void ThriftHandler::getVlanBinaryAddresses(BinaryAddresses& addrs,
    int32_t vlan) {
  getVlanAddresses(getVlan(vlan), addrs, toBinaryAddress);
}

void ThriftHandler::getVlanBinaryAddressesByName(BinaryAddresses& addrs,
    const std::unique_ptr<std::string> vlan) {
  getVlanAddresses(getVlan(*vlan), addrs, toBinaryAddress);
}

template<typename ADDR_TYPE, typename ADDR_CONVERTER>
void ThriftHandler::getVlanAddresses(const Vlan* vlan,
    std::vector<ADDR_TYPE>& addrs, ADDR_CONVERTER& converter) {
  ensureConfigured();
  CHECK(vlan);
  for (auto intf : (*sw_->getState()->getInterfaces())) {
    if (intf->getVlanID() == vlan->getID()) {
      for (const auto& addrAndMask : intf->getAddresses()) {
          addrs.push_back(converter(addrAndMask.first));
      }
    }
  }
}

BootType ThriftHandler::getBootType() {
  return sw_->getBootType();
}

void ThriftHandler::ensureConfigured(StringPiece function) {
  if (sw_->isFullyConfigured()) {
    return;
  }

  if (!function.empty()) {
    XLOG(DBG1) << "failing thrift prior to switch configuration: " << function;
  }
  throw FbossError("switch is still initializing or is exiting and is not "
                   "fully configured yet");
}

void ThriftHandler::ensureFibSynced(StringPiece function) {
  if (sw_->isFibSynced()) {
    return;
  }

  if (!function.empty()) {
    XLOG_EVERY_MS(DBG1, 1000) << "failing thrift prior to FIB Sync: " << function;
  }
  throw FbossError("switch is still initializing, FIB not synced yet");
}

// If this is a premature client disconnect from a duplex connection, we need to
// clean up state.  Failure to do so may allow the server's duplex clients to
// use the destroyed context => segfaults.
void ThriftHandler::connectionDestroyed(TConnectionContext* ctx) {
  // Port status notifications
  if (listeners_) {
    listeners_->clients.erase(ctx);
  }
}

int32_t ThriftHandler::getIdleTimeout() {
  if (thriftIdleTimeout_ < 0) {
    throw FbossError("Idle timeout has not been set");
  }
  return thriftIdleTimeout_;
}

void ThriftHandler::reloadConfig() {
  ensureConfigured();
  return sw_->applyConfig("reload config initiated by thrift call", true);
}

void ThriftHandler::getLacpPartnerPair(
    LacpPartnerPair& lacpPartnerPair,
    int32_t portID) {
  ensureConfigured();

  auto lagManager = sw_->getLagManager();
  if (!lagManager) {
    throw FbossError("LACP not enabled");
  }

  lagManager->populatePartnerPair(static_cast<PortID>(portID), lacpPartnerPair);
}

void ThriftHandler::getAllLacpPartnerPairs(
    std::vector<LacpPartnerPair>& lacpPartnerPairs) {
  ensureConfigured();

  auto lagManager = sw_->getLagManager();
  if (!lagManager) {
    throw FbossError("LACP not enabled");
  }

  lagManager->populatePartnerPairs(lacpPartnerPairs);
}

SwitchRunState ThriftHandler::getSwitchRunState() {
  return sw_->getSwitchRunState();
}

SSLType ThriftHandler::getSSLPolicy() {
  SSLType sslType = SSLType::PERMITTED;

  if (sslPolicy_ == apache::thrift::SSLPolicy::DISABLED) {
    sslType = SSLType::DISABLED;
  } else if (sslPolicy_ == apache::thrift::SSLPolicy::PERMITTED) {
    sslType = SSLType::PERMITTED;
  } else if (sslPolicy_ == apache::thrift::SSLPolicy::REQUIRED) {
    sslType = SSLType::REQUIRED;
  } else {
    throw FbossError("Invalid SSL Policy");
  }

  return sslType;
}

void ThriftHandler::addMplsRoutes(
    int16_t clientId,
    std::unique_ptr<std::vector<MplsRoute>> mplsRoutes) {
  ensureConfigured();
  auto updateFn = [=, routes = std::move(*mplsRoutes)](
                      const std::shared_ptr<SwitchState>& state) {
    auto newState = state->clone();

    addMplsRoutesImpl(&newState, ClientID(clientId), routes);
    if (!sw_->isValidStateUpdate(StateDelta(state, newState))) {
      throw FbossError("Invalid MPLS routes");
    }
    return newState;
  };
  sw_->updateStateBlocking("addMplsRoutes", updateFn);
}

void ThriftHandler::addMplsRoutesImpl(
    std::shared_ptr<SwitchState>* state,
    ClientID clientId,
    const std::vector<MplsRoute>& mplsRoutes) const {
  auto labelFib =
      (*state)->getLabelForwardingInformationBase().get()->modify(state);
  for (const auto& mplsRoute : mplsRoutes) {
    auto topLabel = mplsRoute.topLabel;
    if (topLabel > mpls_constants::MAX_MPLS_LABEL_) {
      throw FbossError("invalid value for label ", topLabel);
    }
    auto adminDistance = mplsRoute.adminDistance_ref().has_value()
        ? mplsRoute.adminDistance_ref().value()
        : sw_->clientIdToAdminDistance(ClientID(clientId));
    LabelNextHopSet nexthops = util::toRouteNextHopSet(mplsRoute.nextHops);
    // validate top label
    labelFib = labelFib->programLabel(
        state,
        topLabel,
        ClientID(clientId),
        adminDistance,
        std::move(nexthops));
  }
}

void ThriftHandler::deleteMplsRoutes(
    int16_t clientId,
    std::unique_ptr<std::vector<int32_t>> topLabels) {
  ensureConfigured();
  auto updateFn = [=, topLabels = std::move(*topLabels)](
                      const std::shared_ptr<SwitchState>& state) {
    auto newState = state->clone();
    auto labelFib = state->getLabelForwardingInformationBase().get();
    for (const auto topLabel : topLabels) {
      if (topLabel > mpls_constants::MAX_MPLS_LABEL_) {
        throw FbossError("invalid value for label ", topLabel);
      }
      labelFib =
          labelFib->unprogramLabel(&newState, topLabel, ClientID(clientId));
    }
    return newState;
  };
  sw_->updateStateBlocking("deleteMplsRoutes", updateFn);
}

void ThriftHandler::syncMplsFib(
    int16_t clientId,
    std::unique_ptr<std::vector<MplsRoute>> mplsRoutes) {
  ensureConfigured();
  auto updateFn = [=, routes = std::move(*mplsRoutes)](
                      const std::shared_ptr<SwitchState>& state) {
    auto newState = state->clone();
    auto labelFib = newState->getLabelForwardingInformationBase();

    labelFib->purgeEntriesForClient(&newState, ClientID(clientId));
    addMplsRoutesImpl(&newState, ClientID(clientId), routes);
    if (!sw_->isValidStateUpdate(StateDelta(state, newState))) {
      throw FbossError("Invalid MPLS routes");
    }
    return newState;
  };
  sw_->updateStateBlocking("syncMplsFib", updateFn);
}

void ThriftHandler::getMplsRouteTableByClient(
    std::vector<MplsRoute>& mplsRoutes,
    int16_t clientId) {
  auto labelFib = sw_->getState()->getLabelForwardingInformationBase();
  for (const auto& entry : *labelFib) {
    auto* labelNextHopEntry = entry->getEntryForClient(ClientID(clientId));
    if (!labelNextHopEntry) {
      continue;
    }
    MplsRoute mplsRoute;
    mplsRoute.topLabel = entry->getID();
    mplsRoute.adminDistance_ref() = labelNextHopEntry->getAdminDistance();
    mplsRoute.nextHops =
        util::fromRouteNextHopSet(labelNextHopEntry->getNextHopSet());
    mplsRoutes.emplace_back(std::move(mplsRoute));
  }
}

void ThriftHandler::getAllMplsRouteDetails(
    std::vector<MplsRouteDetails>& mplsRouteDetails) {
  const auto labelFib = sw_->getState()->getLabelForwardingInformationBase();
  for (const auto& entry : *labelFib) {
    MplsRouteDetails details;
    getMplsRouteDetails(details, entry->getID());
    mplsRouteDetails.push_back(details);
  }
}

void ThriftHandler::getMplsRouteDetails(
    MplsRouteDetails& mplsRouteDetail,
    MplsLabel topLabel) {
  const auto entry = sw_->getState()
                         ->getLabelForwardingInformationBase()
                         ->getLabelForwardingEntry(topLabel);
  mplsRouteDetail.topLabel = entry->getID();
  mplsRouteDetail.nextHopMulti = entry->getLabelNextHopsByClient().toThrift();
  const auto& fwd = entry->getLabelNextHop();
  for (const auto& nh : fwd.getNextHopSet()) {
    mplsRouteDetail.nextHops.push_back(nh.toThrift());
  }
  mplsRouteDetail.adminDistance = fwd.getAdminDistance();
  mplsRouteDetail.action = forwardActionStr(fwd.getAction());
}
}} // facebook::fboss
