//
// Copyright (C) 2005 Michael Tuexen
// Copyright (C) 2008 Irene Ruengeler
// Copyright (C) 2009 Thomas Dreibholz
// Copyright (C) 2009 Thomas Reschka
// Copyright (C) 2011 Zoltan Bojthe
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include <errno.h>

#include "inet/common/packet/PacketDump.h"

#ifdef WITH_UDP
#include "inet/transportlayer/udp/UdpHeader.h"
#endif // ifdef WITH_UDP

#ifdef WITH_SCTP
#include "inet/transportlayer/sctp/SCTPMessage.h"
#include "inet/transportlayer/sctp/SCTPAssociation.h"
#endif // ifdef WITH_SCTP

#ifdef WITH_TCP_COMMON
#include "inet/transportlayer/tcp_common/TCPSegment.h"
#endif // ifdef WITH_TCP_COMMON

#ifdef WITH_IPv4
#include "inet/networklayer/arp/ipv4/ARPPacket_m.h"
#include "inet/networklayer/ipv4/ICMPMessage.h"
#include "inet/networklayer/ipv4/IPv4Header.h"
#endif // ifdef WITH_IPv4

#ifdef WITH_IPv4
#undef WITH_IPv4        //FIXME //KLUDGE
#endif // ifdef WITH_IPv4

#ifdef WITH_IPv6
#include "inet/networklayer/ipv6/IPv6Datagram.h"
#endif // ifdef WITH_IPv6

namespace inet {

PacketDump::PacketDump()
{
    outp = &std::cout;
    verbose = false;
}

PacketDump::~PacketDump()
{
}

void PacketDump::sctpDump(const char *label, sctp::SCTPMessage *sctpmsg,
        const std::string& srcAddr, const std::string& destAddr, const char *comment)
{
    using namespace sctp;

    std::ostream& out = *outp;

    // seq and time (not part of the tcpdump format)
    char buf[30];
    sprintf(buf, "[%.3f%s] ", simTime().dbl(), label);
    out << buf;

#ifndef WITH_SCTP
    out << "[sctp] " << srcAddr << " > " << destAddr;
#else // ifndef WITH_SCTP
    uint32 numberOfChunks;
    SCTPChunk *chunk;
    uint8 type;
    // src/dest
    out << srcAddr << "." << sctpmsg->getSrcPort() << " > "
        << destAddr << "." << sctpmsg->getDestPort() << ": ";

    if (sctpmsg->hasBitError()) {
        sctpmsg->setChecksumOk(false);
    }

    numberOfChunks = sctpmsg->getChunksArraySize();
    out << "numberOfChunks=" << numberOfChunks << " VTag=" << sctpmsg->getTag() << "\n";

    if (sctpmsg->hasBitError())
        out << "Packet has bit error!!\n";

    for (uint32 i = 0; i < numberOfChunks; i++) {
        chunk = (SCTPChunk *)sctpmsg->getChunks(i);
        type = chunk->getChunkType();

        // FIXME create a getChunkTypeName(SCTPChunkType x) function in SCTP code and use it!
        switch (type) {
            case INIT:
                out << "INIT ";
                break;

            case INIT_ACK:
                out << "INIT_ACK ";
                break;

            case COOKIE_ECHO:
                out << "COOKIE_ECHO ";
                break;

            case COOKIE_ACK:
                out << "COOKIE_ACK ";
                break;

            case DATA:
                out << "DATA ";
                break;

            case SACK:
                out << "SACK ";
                break;

            case HEARTBEAT:
                out << "HEARTBEAT ";
                break;

            case HEARTBEAT_ACK:
                out << "HEARTBEAT_ACK ";
                break;

            case ABORT:
                out << "ABORT ";
                break;

            case SHUTDOWN:
                out << "SHUTDOWN ";
                break;

            case SHUTDOWN_ACK:
                out << "SHUTDOWN_ACK ";
                break;

            case SHUTDOWN_COMPLETE:
                out << "SHUTDOWN_COMPLETE ";
                break;

            case ERRORTYPE:
                out << "ERROR";
                break;
        }
    }

    if (verbose) {
        out << endl;

        for (uint32 i = 0; i < numberOfChunks; i++) {
            chunk = (SCTPChunk *)sctpmsg->getChunks(i);
            type = chunk->getChunkType();

            sprintf(buf, "   %3u: ", i + 1);
            out << buf;

            switch (type) {
                case INIT: {
                    SCTPInitChunk *initChunk;
                    initChunk = check_and_cast<SCTPInitChunk *>(chunk);
                    out << "INIT[InitiateTag=";
                    out << initChunk->getInitTag();
                    out << "; a_rwnd=";
                    out << initChunk->getA_rwnd();
                    out << "; OS=";
                    out << initChunk->getNoOutStreams();
                    out << "; IS=";
                    out << initChunk->getNoInStreams();
                    out << "; InitialTSN=";
                    out << initChunk->getInitTSN();

                    if (initChunk->getAddressesArraySize() > 0) {
                        out << "; Addresses=";

                        for (uint32 i = 0; i < initChunk->getAddressesArraySize(); i++) {
                            if (i > 0)
                                out << ",";

                            if (initChunk->getAddresses(i).getType() == L3Address::IPv6)
                                out << initChunk->getAddresses(i).str();
                            else
                                out << initChunk->getAddresses(i);
                        }
                    }

                    out << "]";
                    break;
                }

                case INIT_ACK: {
                    SCTPInitAckChunk *initackChunk;
                    initackChunk = check_and_cast<SCTPInitAckChunk *>(chunk);
                    out << "INIT_ACK[InitiateTag=";
                    out << initackChunk->getInitTag();
                    out << "; a_rwnd=";
                    out << initackChunk->getA_rwnd();
                    out << "; OS=";
                    out << initackChunk->getNoOutStreams();
                    out << "; IS=";
                    out << initackChunk->getNoInStreams();
                    out << "; InitialTSN=";
                    out << initackChunk->getInitTSN();
                    out << "; CookieLength=";
                    out << initackChunk->getCookieArraySize();

                    if (initackChunk->getAddressesArraySize() > 0) {
                        out << "; Addresses=";

                        for (uint32 i = 0; i < initackChunk->getAddressesArraySize(); i++) {
                            if (i > 0)
                                out << ",";

                            out << initackChunk->getAddresses(i);
                        }
                    }

                    out << "]";
                    break;
                }

                case COOKIE_ECHO:
                    out << "COOKIE_ECHO[CookieLength=";
                    out << chunk->getBitLength() / 8 - 4;
                    out << "]";
                    break;

                case COOKIE_ACK:
                    out << "COOKIE_ACK ";
                    break;

                case DATA: {
                    SCTPDataChunk *dataChunk;
                    dataChunk = check_and_cast<SCTPDataChunk *>(chunk);
                    out << "DATA[TSN=";
                    out << dataChunk->getTsn();
                    out << "; SID=";
                    out << dataChunk->getSid();
                    out << "; SSN=";
                    out << dataChunk->getSsn();
                    out << "; PPID=";
                    out << dataChunk->getPpid();
                    out << "; PayloadLength=";
                    out << dataChunk->getBitLength() / 8 - 16;
                    out << "]";
                    break;
                }

                case SACK: {
                    SCTPSackChunk *sackChunk;
                    sackChunk = check_and_cast<SCTPSackChunk *>(chunk);
                    out << "SACK[CumTSNAck=";
                    out << sackChunk->getCumTsnAck();
                    out << "; a_rwnd=";
                    out << sackChunk->getA_rwnd();

                    if (sackChunk->getGapStartArraySize() > 0) {
                        out << "; Gaps=";

                        for (uint32 i = 0; i < sackChunk->getGapStartArraySize(); i++) {
                            if (i > 0)
                                out << ", ";

                            out << sackChunk->getGapStart(i) << "-" << sackChunk->getGapStop(i);
                        }
                    }

                    if (sackChunk->getDupTsnsArraySize() > 0) {
                        out << "; Dups=";

                        for (uint32 i = 0; i < sackChunk->getDupTsnsArraySize(); i++) {
                            if (i > 0)
                                out << ", ";

                            out << sackChunk->getDupTsns(i);
                        }
                    }

                    out << "]";
                    break;
                }

                case HEARTBEAT:
                    SCTPHeartbeatChunk *heartbeatChunk;
                    heartbeatChunk = check_and_cast<SCTPHeartbeatChunk *>(chunk);
                    out << "HEARTBEAT[InfoLength=";
                    out << chunk->getBitLength() / 8 - 4;
                    out << "; time=";
                    out << heartbeatChunk->getTimeField();
                    out << "]";
                    break;

                case HEARTBEAT_ACK:
                    out << "HEARTBEAT_ACK[InfoLength=";
                    out << chunk->getBitLength() / 8 - 4;
                    out << "]";
                    break;

                case ABORT:
                    SCTPAbortChunk *abortChunk;
                    abortChunk = check_and_cast<SCTPAbortChunk *>(chunk);
                    out << "ABORT[T-Bit=";
                    out << abortChunk->getT_Bit();
                    out << "]";
                    break;

                case SHUTDOWN:
                    SCTPShutdownChunk *shutdown;
                    shutdown = check_and_cast<SCTPShutdownChunk *>(chunk);
                    out << "SHUTDOWN[CumTSNAck=";
                    out << shutdown->getCumTsnAck();
                    out << "]";
                    break;

                case SHUTDOWN_ACK:
                    out << "SHUTDOWN_ACK ";
                    break;

                case SHUTDOWN_COMPLETE:
                    out << "SHUTDOWN_COMPLETE ";
                    break;

                case ERRORTYPE: {
                    out << "ERRORTYPE ";
                    SCTPErrorChunk *errorChunk;
                    errorChunk = check_and_cast<SCTPErrorChunk *>(chunk);
                    uint32 numberOfParameters = errorChunk->getParametersArraySize();
                    uint32 parameterType;

                    for (uint32 i = 0; i < numberOfParameters; i++) {
                        SCTPParameter *param = (SCTPParameter *)errorChunk->getParameters(i);
                        parameterType = param->getParameterType();
                        out << parameterType << " ";
                    }
                    break;
                }
            }
            out << endl;
        }
    }
#endif // ifndef WITH_SCTP

    // comment
    if (comment)
        out << "# " << comment;

    out << endl;
}

void PacketDump::dump(const char *label, const char *msg)
{
    std::ostream& out = *outp;

    // seq and time (not part of the tcpdump format)
    char buf[30];

    sprintf(buf, "[%.3f%s] ", simTime().dbl(), label);
    out << buf << msg << endl;
}

void PacketDump::dumpPacket(bool l2r, cPacket *msg)
{
    std::ostream& out = *outp;

#ifdef WITH_IPv4
    if (IPv4Header *ipv4Header = dynamic_cast<IPv4Header *>(msg)) {
        dumpIPv4(l2r, "", ipv4Header, "");
    }
    else if (ARPPacket *arpPacket = dynamic_cast<ARPPacket *>(msg)) {
        dumpARP(l2r, "", arpPacket, "");
    }
    else
#endif // ifdef WITH_IPv4
#ifdef WITH_SCTP
    if (sctp::SCTPMessage *sctpMessage = dynamic_cast<sctp::SCTPMessage *>(msg)) {
        sctpDump("", sctpMessage, std::string(l2r ? "A" : "B"), std::string(l2r ? "B" : "A"));
    }
    else
#endif // ifdef WITH_SCTP
#ifdef WITH_TCP_COMMON
    if (tcp::TcpHeader *tcpseg = dynamic_cast<tcp::TcpHeader *>(msg)) {
        tcpDump(l2r, "", tcpseg, std::string(l2r ? "A" : "B"), std::string(l2r ? "B" : "A"));
    }
    else
#endif // ifdef WITH_TCP_COMMON
#ifdef WITH_IPv4
    if (dynamic_cast<ICMPMessage *>(msg)) {
        out << "ICMPMessage " << msg->getName() << (msg->hasBitError() ? " (BitError)" : "") << endl;
    }
    else
#endif // ifdef WITH_IPv4
    {
        // search for encapsulated IPv4[v6]Datagram in it
        while (msg) {
#ifdef WITH_IPv4
            if (IPv4Header *ipv4Header = dynamic_cast<IPv4Header *>(msg)) {
                dumpIPv4(l2r, "", ipv4Header);
                break;
            }
            else if (ARPPacket *arpPacket = dynamic_cast<ARPPacket *>(msg)) {
                dumpARP(l2r, "", arpPacket, "");
                break;
            }
#endif // ifdef WITH_IPv4
#ifdef WITH_IPv6
            if (IPv6Datagram *ipv6Datagram = dynamic_cast<IPv6Datagram *>(msg)) {
                dumpIPv6(l2r, "", ipv6Datagram);
                break;
            }
#endif // ifdef WITH_IPv6
            out << "Packet " << msg->getClassName() << " '" << msg->getName() << "'"
                << (msg->hasBitError() ? " (BitError)" : "") << ": ";
            msg = msg->getEncapsulatedPacket();
        }

        if (!msg) {
            //We do not want this to end in an error if EtherAutoconf messages
            //are passed, so just print a warning. -WEI
            out << "CANNOT DECODE, packet doesn't contain either IPv4 or IPv6 Datagram\n";
        }
    }
}

void PacketDump::udpDump(bool l2r, const char *label, UdpHeader *udppkt,
        const std::string& srcAddr, const std::string& destAddr, const char *comment)
{
    std::ostream& out = *outp;

    char buf[30];
    sprintf(buf, "[%.3f%s] ", simTime().dbl(), label);
    out << buf;

#ifndef WITH_UDP
    if (l2r)
        out << "[UDP] " << srcAddr << " > " << destAddr << ": ";
    else
        out << "[UDP] " << destAddr << " < " << srcAddr << ": ";
#else // ifndef WITH_UDP
      // seq and time (not part of the tcpdump format)
      // src/dest
    if (l2r) {
        out << srcAddr << "." << udppkt->getSourcePort() << " > ";
        out << destAddr << "." << udppkt->getDestinationPort() << ": ";
    }
    else {
        out << destAddr << "." << udppkt->getDestinationPort() << " < ";
        out << srcAddr << "." << udppkt->getSourcePort() << ": ";
    }

    //out << endl;
    out << "UDP: Payload length=" << udppkt->getTotalLengthField() - UDP_HEADER_BYTES << endl;

#ifdef WITH_SCTP
    if (udppkt->getSourcePort() == 9899 || udppkt->getDestinationPort() == 9899) {
        if (dynamic_cast<sctp::SCTPMessage *>(udppkt->getEncapsulatedPacket()))
            sctpDump("", (sctp::SCTPMessage *)(udppkt->getEncapsulatedPacket()),
                    std::string(l2r ? "A" : "B"), std::string(l2r ? "B" : "A"));
    }
#endif // ifdef WITH_SCTP
#endif // ifndef WITH_UDP

    // comment
    if (comment)
        out << "# " << comment;

    out << endl;
}

void PacketDump::dumpARP(bool l2r, const char *label, ARPPacket *dgram, const char *comment)
{
#ifdef WITH_IPv4
    std::ostream& out = *outp;
    char buf[30];
    sprintf(buf, "[%.3f%s] ", simTime().dbl(), label);
    out << buf << " src: " << dgram->getSrcIPAddress() << ", " << dgram->getSrcMACAddress()
        << "; dest: " << dgram->getDestIPAddress() << ", " << dgram->getDestMACAddress() << endl;
#endif // ifdef WITH_IPv4
}

void PacketDump::dumpIPv4(bool l2r, const char *label, IPv4Header *dgram, const char *comment)
{
    std::ostream& out = *outp;
    char buf[30];
    std::string classes;

#ifdef WITH_IPv4
    cPacket *encapmsg = dgram->getEncapsulatedPacket();

#ifdef WITH_TCP_COMMON
    if (tcp::TcpHeader *tcpseg = dynamic_cast<tcp::TcpHeader *>(encapmsg)) {
        // if TCP, dump as TCP
        tcpDump(l2r, label, tcpseg, dgram->getSrcAddress().str(),
                dgram->getDestAddress().str(), comment);
    }
    else
#endif // ifdef WITH_TCP_COMMON
#ifdef WITH_UDP
    if (dynamic_cast<UdpHeader *>(encapmsg)) {
        udpDump(l2r, label, (UdpHeader *)encapmsg, dgram->getSrcAddress().str(),
                dgram->getDestAddress().str(), comment);
    }
    else
#endif // ifdef WITH_UDP
#ifdef WITH_SCTP
    if (dynamic_cast<sctp::SCTPMessage *>(dgram->getEncapsulatedPacket())) {
        sctp::SCTPMessage *sctpmsg = check_and_cast<sctp::SCTPMessage *>(dgram->getEncapsulatedPacket());
        if (dgram->hasBitError())
            sctpmsg->setBitError(true);
        sctpDump(label, sctpmsg, dgram->getSrcAddress().str(), dgram->getDestAddress().str(), comment);
    }
    else
#endif // ifdef WITH_SCTP
    {
        // some other packet, dump what we can
        // seq and time (not part of the tcpdump format)
        sprintf(buf, "[%.3f%s] ", SIMTIME_DBL(simTime()), label);
        out << buf;
        out << "[IPv4] " << dgram->getSrcAddress() << " > " << dgram->getDestAddress();

        if (dgram->getMoreFragments() || dgram->getFragmentOffset())
            out << ((dgram->getMoreFragments()) ? " inner" : " last") << " fragment from offset " << dgram->getFragmentOffset();

        if (encapmsg) {
            // packet class and name
            out << " ? " << encapmsg->getClassName() << " \"" << encapmsg->getName() << "\"";
        }

        // comment
        if (comment)
            out << " # " << comment;

        out << endl;
    }
#else // ifdef WITH_IPv4
    sprintf(buf, "[%.3f%s] ", SIMTIME_DBL(simTime()), label);
    out << buf << "[IPv4]";

    // comment
    if (comment)
        out << " # " << comment;

    out << endl;
#endif // ifdef WITH_IPv4
}

void PacketDump::dumpIPv6(bool l2r, const char *label, IPv6Datagram *dgram, const char *comment)
{
    using namespace tcp;

    std::ostream& out = *outp;
    char buf[30];

#ifdef WITH_IPv6
    cPacket *encapmsg = dgram->getEncapsulatedPacket();

#ifdef WITH_TCP_COMMON
    if (dynamic_cast<TcpHeader *>(encapmsg)) {
        // if TCP, dump as TCP
        tcpDump(l2r, label, (TcpHeader *)encapmsg, dgram->getSrcAddress().str(),
                dgram->getDestAddress().str(), comment);
    }
    else
#endif // ifdef WITH_TCP_COMMON
    {
        // some other packet, dump what we can
        // seq and time (not part of the tcpdump format)
        sprintf(buf, "[%.3f%s] ", SIMTIME_DBL(simTime()), label);
        out << buf;

        // packet class and name
        out << "? " << encapmsg->getClassName() << " \"" << encapmsg->getName() << "\"\n";

        // comment
        if (comment)
            out << "# " << comment;

        out << endl;
    }
#else // ifdef WITH_IPv6
    sprintf(buf, "[%.3f%s] ", SIMTIME_DBL(simTime()), label);
    out << buf << "[IPv4]";

    // comment
    if (comment)
        out << " # " << comment;

    out << endl;
#endif // ifdef WITH_IPv6
}

void PacketDump::tcpDump(bool l2r, const char *label, tcp::TcpHeader *tcpseg,
        const std::string& srcAddr, const std::string& destAddr, const char *comment)
{
    using namespace tcp;

    std::ostream& out = *outp;

    // seq and time (not part of the tcpdump format)
    char buf[30];
    sprintf(buf, "[%.3f%s] ", SIMTIME_DBL(simTime()), label);
    out << buf;

#ifndef WITH_TCP_COMMON
    if (l2r)
        out << srcAddr << " > " << destAddr << ": ";
    else
        out << destAddr << " < " << srcAddr << ": ";
#else // ifndef WITH_TCP_COMMON
      // src/dest ports
    if (l2r) {
        out << srcAddr << "." << tcpseg->getSrcPort() << " > ";
        out << destAddr << "." << tcpseg->getDestPort() << ": ";
    }
    else {
        out << destAddr << "." << tcpseg->getDestPort() << " < ";
        out << srcAddr << "." << tcpseg->getSrcPort() << ": ";
    }

    // flags
    bool flags = false;
    if (tcpseg->getUrgBit()) {
        flags = true;
        out << "U ";
    }
    if (tcpseg->getAckBit()) {
        flags = true;
        out << "A ";
    }
    if (tcpseg->getPshBit()) {
        flags = true;
        out << "P ";
    }
    if (tcpseg->getRstBit()) {
        flags = true;
        out << "R ";
    }
    if (tcpseg->getSynBit()) {
        flags = true;
        out << "S ";
    }
    if (tcpseg->getFinBit()) {
        flags = true;
        out << "F ";
    }
    if (!flags) {
        out << ". ";
    }

    // data-seqno
    out << "seq " << tcpseg->getSequenceNo();

    // ack
    if (tcpseg->getAckBit())
        out << "ack " << tcpseg->getAckNo() << " ";

    // window
    out << "win " << tcpseg->getWindow() << " ";

    // urgent
    if (tcpseg->getUrgBit())
        out << "urg " << tcpseg->getUrgentPointer() << " ";

    // options present?
    if (tcpseg->getHeaderLength() > 20) {
        const char *direction = l2r ? "sent" : "received";

        if (verbose) {
            unsigned short numOptions = tcpseg->getHeaderOptionArraySize();
            out << "\n  TCP Header Option(s) " << direction << ":\n";

            for (int i = 0; i < numOptions; i++) {
                TCPOption *option = tcpseg->getHeaderOption(i);
                out << "    " << (i + 1) << ". option kind=" << option->getKind() << " length=" << option->getLength() << "\n";
            }
        }
    }
#endif // ifndef WITH_TCP_COMMON

    // comment
    if (comment)
        out << "# " << comment;

    out << endl;
}

} // namespace inet

