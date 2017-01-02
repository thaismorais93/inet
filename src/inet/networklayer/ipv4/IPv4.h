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

#ifndef __INET_IPV4_H
#define __INET_IPV4_H

#include "inet/common/INETDefs.h"

#include "inet/common/IProtocolRegistrationListener.h"
#include "inet/networklayer/contract/IARP.h"
#include "inet/networklayer/ipv4/ICMP.h"
#include "inet/common/lifecycle/ILifecycle.h"
#include "inet/networklayer/contract/INetfilter.h"
#include "inet/networklayer/contract/INetworkProtocol.h"
#include "inet/networklayer/ipv4/IPv4Header.h"
#include "inet/networklayer/ipv4/IPv4FragBuf.h"
#include "inet/common/ProtocolMap.h"
#include "inet/common/queue/QueueBase.h"

namespace inet {

class ARPPacket;
class ICMPMessage;
class IInterfaceTable;
class IIPv4RoutingTable;

/**
 * Implements the IPv4 protocol.
 */
class INET_API IPv4 : public QueueBase, public NetfilterBase, public ILifecycle, public INetworkProtocol, public IProtocolRegistrationListener, public cListener
{
  public:
    /**
     * Represents an IPv4Header, queued by a Hook
     */
    class QueuedDatagramForHook
    {
      public:
        QueuedDatagramForHook(Packet *datagram, const InterfaceEntry *inIE, const InterfaceEntry *outIE, const IPv4Address& nextHopAddr, IHook::Type hookType) :
            datagram(datagram), inIE(inIE), outIE(outIE), nextHopAddr(nextHopAddr), hookType(hookType) {}
        virtual ~QueuedDatagramForHook() {}

        Packet *datagram = nullptr;
        const InterfaceEntry *inIE = nullptr;
        const InterfaceEntry *outIE = nullptr;
        IPv4Address nextHopAddr;
        const IHook::Type hookType = (IHook::Type)-1;
    };
    typedef std::map<IPv4Address, cPacketQueue> PendingPackets;

    struct SocketDescriptor
    {
        int socketId = -1;
        int protocolId = -1;

        SocketDescriptor(int socketId, int protocolId) : socketId(socketId), protocolId(protocolId) { }
    };

  protected:
    IIPv4RoutingTable *rt = nullptr;
    IInterfaceTable *ift = nullptr;
    IARP *arp = nullptr;
    ICMP *icmp = nullptr;
    int transportInGateBaseId = -1;

    // config
    int defaultTimeToLive = -1;
    int defaultMCTimeToLive = -1;
    simtime_t fragmentTimeoutTime;
    bool forceBroadcast = false;
    bool useProxyARP = false;

    // working vars
    bool isUp = false;
    long curFragmentId = -1;    // counter, used to assign unique fragmentIds to datagrams
    IPv4FragBuf fragbuf;    // fragmentation reassembly buffer
    simtime_t lastCheckTime;    // when fragbuf was last checked for state fragments
    ProtocolMapping mapping;    // where to send packets after decapsulation
    std::map<int, SocketDescriptor *> socketIdToSocketDescriptor;
    std::multimap<int, SocketDescriptor *> protocolIdToSocketDescriptors;

    // ARP related
    PendingPackets pendingPackets;    // map indexed with IPv4Address for outbound packets waiting for ARP resolution

    // statistics
    int numMulticast = 0;
    int numLocalDeliver = 0;
    int numDropped = 0;    // forwarding off, no outgoing interface, too large but "don't fragment" is set, TTL exceeded, etc
    int numUnroutable = 0;
    int numForwarded = 0;

    // hooks
    typedef std::list<QueuedDatagramForHook> DatagramQueueForHooks;
    DatagramQueueForHooks queuedDatagramsForHooks;

  protected:
    // utility: look up interface from getArrivalGate()
    virtual const InterfaceEntry *getSourceInterfaceFrom(cPacket *packet);

    // utility: look up route to the source of the datagram and return its interface
    virtual const InterfaceEntry *getShortestPathInterfaceToSource(IPv4Header *datagram);

    // utility: show current statistics above the icon
    virtual void refreshDisplay() const override;

    // utility: processing requested ARP resolution completed
    void arpResolutionCompleted(IARP::Notification *entry);

    // utility: processing requested ARP resolution timed out
    void arpResolutionTimedOut(IARP::Notification *entry);

    /**
     * Encapsulate packet coming from higher layers into IPv4Header, using
     * the given control info. Override if you subclassed controlInfo and/or
     * want to add options etc to the datagram.
     */
    virtual std::shared_ptr<IPv4Header> encapsulate(Packet *transportPacket);

    /**
     * Handle IPv4Header messages arriving from lower layer.
     * Decrements TTL, then invokes routePacket().
     */
    virtual void handleIncomingDatagram(Packet *packet, const InterfaceEntry *fromIE);

    // called after PREROUTING Hook (used for reinject, too)
    virtual void preroutingFinish(Packet *packet, const InterfaceEntry *fromIE, const InterfaceEntry *destIE, IPv4Address nextHopAddr);

    /**
     * Handle messages (typically packets to be send in IPv4) from transport or ICMP.
     * Invokes encapsulate(), then routePacket().
     */
    virtual void handlePacketFromHL(Packet *packet);

    /**
     * Routes and sends datagram received from higher layers.
     * Invokes datagramLocalOutHook(), then routePacket().
     */
    virtual void datagramLocalOut(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *destIE, IPv4Address nextHopAddr);

    /**
     * Performs unicast routing. Based on the routing decision, it sends the
     * datagram through the outgoing interface.
     */
    virtual void routeUnicastPacket(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *fromIE, const InterfaceEntry *destIE, IPv4Address requestedNextHopAddress);

    // called after FORWARD Hook (used for reinject, too)
    void routeUnicastPacketFinish(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *fromIE, const InterfaceEntry *destIE, IPv4Address nextHopAddr);

    /**
     * Broadcasts the datagram on the specified interface.
     * When destIE is nullptr, the datagram is broadcasted on each interface.
     */
    virtual void routeLocalBroadcastPacket(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *destIE);

    /**
     * Determines the output interface for the given multicast datagram.
     */
    virtual const InterfaceEntry *determineOutgoingInterfaceForMulticastDatagram(const IPv4Header *datagram, const InterfaceEntry *multicastIFOption);

    /**
     * Forwards packets to all multicast destinations, using fragmentAndSend().
     */
    virtual void forwardMulticastPacket(Packet *packet, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *fromIE);

    /**
     * Perform reassembly of fragmented datagrams, then send them up to the
     * higher layers using sendToHL().
     */
    virtual void reassembleAndDeliver(Packet *packet, const InterfaceEntry *fromIE);

    // called after LOCAL_IN Hook (used for reinject, too)
    virtual void reassembleAndDeliverFinish(Packet *packet, const InterfaceEntry *fromIE);

    /**
     * Decapsulate and return encapsulated packet after attaching IPv4ControlInfo.
     */
    virtual void decapsulate(Packet *packet);

    /**
     * Call PostRouting Hook and continue with fragmentAndSend() if accepted
     */
    virtual void fragmentPostRouting(Packet *datagram, const std::shared_ptr<IPv4Header>& ipv4Header, const InterfaceEntry *destIe, IPv4Address nextHopAddr);

    /**
     * Fragment packet if needed, then send it to the selected interface using
     * sendDatagramToOutput().
     */
    virtual void fragmentAndSend(Packet *packet, const std::shared_ptr<IPv4Header>&, const InterfaceEntry *destIe, IPv4Address nextHopAddr);

    /**
     * Send datagram on the given interface.
     */
    virtual void sendDatagramToOutput(Packet *packet, const InterfaceEntry *ie, IPv4Address nextHopAddr);

    virtual MACAddress resolveNextHopMacAddress(cPacket *packet, IPv4Address nextHopAddr, const InterfaceEntry *destIE);

    virtual void sendPacketToIeee802NIC(cPacket *packet, const InterfaceEntry *ie, const MACAddress& macAddress, int etherType);

    virtual void sendPacketToNIC(cPacket *packet, const InterfaceEntry *ie);

    virtual void sendIcmpError(Packet *packet, int inputInterfaceId, ICMPType type, ICMPCode code);

  public:
    IPv4();
    virtual ~IPv4();

    virtual void handleRegisterProtocol(const Protocol& protocol, cGate *gate) override;

  protected:
    virtual int numInitStages() const override { return NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
    virtual void handleMessage(cMessage *msg) override;

    /**
     * Processing of IPv4 datagrams. Called when a datagram reaches the front
     * of the queue.
     */
    virtual void endService(cPacket *packet) override;

    // NetFilter functions:

  protected:
    /**
     * called before a packet arriving from the network is routed
     */
    IHook::Result datagramPreRoutingHook(Packet *datagram, const InterfaceEntry *inIE, const InterfaceEntry *& outIE, L3Address& nextHopAddr);

    /**
     * called before a packet arriving from the network is delivered via the network
     */
    IHook::Result datagramForwardHook(Packet *datagram, const InterfaceEntry *inIE, const InterfaceEntry *& outIE, L3Address& nextHopAddr);

    /**
     * called before a packet is delivered via the network
     */
    IHook::Result datagramPostRoutingHook(Packet *datagram, const InterfaceEntry *inIE, const InterfaceEntry *& outIE, L3Address& nextHopAddr);

    /**
     * called before a packet arriving from the network is delivered locally
     */
    IHook::Result datagramLocalInHook(Packet *datagram, const InterfaceEntry *inIE);

    /**
     * called before a packet arriving locally is delivered
     */
    IHook::Result datagramLocalOutHook(Packet *datagram, const InterfaceEntry *& outIE, L3Address& nextHopAddr);

  public:
    /**
     * registers a Hook to be executed during datagram processing
     */
    virtual void registerHook(int priority, IHook *hook) override;

    /**
     * unregisters a Hook to be executed during datagram processing
     */
    virtual void unregisterHook(IHook *hook) override;

    /**
     * drop a previously queued datagram
     */
    virtual void dropQueuedDatagram(const Packet *datagram) override;

    /**
     * re-injects a previously queued datagram
     */
    virtual void reinjectQueuedDatagram(const Packet *datagram) override;

    /**
     * ILifecycle method
     */
    virtual bool handleOperationStage(LifecycleOperation *operation, int stage, IDoneCallback *doneCallback) override;

    /// cListener method
    virtual void receiveSignal(cComponent *source, simsignal_t signalID, cObject *obj DETAILS_ARG) override;

  protected:
    virtual bool isNodeUp();
    virtual void stop();
    virtual void start();
    virtual void flush();
};

} // namespace inet

#endif // ifndef __INET_IPV4_H

