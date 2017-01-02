//
// Copyright (C) 2004 Andras Varga
// Copyright (C) 2014 OpenSim Ltd.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include <stdlib.h>
#include <string.h>

#include "inet/networklayer/ipv4/IPv4.h"

#include "inet/applications/common/SocketTag_m.h"
#include "inet/common/INETUtils.h"
#include "inet/common/IProtocolRegistrationListener.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/common/lifecycle/NodeOperations.h"
#include "inet/common/lifecycle/NodeStatus.h"
#include "inet/networklayer/arp/ipv4/ARPPacket_m.h"
#include "inet/networklayer/common/FragmentationTag_m.h"
#include "inet/networklayer/common/DscpTag_m.h"
#include "inet/networklayer/common/EcnTag_m.h"
#include "inet/networklayer/common/HopLimitTag_m.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/networklayer/common/MulticastTag_m.h"
#include "inet/networklayer/common/OrigNetworkDatagramTag.h"
#include "inet/networklayer/contract/IARP.h"
#include "inet/networklayer/contract/IInterfaceTable.h"
#include "inet/networklayer/contract/L3SocketCommand_m.h"
#include "inet/networklayer/ipv4/ICMPMessage_m.h"
#include "inet/networklayer/ipv4/IIPv4RoutingTable.h"
#include "inet/networklayer/ipv4/IPv4Header.h"
#include "inet/networklayer/ipv4/IPv4InterfaceData.h"
#include "inet/linklayer/common/EtherTypeTag_m.h"
#include "inet/linklayer/common/Ieee802Ctrl.h"
#include "inet/linklayer/common/InterfaceTag_m.h"
#include "inet/linklayer/common/MACAddressTag_m.h"

namespace inet {

Define_Module(IPv4);

//TODO TRANSLATE
// a multicast cimek eseten hianyoznak bizonyos NetFilter hook-ok
// a local interface-k hasznalata eseten szinten hianyozhatnak bizonyos NetFilter hook-ok

IPv4::IPv4() :
    isUp(true)
{
}

IPv4::~IPv4()
{
    flush();
}

void IPv4::initialize(int stage)
{
    if (stage == INITSTAGE_LOCAL) {
        QueueBase::initialize();

        ift = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);
        rt = getModuleFromPar<IIPv4RoutingTable>(par("routingTableModule"), this);
        arp = getModuleFromPar<IARP>(par("arpModule"), this);
        icmp = getModuleFromPar<ICMP>(par("icmpModule"), this);

        transportInGateBaseId = gateBaseId("transportIn");

        defaultTimeToLive = par("timeToLive");
        defaultMCTimeToLive = par("multicastTimeToLive");
        fragmentTimeoutTime = par("fragmentTimeout");
        forceBroadcast = par("forceBroadcast");
        useProxyARP = par("useProxyARP");

        curFragmentId = 0;
        lastCheckTime = 0;

        numMulticast = numLocalDeliver = numDropped = numUnroutable = numForwarded = 0;

        // NetFilter:
        hooks.clear();
        queuedDatagramsForHooks.clear();

        pendingPackets.clear();
        cModule *arpModule = check_and_cast<cModule *>(arp);
        arpModule->subscribe(IARP::completedARPResolutionSignal, this);
        arpModule->subscribe(IARP::failedARPResolutionSignal, this);

        WATCH(numMulticast);
        WATCH(numLocalDeliver);
        WATCH(numDropped);
        WATCH(numUnroutable);
        WATCH(numForwarded);
        WATCH_MAP(pendingPackets);
    }
    else if (stage == INITSTAGE_NETWORK_LAYER) {
        isUp = isNodeUp();
        registerProtocol(Protocol::ipv4, gate("transportOut"));
        registerProtocol(Protocol::ipv4, gate("queueOut"));
    }
}

void IPv4::handleRegisterProtocol(const Protocol& protocol, cGate *gate)
{
    Enter_Method("handleRegisterProtocol");
    mapping.addProtocolMapping(ProtocolGroup::ipprotocol.getProtocolNumber(&protocol), gate->getIndex());
}

void IPv4::refreshDisplay() const
{
    char buf[80] = "";
    if (numForwarded > 0)
        sprintf(buf + strlen(buf), "fwd:%d ", numForwarded);
    if (numLocalDeliver > 0)
        sprintf(buf + strlen(buf), "up:%d ", numLocalDeliver);
    if (numMulticast > 0)
        sprintf(buf + strlen(buf), "mcast:%d ", numMulticast);
    if (numDropped > 0)
        sprintf(buf + strlen(buf), "DROP:%d ", numDropped);
    if (numUnroutable > 0)
        sprintf(buf + strlen(buf), "UNROUTABLE:%d ", numUnroutable);
    getDisplayString().setTagArg("t", 0, buf);
}

void IPv4::handleMessage(cMessage *msg)
{
    if (L3SocketBindCommand *command = dynamic_cast<L3SocketBindCommand *>(msg->getControlInfo())) {
        int socketId = msg->getMandatoryTag<SocketReq>()->getSocketId();
        SocketDescriptor *descriptor = new SocketDescriptor(socketId, command->getProtocolId());
        socketIdToSocketDescriptor[socketId] = descriptor;
        protocolIdToSocketDescriptors.insert(std::pair<int, SocketDescriptor *>(command->getProtocolId(), descriptor));
        delete msg;
    }
    else if (dynamic_cast<L3SocketCloseCommand *>(msg->getControlInfo()) != nullptr) {
        int socketId = msg->getMandatoryTag<SocketReq>()->getSocketId();
        auto it = socketIdToSocketDescriptor.find(socketId);
        if (it != socketIdToSocketDescriptor.end()) {
            int protocol = it->second->protocolId;
            auto lowerBound = protocolIdToSocketDescriptors.lower_bound(protocol);
            auto upperBound = protocolIdToSocketDescriptors.upper_bound(protocol);
            for (auto jt = lowerBound; jt != upperBound; jt++) {
                if (it->second == jt->second) {
                    protocolIdToSocketDescriptors.erase(jt);
                    break;
                }
            }
            delete it->second;
            socketIdToSocketDescriptor.erase(it);
        }
        delete msg;
    }
    else
        QueueBase::handleMessage(msg);
}

void IPv4::endService(cPacket *packet)
{
    if (!isUp) {
        EV_ERROR << "IPv4 is down -- discarding message\n";
        delete packet;
        return;
    }
    if (packet->getArrivalGate()->isName("transportIn")) {    //TODO packet->getArrivalGate()->getBaseId() == transportInGateBaseId
        handlePacketFromHL(check_and_cast<Packet*>(packet));
    }
    else {    // from network
        EV_INFO << "Received " << packet << " from network.\n";
        const InterfaceEntry *fromIE = getSourceInterfaceFrom(packet);
        if (auto pk = dynamic_cast<Packet *>(packet))
            handleIncomingDatagram(pk, fromIE);
        else
            throw cRuntimeError(packet, "Unexpected packet type: %s", packet->getClassName());
    }
}

const InterfaceEntry *IPv4::getSourceInterfaceFrom(cPacket *packet)
{
    auto interfaceInd = packet->getTag<InterfaceInd>();
    return interfaceInd != nullptr ? ift->getInterfaceById(interfaceInd->getInterfaceId()) : nullptr;
}

void IPv4::handleIncomingDatagram(Packet *packet, const InterfaceEntry *fromIE)
{
    ASSERT(packet);
    ASSERT(fromIE);

    //
    // "Prerouting"
    //

    const auto& datagram = packet->peekHeader<IPv4Header>();
    ASSERT(datagram);
    // check for header biterror
    if (packet->hasBitError()) {
        // probability of bit error in header = size of header / size of total message
        // (ignore bit error if in payload)
        double relativeHeaderLength = datagram->getHeaderLength() / (double)datagram->getChunkLength();
        if (dblrand() <= relativeHeaderLength) {
            EV_WARN << "bit error found, sending ICMP_PARAMETER_PROBLEM\n";
            sendIcmpError(packet, fromIE->getInterfaceId(), ICMP_PARAMETER_PROBLEM, 0);
            return;
        }
    }

    EV_DETAIL << "Received datagram `" << datagram->getName() << "' with dest=" << datagram->getDestAddress() << "\n";

    const InterfaceEntry *destIE = nullptr;
    L3Address nextHop(IPv4Address::UNSPECIFIED_ADDRESS);
    if (datagramPreRoutingHook(packet, fromIE, destIE, nextHop) == INetfilter::IHook::ACCEPT)
        preroutingFinish(packet, fromIE, destIE, nextHop.toIPv4());
}

namespace {
Packet *toMutable(Packet *packet)
{
    auto newPacket = new Packet(packet->getName());
    newPacket->append(packet->peekDataAt(0, packet->getDataLength()));
    delete packet;
    return newPacket;
}
}

void IPv4::preroutingFinish(Packet *packet, const InterfaceEntry *fromIE, const InterfaceEntry *destIE, IPv4Address nextHopAddr)
{
    const auto& datagram = packet->peekHeader<IPv4Header>();
    ASSERT(datagram);
    IPv4Address& destAddr = datagram->getDestAddress();

    // route packet

    if (fromIE->isLoopback()) {
        reassembleAndDeliver(packet, fromIE);
    }
    else if (destAddr.isMulticast()) {
        // check for local delivery
        // Note: multicast routers will receive IGMP datagrams even if their interface is not joined to the group
        if (fromIE->ipv4Data()->isMemberOfMulticastGroup(destAddr) ||
            (rt->isMulticastForwardingEnabled() && datagram->getTransportProtocol() == IP_PROT_IGMP))
            reassembleAndDeliver(packet->dup(), fromIE);
        else
            EV_WARN << "Skip local delivery of multicast datagram (input interface not in multicast group)\n";

        // don't forward if IP forwarding is off, or if dest address is link-scope
        if (!rt->isMulticastForwardingEnabled()) {
            EV_WARN << "Skip forwarding of multicast datagram (forwarding disabled)\n";
            delete packet;
        }
        else if (destAddr.isLinkLocalMulticast()) {
            EV_WARN << "Skip forwarding of multicast datagram (packet is link-local)\n";
            delete packet;
        }
        else if (datagram->getTimeToLive() == 0) {
            EV_WARN << "Skip forwarding of multicast datagram (TTL reached 0)\n";
            delete packet;
        }
        else {
            auto newHeader = std::static_pointer_cast<IPv4Header>(packet->popHeader<IPv4Header>()->dupShared());
            // needed a mutable copy for forwarding
            forwardMulticastPacket(toMutable(packet), newHeader, fromIE);
        }
    }
    else {
        const InterfaceEntry *broadcastIE = nullptr;

        // check for local delivery; we must accept also packets coming from the interfaces that
        // do not yet have an IP address assigned. This happens during DHCP requests.
        if (rt->isLocalAddress(destAddr) || fromIE->ipv4Data()->getIPAddress().isUnspecified()) {
            reassembleAndDeliver(packet, fromIE);
        }
        else if (destAddr.isLimitedBroadcastAddress() || (broadcastIE = rt->findInterfaceByLocalBroadcastAddress(destAddr))) {
            // broadcast datagram on the target subnet if we are a router
            if (broadcastIE && fromIE != broadcastIE && rt->isForwardingEnabled()) {
                auto newHeader = std::static_pointer_cast<IPv4Header>(packet->popHeader<IPv4Header>()->dupShared());
                // needed a mutable copy for forwarding
                fragmentPostRouting(toMutable(packet->dup()), newHeader, broadcastIE, IPv4Address::ALLONES_ADDRESS);
            }

            EV_INFO << "Broadcast received\n";
            reassembleAndDeliver(packet, fromIE);
        }
        else if (!rt->isForwardingEnabled()) {
            EV_WARN << "forwarding off, dropping packet\n";
            numDropped++;
            delete packet;
        }
        else {
            auto newHeader = std::static_pointer_cast<IPv4Header>(packet->popHeader<IPv4Header>()->dupShared());
            // needed a mutable copy for forwarding
            routeUnicastPacket(toMutable(packet), newHeader, fromIE, destIE, nextHopAddr);
        }
    }
}

void IPv4::handlePacketFromHL(Packet *packet)
{
    EV_INFO << "Received " << packet << " from upper layer.\n";

    // if no interface exists, do not send datagram
    if (ift->getNumInterfaces() == 0) {
        EV_ERROR << "No interfaces exist, dropping packet\n";
        numDropped++;
        delete packet;
        return;
    }

    // encapsulate
    const auto& ipv4Header = encapsulate(packet);

    // extract requested interface and next hop
    auto ifTag = packet->getTag<InterfaceReq>();
    const InterfaceEntry *destIE = ifTag ? const_cast<const InterfaceEntry *>(ift->getInterfaceById(ifTag->getInterfaceId())) : nullptr;

    // TODO:
    L3Address nextHopAddr(IPv4Address::UNSPECIFIED_ADDRESS);
    if (datagramLocalOutHook(packet, destIE, nextHopAddr) == INetfilter::IHook::ACCEPT)
        datagramLocalOut(packet, ipv4Header, destIE, nextHopAddr.toIPv4());
}

void IPv4::datagramLocalOut(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *destIE, IPv4Address requestedNextHopAddress)
{
//    const auto& datagram = packet->peekHeader<IPv4Header>();
    bool multicastLoop = false;
    MulticastReq *mcr = packet->getTag<MulticastReq>();
    if (mcr != nullptr) {
        multicastLoop = mcr->getMulticastLoop();
    }

    // send
    IPv4Address& destAddr = ipv4Header->getDestAddress();

    EV_DETAIL << "Sending datagram " << packet << " with destination = " << destAddr << "\n";

    if (ipv4Header->getDestAddress().isMulticast()) {
        destIE = determineOutgoingInterfaceForMulticastDatagram(ipv4Header.get(), destIE);

        // loop back a copy
        if (multicastLoop && (!destIE || !destIE->isLoopback())) {
            const InterfaceEntry *loopbackIF = ift->getFirstLoopbackInterface();
            if (loopbackIF)
                fragmentPostRouting(packet->dup(), ipv4Header, loopbackIF, destAddr);
        }

        if (destIE) {
            numMulticast++;
            fragmentPostRouting(packet, ipv4Header, destIE, destAddr);
        }
        else {
            EV_ERROR << "No multicast interface, packet dropped\n";
            numUnroutable++;
            delete packet;
        }
    }
    else {    // unicast and broadcast
              // check for local delivery
        if (rt->isLocalAddress(destAddr)) {
            EV_INFO << "Delivering " << packet << " locally.\n";
            if (destIE && !destIE->isLoopback()) {
                EV_DETAIL << "datagram destination address is local, ignoring destination interface specified in the control info\n";
                destIE = nullptr;
            }
            if (!destIE)
                destIE = ift->getFirstLoopbackInterface();
            ASSERT(destIE);
            routeUnicastPacket(packet, ipv4Header, nullptr, destIE, destAddr);
        }
        else if (destAddr.isLimitedBroadcastAddress() || rt->isLocalBroadcastAddress(destAddr))
            routeLocalBroadcastPacket(packet, ipv4Header, destIE);
        else
            routeUnicastPacket(packet, ipv4Header, nullptr, destIE, requestedNextHopAddress);
    }
}

/* Choose the outgoing interface for the muticast datagram:
 *   1. use the interface specified by MULTICAST_IF socket option (received in the control info)
 *   2. lookup the destination address in the routing table
 *   3. if no route, choose the interface according to the source address
 *   4. or if the source address is unspecified, choose the first MULTICAST interface
 */
const InterfaceEntry *IPv4::determineOutgoingInterfaceForMulticastDatagram(const IPv4Header *datagram, const InterfaceEntry *multicastIFOption)
{
    const InterfaceEntry *ie = nullptr;
    if (multicastIFOption) {
        ie = multicastIFOption;
        EV_DETAIL << "multicast packet routed by socket option via output interface " << ie->getName() << "\n";
    }
    if (!ie) {
        IPv4Route *route = rt->findBestMatchingRoute(datagram->getDestAddress());
        if (route)
            ie = route->getInterface();
        if (ie)
            EV_DETAIL << "multicast packet routed by routing table via output interface " << ie->getName() << "\n";
    }
    if (!ie) {
        ie = rt->getInterfaceByAddress(datagram->getSrcAddress());
        if (ie)
            EV_DETAIL << "multicast packet routed by source address via output interface " << ie->getName() << "\n";
    }
    if (!ie) {
        ie = ift->getFirstMulticastInterface();
        if (ie)
            EV_DETAIL << "multicast packet routed via the first multicast interface " << ie->getName() << "\n";
    }
    return ie;
}

void IPv4::routeUnicastPacket(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *fromIE, const InterfaceEntry *destIE, IPv4Address requestedNextHopAddress)
{
//    const auto& datagram = packet->peekHeader<IPv4Header>();
    IPv4Address destAddr = ipv4Header->getDestAddress();
    EV_INFO << "Routing " << packet << " with destination = " << destAddr << ", ";

    IPv4Address nextHopAddr;
    // if output port was explicitly requested, use that, otherwise use IPv4 routing
    if (destIE) {
        EV_DETAIL << "using manually specified output interface " << destIE->getName() << "\n";
        // and nextHopAddr remains unspecified
        if (!requestedNextHopAddress.isUnspecified())
            nextHopAddr = requestedNextHopAddress;
        // special case ICMP reply
        else if (destIE->isBroadcast()) {
            // if the interface is broadcast we must search the next hop
            const IPv4Route *re = rt->findBestMatchingRoute(destAddr);
            if (re && re->getInterface() == destIE)
                nextHopAddr = re->getGateway();
        }
    }
    else {
        // use IPv4 routing (lookup in routing table)
        const IPv4Route *re = rt->findBestMatchingRoute(destAddr);
        if (re) {
            destIE = re->getInterface();
            nextHopAddr = re->getGateway();
        }
    }

    if (!destIE) {    // no route found
        EV_WARN << "unroutable, sending ICMP_DESTINATION_UNREACHABLE\n";
        numUnroutable++;
        sendIcmpError(packet, fromIE ? fromIE->getInterfaceId() : -1, ICMP_DESTINATION_UNREACHABLE, 0);
    }
    else {    // fragment and send
        L3Address nextHop(nextHopAddr);
        if (fromIE != nullptr) {
            if (datagramForwardHook(packet, fromIE, destIE, nextHop) != INetfilter::IHook::ACCEPT)
                return;
            nextHopAddr = nextHop.toIPv4();
        }

        routeUnicastPacketFinish(packet, ipv4Header, fromIE, destIE, nextHopAddr);
    }
}

void IPv4::routeUnicastPacketFinish(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *fromIE, const InterfaceEntry *destIE, IPv4Address nextHopAddr)
{
    EV_INFO << "output interface = " << destIE->getName() << ", next hop address = " << nextHopAddr << "\n";
    numForwarded++;
    fragmentPostRouting(packet, ipv4Header, destIE, nextHopAddr);
}

void IPv4::routeLocalBroadcastPacket(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *destIE)
{
    // The destination address is 255.255.255.255 or local subnet broadcast address.
    // We always use 255.255.255.255 as nextHopAddress, because it is recognized by ARP,
    // and mapped to the broadcast MAC address.
    if (destIE != nullptr) {
        fragmentPostRouting(packet, ipv4Header, destIE, IPv4Address::ALLONES_ADDRESS);
    }
    else if (forceBroadcast) {
        // forward to each interface including loopback
        for (int i = 0; i < ift->getNumInterfaces(); i++) {
            const InterfaceEntry *ie = ift->getInterface(i);
            fragmentPostRouting(packet->dup(), ipv4Header, ie, IPv4Address::ALLONES_ADDRESS);
        }
        delete packet;
    }
    else {
        numDropped++;
        delete packet;
    }
}

const InterfaceEntry *IPv4::getShortestPathInterfaceToSource(IPv4Header *datagram)
{
    return rt->getInterfaceForDestAddr(datagram->getSrcAddress());
}

void IPv4::forwardMulticastPacket(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *fromIE)
{
    ASSERT(fromIE);
    auto datagram = packet->peekHeader<IPv4Header>();
    const IPv4Address& srcAddr = datagram->getSrcAddress();
    const IPv4Address& destAddr = datagram->getDestAddress();
    ASSERT(destAddr.isMulticast());
    ASSERT(!destAddr.isLinkLocalMulticast());

    EV_INFO << "Forwarding multicast datagram `" << packet->getName() << "' with dest=" << destAddr << "\n";

    numMulticast++;

    const IPv4MulticastRoute *route = rt->findBestMatchingMulticastRoute(srcAddr, destAddr);
    if (!route) {
        EV_WARN << "Multicast route does not exist, try to add.\n";
        // TODO: no need to emit fromIE when tags will be used in place of control infos
        emit(NF_IPv4_NEW_MULTICAST, datagram.get(), const_cast<InterfaceEntry *>(fromIE));

        // read new record
        route = rt->findBestMatchingMulticastRoute(srcAddr, destAddr);

        if (!route) {
            EV_ERROR << "No route, packet dropped.\n";
            numUnroutable++;
            delete packet;
            return;
        }
    }

    if (route->getInInterface() && fromIE != route->getInInterface()->getInterface()) {
        EV_ERROR << "Did not arrive on input interface, packet dropped.\n";
        // TODO: no need to emit fromIE when tags will be used in place of control infos
        emit(NF_IPv4_DATA_ON_NONRPF, datagram.get(), const_cast<InterfaceEntry *>(fromIE));
        numDropped++;
        delete packet;
    }
    // backward compatible: no parent means shortest path interface to source (RPB routing)
    else if (!route->getInInterface() && fromIE != getShortestPathInterfaceToSource(datagram.get())) {
        EV_ERROR << "Did not arrive on shortest path, packet dropped.\n";
        numDropped++;
        delete packet;
    }
    else {
        // TODO: no need to emit fromIE when tags will be used in place of control infos
        emit(NF_IPv4_DATA_ON_RPF, packet, const_cast<InterfaceEntry *>(fromIE));    // forwarding hook

        numForwarded++;
        // copy original datagram for multiple destinations
        for (unsigned int i = 0; i < route->getNumOutInterfaces(); i++) {
            IPv4MulticastRoute::OutInterface *outInterface = route->getOutInterface(i);
            const InterfaceEntry *destIE = outInterface->getInterface();
            if (destIE != fromIE && outInterface->isEnabled()) {
                int ttlThreshold = destIE->ipv4Data()->getMulticastTtlThreshold();
                if (datagram->getTimeToLive() <= ttlThreshold)
                    EV_WARN << "Not forwarding to " << destIE->getName() << " (ttl treshold reached)\n";
                else if (outInterface->isLeaf() && !destIE->ipv4Data()->hasMulticastListener(destAddr))
                    EV_WARN << "Not forwarding to " << destIE->getName() << " (no listeners)\n";
                else {
                    EV_DETAIL << "Forwarding to " << destIE->getName() << "\n";
                    fragmentPostRouting(packet->dup(), ipv4Header, destIE, destAddr);
                }
            }
        }

        // TODO: no need to emit fromIE when tags will be used in place of control infos
        emit(NF_IPv4_MDATA_REGISTER, packet, const_cast<InterfaceEntry *>(fromIE));    // postRouting hook

        // only copies sent, delete original datagram
        delete packet;
    }
}

void IPv4::reassembleAndDeliver(Packet *packet, const InterfaceEntry *fromIE)
{
    EV_INFO << "Delivering " << packet << " locally.\n";

    const auto& datagram = packet->peekHeader<IPv4Header>();
    if (datagram->getSrcAddress().isUnspecified())
        EV_WARN << "Received datagram '" << packet->getName() << "' without source address filled in\n";

    // reassemble the packet (if fragmented)
    if (datagram->getFragmentOffset() != 0 || datagram->getMoreFragments()) {
        EV_DETAIL << "Datagram fragment: offset=" << datagram->getFragmentOffset()
                  << ", MORE=" << (datagram->getMoreFragments() ? "true" : "false") << ".\n";

        // erase timed out fragments in fragmentation buffer; check every 10 seconds max
        if (simTime() >= lastCheckTime + 10) {
            lastCheckTime = simTime();
            fragbuf.purgeStaleFragments(icmp, simTime() - fragmentTimeoutTime);
        }

        packet = fragbuf.addFragment(packet, simTime());
        if (!packet) {
            EV_DETAIL << "No complete datagram yet.\n";
            return;
        }
        EV_DETAIL << "This fragment completes the datagram.\n";
    }

    if (datagramLocalInHook(packet, fromIE) != INetfilter::IHook::ACCEPT) {
        return;
    }

    reassembleAndDeliverFinish(packet, fromIE);
}

void IPv4::reassembleAndDeliverFinish(Packet *packet, const InterfaceEntry *fromIE)
{
    auto ipv4HeaderPosition = packet->getHeaderPopOffset();
    const auto& datagram = packet->peekHeader<IPv4Header>();
    int protocol = datagram->getTransportProtocol();
    decapsulate(packet);
    auto lowerBound = protocolIdToSocketDescriptors.lower_bound(protocol);
    auto upperBound = protocolIdToSocketDescriptors.upper_bound(protocol);
    bool hasSocket = lowerBound != upperBound;
    for (auto it = lowerBound; it != upperBound; it++) {
        cPacket *packetCopy = utils::dupPacketAndControlInfo(packet);
        packetCopy->ensureTag<SocketInd>()->setSocketId(it->second->socketId);
        send(packetCopy, "transportOut");
    }
    if (mapping.findOutputGateForProtocol(protocol) >= 0) {
        send(packet, "transportOut");
        numLocalDeliver++;
    }
    else if (hasSocket) {
        delete packet;
    }
    else {
        EV_ERROR << "Transport protocol ID=" << protocol << " not connected, discarding packet\n";
        packet->setHeaderPopOffset(ipv4HeaderPosition);
        sendIcmpError(packet, fromIE ? fromIE->getInterfaceId() : -1, ICMP_DESTINATION_UNREACHABLE, ICMP_DU_PROTOCOL_UNREACHABLE);
    }
}

void IPv4::decapsulate(Packet *packet)
{
    // decapsulate transport packet
    auto ipv4HeaderPos = packet->getHeaderPopOffset();
    const auto& datagram = packet->popHeader<IPv4Header>();

    // create and fill in control info
    packet->ensureTag<DscpInd>()->setDifferentiatedServicesCodePoint(datagram->getDiffServCodePoint());
    packet->ensureTag<EcnInd>()->setExplicitCongestionNotification(datagram->getExplicitCongestionNotification());

    // original IPv4 datagram might be needed in upper layers to send back ICMP error message

    auto transportProtocol = ProtocolGroup::ipprotocol.getProtocol(datagram->getTransportProtocol());
    packet->ensureTag<PacketProtocolTag>()->setProtocol(transportProtocol);
    packet->ensureTag<DispatchProtocolReq>()->setProtocol(transportProtocol);
    packet->ensureTag<NetworkProtocolInd>()->setProtocol(&Protocol::ipv4);
    packet->ensureTag<NetworkProtocolInd>()->setPosition(ipv4HeaderPos);
    auto l3AddressInd = packet->ensureTag<L3AddressInd>();
    l3AddressInd->setSrcAddress(datagram->getSrcAddress());
    l3AddressInd->setDestAddress(datagram->getDestAddress());
    packet->ensureTag<HopLimitInd>()->setHopLimit(datagram->getTimeToLive());
}

void IPv4::fragmentPostRouting(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *destIe, IPv4Address nextHopAddr)
{
    L3Address nextHop(nextHopAddr);
    if (datagramPostRoutingHook(packet, getSourceInterfaceFrom(packet), destIe, nextHop) == INetfilter::IHook::ACCEPT)
        fragmentAndSend(packet, ipv4Header, destIe, nextHop.toIPv4());
}

void IPv4::fragmentAndSend(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *destIe, IPv4Address nextHopAddr)
{
    // fill in source address
    if (ipv4Header->getSrcAddress().isUnspecified())
        ipv4Header->setSrcAddress(destIe->ipv4Data()->getIPAddress());

    // hop counter decrement; but not if it will be locally delivered
    if (!destIe->isLoopback())
        ipv4Header->setTimeToLive(ipv4Header->getTimeToLive() - 1);

    // hop counter check
    if (ipv4Header->getTimeToLive() < 0) {
        // drop datagram, destruction responsibility in ICMP
        EV_WARN << "datagram TTL reached zero, sending ICMP_TIME_EXCEEDED\n";
        sendIcmpError(packet, -1    /*TODO*/, ICMP_TIME_EXCEEDED, 0);
        numDropped++;
        return;
    }

    int mtu = destIe->getMTU();

    // send datagram straight out if it doesn't require fragmentation (note: mtu==0 means infinite mtu)
    if (mtu == 0 || packet->getByteLength() <= mtu) {
        ipv4Header->markImmutable();
        packet->prepend(ipv4Header);
        sendDatagramToOutput(packet, destIe, nextHopAddr);
        return;
    }

    // if "don't fragment" bit is set, throw datagram away and send ICMP error message
    if (ipv4Header->getDontFragment()) {
        EV_WARN << "datagram larger than MTU and don't fragment bit set, sending ICMP_DESTINATION_UNREACHABLE\n";
        sendIcmpError(packet, -1    /*TODO*/, ICMP_DESTINATION_UNREACHABLE,
                ICMP_DU_FRAGMENTATION_NEEDED);
        numDropped++;
        return;
    }

    packet->markContentsImmutable();
    // FIXME some IP options should not be copied into each fragment, check their COPY bit
    int headerLength = ipv4Header->getHeaderLength();
    int payloadLength = packet->getDataLength() - headerLength;
    int fragmentLength = ((mtu - headerLength) / 8) * 8;    // payload only (without header)
    int offsetBase = ipv4Header->getFragmentOffset();
    if (fragmentLength <= 0)
        throw cRuntimeError("Cannot fragment datagram: MTU=%d too small for header size (%d bytes)", mtu, headerLength); // exception and not ICMP because this is likely a simulation configuration error, not something one wants to simulate

    int noOfFragments = (payloadLength + fragmentLength - 1) / fragmentLength;
    EV_DETAIL << "Breaking datagram into " << noOfFragments << " fragments\n";

    // create and send fragments
    std::string fragMsgName = packet->getName();
    fragMsgName += "-frag-";

    int offset = 0;
    while (offset < payloadLength) {
        bool lastFragment = (offset + fragmentLength >= payloadLength);
        // length equal to fragmentLength, except for last fragment;
        int thisFragmentLength = lastFragment ? payloadLength - offset : fragmentLength;

        std::string curFragName = fragMsgName + std::to_string(offset);
        if (lastFragment)
            curFragName += "-last";
        Packet *fragment = new Packet(curFragName.c_str());     //TODO add offset or index to fragment name

        //copy Tags from packet to fragment     //FIXME optimizing needed
        {
            Packet *tmp = packet->dup();
            fragment->transferTagsFrom(tmp);
            delete tmp;
        }

        ASSERT(fragment->getByteLength() == 0);
        const auto& fraghdr = std::make_shared<IPv4Header>(*ipv4Header.get()->dup());
        fragment->append(fraghdr);
        ASSERT(fragment->getByteLength() == headerLength);
        const auto& fragData = packet->peekDataAt(headerLength + offset, thisFragmentLength);
        ASSERT(fragData->getChunkLength() == thisFragmentLength);
        fragment->append(fragData);
        ASSERT(fragment->getByteLength() == headerLength + thisFragmentLength);

        // "more fragments" bit is unchanged in the last fragment, otherwise true
        if (!lastFragment)
            fraghdr->setMoreFragments(true);

        fraghdr->setFragmentOffset(offsetBase + offset);
        fraghdr->setTotalLengthField(headerLength + thisFragmentLength);

        int bl = fragment->getByteLength();
        ASSERT(fragment->getByteLength() == headerLength + thisFragmentLength);

        ipv4Header->markImmutable();
        fragment->prepend(ipv4Header);
        sendDatagramToOutput(fragment, destIe, nextHopAddr);
        offset += thisFragmentLength;
    }

    delete packet;
}

std::shared_ptr<IPv4Header> IPv4::encapsulate(Packet *transportPacket)
{
    const auto& ipv4Header = std::make_shared<IPv4Header>();

    auto l3AddressReq = transportPacket->removeMandatoryTag<L3AddressReq>();
    IPv4Address src = l3AddressReq->getSrcAddress().toIPv4();
    IPv4Address dest = l3AddressReq->getDestAddress().toIPv4();
    delete l3AddressReq;

    ipv4Header->setTransportProtocol(ProtocolGroup::ipprotocol.getProtocolNumber(transportPacket->getMandatoryTag<PacketProtocolTag>()->getProtocol()));

    auto hopLimitReq = transportPacket->removeTag<HopLimitReq>();
    short ttl = (hopLimitReq != nullptr) ? hopLimitReq->getHopLimit() : -1;
    delete hopLimitReq;
    bool dontFragment = false;
    if (auto dontFragmentReq = transportPacket->removeTag<FragmentationReq>()) {
        dontFragment = dontFragmentReq->getDontFragment();
        delete dontFragmentReq;
    }

    // set source and destination address
    ipv4Header->setDestAddress(dest);

    // when source address was given, use it; otherwise it'll get the address
    // of the outgoing interface after routing
    if (!src.isUnspecified()) {
        // if interface parameter does not match existing interface, do not send datagram
        if (rt->getInterfaceByAddress(src) == nullptr)
            throw cRuntimeError("Wrong source address %s in (%s)%s: no interface with such address",
                    src.str().c_str(), transportPacket->getClassName(), transportPacket->getFullName());

        ipv4Header->setSrcAddress(src);
    }

    // set other fields
    if (DscpReq *dscpReq = transportPacket->removeTag<DscpReq>()) {
        ipv4Header->setDiffServCodePoint(dscpReq->getDifferentiatedServicesCodePoint());
        delete dscpReq;
    }
    if (EcnReq *ecnReq = transportPacket->removeTag<EcnReq>()) {
        ipv4Header->setExplicitCongestionNotification(ecnReq->getExplicitCongestionNotification());
        delete ecnReq;
    }

    ipv4Header->setIdentification(curFragmentId++);
    ipv4Header->setMoreFragments(false);
    ipv4Header->setDontFragment(dontFragment);
    ipv4Header->setFragmentOffset(0);

    if (ttl != -1) {
        ASSERT(ttl > 0);
    }
    else if (ipv4Header->getDestAddress().isLinkLocalMulticast())
        ttl = 1;
    else if (ipv4Header->getDestAddress().isMulticast())
        ttl = defaultMCTimeToLive;
    else
        ttl = defaultTimeToLive;
    ipv4Header->setTimeToLive(ttl);
    ipv4Header->setTotalLengthField(ipv4Header->getChunkLength() + transportPacket->getByteLength());
//    ipv4Header->markImmutable();
    //transportPacket->prepend(ipv4Header);
    // setting IPv4 options is currently not supported
    return ipv4Header;
}

void IPv4::sendDatagramToOutput(Packet *packet, const InterfaceEntry *ie, IPv4Address nextHopAddr)
{
    {
        bool isIeee802Lan = ie->isBroadcast() && !ie->getMacAddress().isUnspecified();    // we only need/can do ARP on IEEE 802 LANs
        if (!isIeee802Lan) {
            delete packet->removeControlInfo();
            packet->removeTag<DispatchProtocolReq>();         // send to NIC
            sendPacketToNIC(packet, ie);
        }
        else {
            if (nextHopAddr.isUnspecified()) {
                IPv4InterfaceData *ipv4Data = ie->ipv4Data();
                const auto& ipv4hdr = packet->peekHeader<IPv4Header>();
                IPv4Address destAddress = ipv4hdr->getDestAddress();
                if (IPv4Address::maskedAddrAreEqual(destAddress, ie->ipv4Data()->getIPAddress(), ipv4Data->getNetmask()))
                    nextHopAddr = destAddress;
                else if (useProxyARP) {
                    nextHopAddr = destAddress;
                    EV_WARN << "no next-hop address, using destination address " << nextHopAddr << " (proxy ARP)\n";
                }
                else {
                    throw cRuntimeError(packet, "Cannot send datagram on broadcast interface: no next-hop address and Proxy ARP is disabled");
                }
            }

            MACAddress nextHopMacAddr;    // unspecified
            nextHopMacAddr = resolveNextHopMacAddress(packet, nextHopAddr, ie);

            if (nextHopMacAddr.isUnspecified()) {
                EV_INFO << "Pending " << packet << " to ARP resolution.\n";
                pendingPackets[nextHopAddr].insert(packet);
            }
            else {
                ASSERT2(pendingPackets.find(nextHopAddr) == pendingPackets.end(), "IPv4-ARP error: nextHopAddr found in ARP table, but IPv4 queue for nextHopAddr not empty");
                sendPacketToIeee802NIC(packet, ie, nextHopMacAddr, ETHERTYPE_IPv4);
            }
        }
    }
}

void IPv4::arpResolutionCompleted(IARP::Notification *entry)
{
    if (entry->l3Address.getType() != L3Address::IPv4)
        return;
    auto it = pendingPackets.find(entry->l3Address.toIPv4());
    if (it != pendingPackets.end()) {
        cPacketQueue& packetQueue = it->second;
        EV << "ARP resolution completed for " << entry->l3Address << ". Sending " << packetQueue.getLength()
           << " waiting packets from the queue\n";

        while (!packetQueue.isEmpty()) {
            cPacket *msg = packetQueue.pop();
            EV << "Sending out queued packet " << msg << "\n";
            sendPacketToIeee802NIC(msg, entry->ie, entry->macAddress, ETHERTYPE_IPv4);
        }
        pendingPackets.erase(it);
    }
}

void IPv4::arpResolutionTimedOut(IARP::Notification *entry)
{
    if (entry->l3Address.getType() != L3Address::IPv4)
        return;
    auto it = pendingPackets.find(entry->l3Address.toIPv4());
    if (it != pendingPackets.end()) {
        cPacketQueue& packetQueue = it->second;
        EV << "ARP resolution failed for " << entry->l3Address << ",  dropping " << packetQueue.getLength() << " packets\n";
        packetQueue.clear();
        pendingPackets.erase(it);
    }
}

MACAddress IPv4::resolveNextHopMacAddress(cPacket *packet, IPv4Address nextHopAddr, const InterfaceEntry *destIE)
{
    if (nextHopAddr.isLimitedBroadcastAddress() || nextHopAddr == destIE->ipv4Data()->getNetworkBroadcastAddress()) {
        EV_DETAIL << "destination address is broadcast, sending packet to broadcast MAC address\n";
        return MACAddress::BROADCAST_ADDRESS;
    }

    if (nextHopAddr.isMulticast()) {
        MACAddress macAddr = MACAddress::makeMulticastAddress(nextHopAddr);
        EV_DETAIL << "destination address is multicast, sending packet to MAC address " << macAddr << "\n";
        return macAddr;
    }

    return arp->resolveL3Address(nextHopAddr, destIE);
}

void IPv4::sendPacketToIeee802NIC(cPacket *packet, const InterfaceEntry *ie, const MACAddress& macAddress, int etherType)
{
    // remove old control info
    delete packet->removeControlInfo();

    // add control info with MAC address
    packet->ensureTag<EtherTypeReq>()->setEtherType(etherType);
    packet->ensureTag<MacAddressReq>()->setDestAddress(macAddress);
    packet->removeTag<DispatchProtocolReq>();         // send to NIC

    sendPacketToNIC(packet, ie);
}

void IPv4::sendPacketToNIC(cPacket *packet, const InterfaceEntry *ie)
{
    EV_INFO << "Sending " << packet << " to output interface = " << ie->getName() << ".\n";
    packet->ensureTag<PacketProtocolTag>()->setProtocol(&Protocol::ipv4);
    packet->ensureTag<NetworkProtocolTag>()->setProtocol(&Protocol::ipv4);
    packet->ensureTag<DispatchProtocolInd>()->setProtocol(&Protocol::ipv4);
    packet->ensureTag<InterfaceReq>()->setInterfaceId(ie->getInterfaceId());
    send(packet, "queueOut");
}

// NetFilter:

void IPv4::registerHook(int priority, INetfilter::IHook *hook)
{
    Enter_Method("registerHook()");
    NetfilterBase::registerHook(priority, hook);
}

void IPv4::unregisterHook(INetfilter::IHook *hook)
{
    Enter_Method("unregisterHook()");
    NetfilterBase::unregisterHook(hook);
}

void IPv4::dropQueuedDatagram(const Packet *datagram)
{
    Enter_Method("dropQueuedDatagram()");
    for (auto iter = queuedDatagramsForHooks.begin(); iter != queuedDatagramsForHooks.end(); iter++) {
        if (iter->datagram == datagram) {
            delete datagram;
            queuedDatagramsForHooks.erase(iter);
            return;
        }
    }
}

void IPv4::reinjectQueuedDatagram(const Packet *datagram)
{
    Enter_Method("reinjectDatagram()");
    for (auto iter = queuedDatagramsForHooks.begin(); iter != queuedDatagramsForHooks.end(); iter++) {
        if (iter->datagram == datagram) {
            auto *datagram = iter->datagram;
            take(datagram);
            const auto& ipv4Header = datagram->peekHeader<IPv4Header>();
            switch (iter->hookType) {
                case INetfilter::IHook::LOCALOUT:
                    datagramLocalOut(datagram, ipv4Header, iter->outIE, iter->nextHopAddr);
                    break;

                case INetfilter::IHook::PREROUTING:
                    preroutingFinish(datagram, iter->inIE, iter->outIE, iter->nextHopAddr);
                    break;

                case INetfilter::IHook::POSTROUTING:
                    fragmentAndSend(datagram, ipv4Header, iter->outIE, iter->nextHopAddr);
                    break;

                case INetfilter::IHook::LOCALIN:
                    reassembleAndDeliverFinish(datagram, iter->inIE);
                    break;

                case INetfilter::IHook::FORWARD:
                    routeUnicastPacketFinish(datagram, ipv4Header, iter->inIE, iter->outIE, iter->nextHopAddr);
                    break;

                default:
                    throw cRuntimeError("Unknown hook ID: %d", (int)(iter->hookType));
                    break;
            }
            queuedDatagramsForHooks.erase(iter);
            return;
        }
    }
}

INetfilter::IHook::Result IPv4::datagramPreRoutingHook(Packet *datagram, const InterfaceEntry *inIE, const InterfaceEntry *& outIE, L3Address& nextHopAddr)
{
    for (auto & elem : hooks) {
        IHook::Result r = elem.second->datagramPreRoutingHook(datagram, inIE, outIE, nextHopAddr);
        switch (r) {
            case INetfilter::IHook::ACCEPT:
                break;    // continue iteration

            case INetfilter::IHook::DROP:
                delete datagram;
                return r;

            case INetfilter::IHook::QUEUE:
                queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, inIE, outIE, nextHopAddr.toIPv4(), INetfilter::IHook::PREROUTING));
                return r;

            case INetfilter::IHook::STOLEN:
                return r;

            default:
                throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return INetfilter::IHook::ACCEPT;
}

INetfilter::IHook::Result IPv4::datagramForwardHook(Packet *datagram, const InterfaceEntry *inIE, const InterfaceEntry *& outIE, L3Address& nextHopAddr)
{
    for (auto & elem : hooks) {
        IHook::Result r = elem.second->datagramForwardHook(datagram, inIE, outIE, nextHopAddr);
        switch (r) {
            case INetfilter::IHook::ACCEPT:
                break;    // continue iteration

            case INetfilter::IHook::DROP:
                delete datagram;
                return r;

            case INetfilter::IHook::QUEUE:
                queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, inIE, outIE, nextHopAddr.toIPv4(), INetfilter::IHook::FORWARD));
                return r;

            case INetfilter::IHook::STOLEN:
                return r;

            default:
                throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return INetfilter::IHook::ACCEPT;
}

INetfilter::IHook::Result IPv4::datagramPostRoutingHook(Packet *datagram, const InterfaceEntry *inIE, const InterfaceEntry *& outIE, L3Address& nextHopAddr)
{
    for (auto & elem : hooks) {
        IHook::Result r = elem.second->datagramPostRoutingHook(datagram, inIE, outIE, nextHopAddr);
        switch (r) {
            case INetfilter::IHook::ACCEPT:
                break;    // continue iteration

            case INetfilter::IHook::DROP:
                delete datagram;
                return r;

            case INetfilter::IHook::QUEUE:
                queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, inIE, outIE, nextHopAddr.toIPv4(), INetfilter::IHook::POSTROUTING));
                return r;

            case INetfilter::IHook::STOLEN:
                return r;

            default:
                throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return INetfilter::IHook::ACCEPT;
}

bool IPv4::handleOperationStage(LifecycleOperation *operation, int stage, IDoneCallback *doneCallback)
{
    Enter_Method_Silent();
    if (dynamic_cast<NodeStartOperation *>(operation)) {
        if ((NodeStartOperation::Stage)stage == NodeStartOperation::STAGE_NETWORK_LAYER)
            start();
    }
    else if (dynamic_cast<NodeShutdownOperation *>(operation)) {
        if ((NodeShutdownOperation::Stage)stage == NodeShutdownOperation::STAGE_NETWORK_LAYER)
            stop();
    }
    else if (dynamic_cast<NodeCrashOperation *>(operation)) {
        if ((NodeCrashOperation::Stage)stage == NodeCrashOperation::STAGE_CRASH)
            stop();
    }
    return true;
}

void IPv4::start()
{
    ASSERT(queue.isEmpty());
    isUp = true;
}

void IPv4::stop()
{
    isUp = false;
    flush();
}

void IPv4::flush()
{
    delete cancelService();
    EV_DEBUG << "IPv4::flush(): packets in queue: " << queue.info() << endl;
    queue.clear();

    EV_DEBUG << "IPv4::flush(): pending packets:\n";
    for (auto & elem : pendingPackets) {
        EV_DEBUG << "IPv4::flush():    " << elem.first << ": " << elem.second.info() << endl;
        elem.second.clear();
    }
    pendingPackets.clear();

    EV_DEBUG << "IPv4::flush(): packets in hooks: " << queuedDatagramsForHooks.size() << endl;
    for (auto & elem : queuedDatagramsForHooks) {
        delete elem.datagram;
    }
    queuedDatagramsForHooks.clear();
}

bool IPv4::isNodeUp()
{
    NodeStatus *nodeStatus = dynamic_cast<NodeStatus *>(findContainingNode(this)->getSubmodule("status"));
    return !nodeStatus || nodeStatus->getState() == NodeStatus::UP;
}

INetfilter::IHook::Result IPv4::datagramLocalInHook(Packet *datagram, const InterfaceEntry *inIE)
{
    for (auto & elem : hooks) {
        IHook::Result r = elem.second->datagramLocalInHook(datagram, inIE);
        switch (r) {
            case INetfilter::IHook::ACCEPT:
                break;    // continue iteration

            case INetfilter::IHook::DROP:
                delete datagram;
                return r;

            case INetfilter::IHook::QUEUE: {
                if (datagram->getOwner() != this)
                    throw cRuntimeError("Model error: netfilter hook changed the owner of queued datagram '%s'", datagram->getFullName());
                queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, inIE, nullptr, IPv4Address::UNSPECIFIED_ADDRESS, INetfilter::IHook::LOCALIN));
                return r;
            }

            case INetfilter::IHook::STOLEN:
                return r;

            default:
                throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return INetfilter::IHook::ACCEPT;
}

INetfilter::IHook::Result IPv4::datagramLocalOutHook(Packet *datagram, const InterfaceEntry *& outIE, L3Address& nextHopAddr)
{
    for (auto & elem : hooks) {
        IHook::Result r = elem.second->datagramLocalOutHook(datagram, outIE, nextHopAddr);
        switch (r) {
            case INetfilter::IHook::ACCEPT:
                break;    // continue iteration

            case INetfilter::IHook::DROP:
                delete datagram;
                return r;

            case INetfilter::IHook::QUEUE:
                queuedDatagramsForHooks.push_back(QueuedDatagramForHook(datagram, nullptr, outIE, nextHopAddr.toIPv4(), INetfilter::IHook::LOCALOUT));
                return r;

            case INetfilter::IHook::STOLEN:
                return r;

            default:
                throw cRuntimeError("Unknown Hook::Result value: %d", (int)r);
        }
    }
    return INetfilter::IHook::ACCEPT;
}

void IPv4::receiveSignal(cComponent *source, simsignal_t signalID, cObject *obj DETAILS_ARG)
{
    Enter_Method_Silent();

    if (signalID == IARP::completedARPResolutionSignal) {
        arpResolutionCompleted(check_and_cast<IARP::Notification *>(obj));
    }
    if (signalID == IARP::failedARPResolutionSignal) {
        arpResolutionTimedOut(check_and_cast<IARP::Notification *>(obj));
    }
}

void IPv4::sendIcmpError(Packet *origDatagram, int inputInterfaceId, ICMPType type, ICMPCode code)
{
    icmp->sendErrorMessage(origDatagram, inputInterfaceId, type, code);
}

} // namespace inet

