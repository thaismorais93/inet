//
// Copyright (C) 2008 Irene Ruengeler
// Copyright (C) 2009-2015 Thomas Dreibholz
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "inet/applications/sctpapp/SctpClient.h"

#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/lifecycle/NodeStatus.h"
#include "inet/transportlayer/sctp/SctpAssociation.h"
#include "inet/transportlayer/contract/sctp/SctpCommand_m.h"
#include "inet/common/packet/chunk/ByteCountChunk.h"
#include "inet/common/TimeTag_m.h"
#include "inet/common/IProtocolRegistrationListener.h"
#include "inet/common/packet/Message.h"

namespace inet {

#define MSGKIND_CONNECT    0
#define MSGKIND_SEND       1
#define MSGKIND_ABORT      2
#define MSGKIND_PRIMARY    3
#define MSGKIND_RESET      4
#define MSGKIND_STOP       5

Define_Module(SctpClient);

simsignal_t SctpClient::echoedPkSignal = registerSignal("echoedPk");

SctpClient::SctpClient()
{
    timeMsg = nullptr;
    stopTimer = nullptr;
    primaryChangeTimer = nullptr;
    numSessions = 0;
    numBroken = 0;
    packetsSent = 0;
    packetsRcvd = 0;
    bytesSent = 0;
    echoedBytesSent = 0;
    bytesRcvd = 0;
    queueSize = 0;
    outStreams = 1;
    inStreams = 17;
    echo = false;
    ordered = true;
    finishEndsSimulation = false;
    bufferSize = 0;
    timer = false;
    sendAllowed = true;
    numRequestsToSend = 0;    // requests to send in this session
    numPacketsToReceive = 0;
    chunksAbandoned = 0;
}

SctpClient::~SctpClient()
{
    cancelAndDelete(timeMsg);
    cancelAndDelete(stopTimer);
    cancelAndDelete(primaryChangeTimer);
}

void SctpClient::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    EV_DEBUG << "initialize SCTP Client stage " << stage << endl;
    if (stage == INITSTAGE_LOCAL) {
        echo = par("echo");
        ordered = par("ordered");
        finishEndsSimulation = par("finishEndsSimulation");
        queueSize = par("queueSize");
        WATCH(numRequestsToSend);
        recordScalar("ums", (int)par("requestLength"));

        timeMsg = new cMessage("CliAppTimer");
        timeMsg->setKind(MSGKIND_CONNECT);
        scheduleAt(par("startTime"), timeMsg);

        stopTimer = nullptr;
        primaryChangeTimer = nullptr;

        WATCH(numSessions);
        WATCH(numBroken);
        WATCH(packetsSent);
        WATCH(packetsRcvd);
        WATCH(bytesSent);
        WATCH(bytesRcvd);
    }
    else if (stage == INITSTAGE_APPLICATION_LAYER) {
        NodeStatus *nodeStatus = dynamic_cast<NodeStatus *>(findContainingNode(this)->getSubmodule("status"));
        bool isOperational = (!nodeStatus) || nodeStatus->getState() == NodeStatus::UP;
        if (!isOperational)
            throw cRuntimeError("This module doesn't support starting in node DOWN state");

        // parameters
        const char *addressesString = par("localAddress");
        AddressVector addresses = L3AddressResolver().resolve(cStringTokenizer(addressesString).asVector());
        int port = par("localPort");
        socket.setOutputGate(gate("socketOut"));
        socket.setCallbackObject(this);

        if (addresses.size() == 0) {
        EV_INFO << "Call socket bind\n";
            socket.bind(port);
           EV_INFO << "Nach socket bind\n";
        } else {
            socket.bindx(addresses, port);
        }


       // socket.setOutputGate(gate("socketOut"));

        simtime_t stopTime = par("stopTime");
        if (stopTime >= SIMTIME_ZERO) {
            stopTimer = new cMessage("StopTimer");
            stopTimer->setKind(MSGKIND_STOP);
            scheduleAt(par("stopTime"), stopTimer);
            timer = true;
        }

        simtime_t primaryTime = par("primaryTime");
        if (primaryTime != SIMTIME_ZERO) {
            primaryChangeTimer = new cMessage("PrimaryTime");
            primaryChangeTimer->setKind(MSGKIND_PRIMARY);
            scheduleAt(primaryTime, primaryChangeTimer);
        }
    }
}

void SctpClient::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage()) {
        handleTimer(msg);
    }
    else {
        socket.processMessage(msg);
    }
}

void SctpClient::connect()
{
    const char *connectAddress = par("connectAddress");
    int connectPort = par("connectPort");
    inStreams = par("inboundStreams");
    outStreams = par("outboundStreams");
    socket.setInboundStreams(inStreams);
    socket.setOutboundStreams(outStreams);
    EV_INFO << "issuing OPEN command, connect to address " << connectAddress << "\n";
    bool streamReset = par("streamReset");
    L3Address destination;
    L3AddressResolver().tryResolve(connectAddress, destination);
    if (destination.isUnspecified())
        EV << "cannot resolve destination address: " << connectAddress << endl;
    else {
        socket.connect(destination, connectPort, streamReset, (int)par("prMethod"), (unsigned int)par("numRequestsPerSession"));
    }

    if (streamReset) {
        cMessage *cmsg = new cMessage("StreamReset");
        cmsg->setKind(MSGKIND_RESET);
        EV_INFO << "StreamReset Timer scheduled at " << simTime() << "\n";
        scheduleAt(simTime() + par("streamRequestTime"), cmsg);
    }

    for (unsigned int i = 0; i < outStreams; i++) {
        streamRequestLengthMap[i] = par("requestLength");
        streamRequestRatioMap[i] = 1;
        streamRequestRatioSendMap[i] = 1;
    }

    unsigned int streamNum = 0;
    cStringTokenizer ratioTokenizer(par("streamRequestRatio"));
    while (ratioTokenizer.hasMoreTokens()) {
        const char *token = ratioTokenizer.nextToken();
        streamRequestRatioMap[streamNum] = atoi(token);
        streamRequestRatioSendMap[streamNum] = atoi(token);

        streamNum++;
    }

    numSessions++;
}

void SctpClient::close()
{
    socket.close();
}

void SctpClient::refreshDisplay() const
{
    getDisplayString().setTagArg("t", 0, SctpSocket::stateName(socket.getState()));
}

void SctpClient::socketEstablished(int, void *, unsigned long int buffer)
{
    int count = 0;
    EV_INFO << "SctpClient: connected\n";
    bufferSize = buffer;
    // determine number of requests in this session
    numRequestsToSend = par("numRequestsPerSession");
    numPacketsToReceive = par("numPacketsToReceive");

    if (numRequestsToSend < 1)
        numRequestsToSend = 0;

    EV_INFO << "SctpClient:numRequestsToSend=" << numRequestsToSend << "\n";

    // perform first request (next one will be sent when reply arrives)
    if ((numRequestsToSend > 0 && !timer) || timer) {
        if (par("thinkTime").doubleValue() > 0) {
            if (sendAllowed) {
                sendRequest();

                if (!timer)
                    numRequestsToSend--;
            }

            timeMsg->setKind(MSGKIND_SEND);
            scheduleAt(simTime() + par("thinkTime"), timeMsg);
        }
        else {
            if (queueSize > 0) {
                while (((!timer && numRequestsToSend > 0) || timer) && count++ < queueSize * 2 && sendAllowed) {
                    if (count == queueSize * 2)
                        sendRequest();
                    else
                        sendRequest(false);

                    if (!timer) {
                        if (--numRequestsToSend == 0)
                            sendAllowed = false;
                    }
                }

                if (((!timer && numRequestsToSend > 0) || timer) && sendAllowed)
                    sendQueueRequest();
            }
            else {
                while ((((!timer && numRequestsToSend > 0) || timer) && sendAllowed && bufferSize > 0) ||
                       (((!timer && numRequestsToSend > 0) || timer) && sendAllowed && buffer == 0))
                {
                    if (!timer && numRequestsToSend == 1)
                        sendRequest(true);
                    else
                        sendRequest(false);

                    if (!timer && (--numRequestsToSend == 0))
                        sendAllowed = false;
                }
            }
        }

        if ((!timer && numPacketsToReceive == 0) && par("waitToClose").doubleValue() > 0) {
            timeMsg->setKind(MSGKIND_ABORT);
            scheduleAt(simTime() + par("waitToClose"), timeMsg);
        }

        if ((!timer && numRequestsToSend == 0) && par("waitToClose").doubleValue() == 0) {
            EV_INFO << "socketEstablished:no more packets to send, call shutdown\n";
            socket.shutdown();

            if (timeMsg->isScheduled())
                cancelEvent(timeMsg);

            if (finishEndsSimulation) {
                endSimulation();
            }
        }
    }
}

void SctpClient::sendQueueRequest()
{
    Request *cmsg = new Request("SCTP_C_QUEUE_MSGS_LIMIT");
    auto& tags = getTags(cmsg);
    SctpInfoReq *qinfo = tags.addTagIfAbsent<SctpInfoReq>();
    qinfo->setText(queueSize);
    cmsg->setKind(SCTP_C_QUEUE_MSGS_LIMIT);
    qinfo->setSocketId(socket.getConnectionId());
    EV_INFO << "Sending queue request ..." << endl;
    socket.sendRequest(cmsg);
}

void SctpClient::sendRequestArrived()
{
    int count = 0;

    EV_INFO << "sendRequestArrived numRequestsToSend=" << numRequestsToSend << "\n";

    while (((!timer && numRequestsToSend > 0) || timer) && count++ < queueSize && sendAllowed) {
        sendRequest(count == queueSize);

        if (!timer)
            numRequestsToSend--;

        if ((!timer && numRequestsToSend == 0)) {
            EV_INFO << "no more packets to send, call shutdown\n";
            socket.shutdown();

            if (timeMsg->isScheduled())
                cancelEvent(timeMsg);

            if (finishEndsSimulation)
                endSimulation();
        }
    }
}

void SctpClient::socketDataArrived(int, void *, Packet *msg, bool)
{
    packetsRcvd++;

    EV_INFO << "Client received packet Nr " << packetsRcvd << " from SCTP\n";
    SctpCommandReq *ind = msg->getTag<SctpCommandReq>();
    emit(packetReceivedSignal, msg);
    bytesRcvd += msg->getByteLength();

    if (echo) {

       // auto& smsg = msg->peekDataAt(B(0), B(msg->getByteLength()));

        SctpSimpleMessage *smsg = check_and_cast<SctpSimpleMessage *>(msg);
        cPacket *cmsg = new cPacket("SCTP_C_SEND");
        echoedBytesSent += smsg->getByteLength();
        emit(echoedPkSignal, msg);
        cmsg->encapsulate(smsg);
       // cmsg->insertAtEnd(smsg);
        cmsg->setKind(ind->getSendUnordered() ? SCTP_C_SEND_UNORDERED : SCTP_C_SEND_ORDERED);
        packetsSent++;
        socket.sendMsg(cmsg);
    }

    if (par("numPacketsToReceive").intValue() > 0) {
        numPacketsToReceive--;
        if (numPacketsToReceive == 0)
            close();
    }

    delete ind;
}

void SctpClient::sendRequest(bool last)
{
    // find next stream
    unsigned int nextStream = 0;
    for (unsigned int i = 0; i < outStreams; i++) {
        if (streamRequestRatioSendMap[i] > streamRequestRatioSendMap[nextStream])
            nextStream = i;
    }

    // no stream left, reset map
    if (nextStream == 0 && streamRequestRatioSendMap[nextStream] == 0) {
        for (unsigned int i = 0; i < outStreams; i++) {
            streamRequestRatioSendMap[i] = streamRequestRatioMap[i];
            if (streamRequestRatioSendMap[i] > streamRequestRatioSendMap[nextStream])
                nextStream = i;
        }
    }

    if (nextStream == 0 && streamRequestRatioSendMap[nextStream] == 0) {
        throw cRuntimeError("Invalid setting of streamRequestRatio: only 0 weightings");
    }

    unsigned int sendBytes = streamRequestLengthMap[nextStream];
    streamRequestRatioSendMap[nextStream]--;

    if (sendBytes < 1)
        sendBytes = 1;

    auto applicationPacket = new Packet("ApplicationPacket");
    auto applicationData = makeShared<BytesChunk>();
    std::vector<uint8_t> vec;
    vec.resize(sendBytes);
    for (int i = 0; i < sendBytes; i++)
        vec[i] = (bytesSent + i) & 0xFF;
    applicationData->setBytes(vec);
    applicationPacket->insertAtEnd(applicationData);
   // applicationPacket->setDataLength(B(sendBytes));
    auto sctpSendReq = applicationPacket->addTagIfAbsent<SctpSendReq>();
    sctpSendReq->setLast(last);
    sctpSendReq->setPrMethod(par("prMethod"));
    sctpSendReq->setPrValue(par("prValue"));
    sctpSendReq->setSid(nextStream);
    auto creationTimeTag = applicationData->addTag<CreationTimeTag>();
    creationTimeTag->setCreationTime(simTime());
    std::cout << "CreationTimeTag : " << creationTimeTag->getCreationTime() << endl;
    applicationPacket->setKind(ordered ? SCTP_C_SEND_ORDERED : SCTP_C_SEND_UNORDERED);
  //  emit(packetSentSignal, msg);
    std::cout << "Call socket.sendMsg\n";
    socket.sendMsg(applicationPacket);
    std::cout << "socket.sendMsg returned\n";

    bytesSent += sendBytes;
}

void SctpClient::handleTimer(cMessage *msg)
{
    switch (msg->getKind()) {
        case MSGKIND_CONNECT:
            EV_DEBUG << "starting session call connect\n";
            connect();
            break;

        case MSGKIND_SEND:
            if (((!timer && numRequestsToSend > 0) || timer)) {
                if (sendAllowed) {
                    sendRequest();
                    if (!timer)
                        numRequestsToSend--;
                }
                if (par("thinkTime").doubleValue() > 0)
                    scheduleAt(simTime() + par("thinkTime"), timeMsg);

                if ((!timer && numRequestsToSend == 0) && par("waitToClose").doubleValue() == 0) {
                    socket.shutdown();
                    if (timeMsg->isScheduled())
                        cancelEvent(timeMsg);

                    if (finishEndsSimulation) {
                        endSimulation();
                    }
                }
            }
            else if ((!timer && numRequestsToSend == 0) && par("waitToClose").doubleValue() == 0) {
                socket.shutdown();
                if (timeMsg->isScheduled())
                    cancelEvent(timeMsg);

                if (finishEndsSimulation) {
                    endSimulation();
                }
            }
            break;

        case MSGKIND_ABORT:
            close();
            break;

        case MSGKIND_PRIMARY:
            setPrimaryPath(par("newPrimary"));
            break;

        case MSGKIND_RESET:
            EV_INFO << "StreamReset Timer expired at Client at " << simTime() << "...send notification\n";
            sendStreamResetNotification();
            delete msg;
            break;

        case MSGKIND_STOP:
            numRequestsToSend = 0;
            sendAllowed = false;
            socket.abort();
            socket.close();

            if (timeMsg->isScheduled())
                cancelEvent(timeMsg);

            if (finishEndsSimulation) {
                endSimulation();
            }
            break;

        default:
            throw cRuntimeError("unknown selfmessage kind = %d ", msg->getKind());
            break;
    }
}

void SctpClient::socketDataNotificationArrived(int connId, void *ptr, Packet *msg)
{
 std::cout << "***********To Do: socketDataNotificationArrived********\n";
 /*   SctpCommand *ind = check_and_cast<SctpCommand *>(msg->removeControlInfo());
    cMessage *cmsg = new cMessage("SCTP_C_RECEIVE");
    SctpSendInfo *cmd = new SctpSendInfo();
    cmd->setSocketId(ind->getSocketId());
    cmd->setSid(ind->getSid());
    cmd->setNumMsgs(ind->getNumMsgs());
    cmsg->setKind(SCTP_C_RECEIVE);
    cmsg->setControlInfo(cmd);
    delete ind;
    socket.sendNotification(cmsg);*/
}

void SctpClient::shutdownReceivedArrived(int connId)
{
    if (numRequestsToSend == 0) {
        Message *cmsg = new Message("SCTP_C_NO_OUTSTANDING");
        auto& tags = getTags(cmsg);
        SctpCommandReq *qinfo = tags.addTagIfAbsent<SctpCommandReq>();
        cmsg->setKind(SCTP_C_NO_OUTSTANDING);
        qinfo->setSocketId(connId);
        socket.sendNotification(cmsg);
    }
}

void SctpClient::socketPeerClosed(int, void *)
{
    // close the connection (if not already closed)
    if (socket.getState() == SctpSocket::PEER_CLOSED) {
        EV_INFO << "remote SCTP closed, closing here as well\n";
        close();
    }
}

void SctpClient::socketClosed(int, void *)
{
    // *redefine* to start another session etc.
    EV_INFO << "connection closed\n";

    if (primaryChangeTimer) {
        cancelEvent(primaryChangeTimer);
        delete primaryChangeTimer;
        primaryChangeTimer = nullptr;
    }
}

void SctpClient::socketFailure(int, void *, int code)
{
    // subclasses may override this function, and add code try to reconnect after a delay.
    EV_WARN << "connection broken\n";
    numBroken++;
    // reconnect after a delay
    timeMsg->setKind(MSGKIND_CONNECT);
    scheduleAt(simTime() + par("reconnectInterval"), timeMsg);
}

void SctpClient::socketStatusArrived(int assocId, void *yourPtr, SctpStatusReq *status)
{
    struct PathStatus ps;
    auto i = sctpPathStatus.find(status->getPathId());

    if (i != sctpPathStatus.end()) {
        ps = i->second;
        ps.active = status->getActive();
    }
    else {
        ps.active = status->getActive();
        ps.pid = status->getPathId();
        ps.primaryPath = false;
        sctpPathStatus[ps.pid] = ps;
    }
    delete status;
}

void SctpClient::setPrimaryPath(const char *str)
{
    Request *cmsg = new Request("SCTP_C_PRIMARY");
    auto& tags = getTags(cmsg);

    SctpPathInfoReq *pinfo = tags.addTagIfAbsent<SctpPathInfoReq>();

    if (strcmp(str, "") != 0) {
        pinfo->setRemoteAddress(L3Address(str));
    }
    else {
        str = par("newPrimary");
        if (strcmp(str, "") != 0)
            pinfo->setRemoteAddress(L3Address(str));
        else {
            str = par("connectAddress");
            pinfo->setRemoteAddress(L3Address(str));
        }
    }

    pinfo->setSocketId(socket.getConnectionId());
    cmsg->setKind(SCTP_C_PRIMARY);
    socket.sendNotification(cmsg);
}

void SctpClient::sendStreamResetNotification()
{
    unsigned int type = par("streamResetType");
    if (type >= 6 && type <= 9) {
        Message *cmsg = new Message("SCTP_C_STREAM_RESET");
        auto& tags = getTags(cmsg);
        SctpResetReq *rinfo = tags.addTagIfAbsent<SctpResetReq>();
        rinfo->setSocketId(socket.getConnectionId());
        rinfo->setRemoteAddr(socket.getRemoteAddr());
        rinfo->setRequestType((unsigned short int)type);
        cmsg->setKind(SCTP_C_STREAM_RESET);
        socket.sendNotification(cmsg);
    }
}

void SctpClient::msgAbandonedArrived(int assocId)
{
    chunksAbandoned++;
}

void SctpClient::sendqueueAbatedArrived(int assocId, unsigned long int buffer)
{
    bufferSize = buffer;
    sendAllowed = true;

    while ((((!timer && numRequestsToSend > 0) || timer) && sendAllowed && bufferSize > 0) ||
           (((!timer && numRequestsToSend > 0) || timer) && sendAllowed && buffer == 0))
    {
        if (!timer && numRequestsToSend == 1)
            sendRequest(true);
        else
            sendRequest(false);

        if (!timer && (--numRequestsToSend == 0))
            sendAllowed = false;
    }

    if ((!timer && numRequestsToSend == 0) && par("waitToClose").doubleValue() == 0) {
        EV_INFO << "socketEstablished:no more packets to send, call shutdown\n";
        socket.shutdown();

        if (timeMsg->isScheduled())
            cancelEvent(timeMsg);

        if (finishEndsSimulation) {
            endSimulation();
        }
    }
}

void SctpClient::finish()
{
    EV_INFO << getFullPath() << ": opened " << numSessions << " sessions\n";
    EV_INFO << getFullPath() << ": sent " << bytesSent << " bytes in " << packetsSent << " packets\n";
    EV_INFO << getFullPath() << ": received " << bytesRcvd << " bytes in " << packetsRcvd << " packets\n";
    EV_INFO << "Client finished\n";
}

} // namespace inet
