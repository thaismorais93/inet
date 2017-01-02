//
// Copyright (C) 2004 Andras Varga
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

#include "inet/transportlayer/tcp_common/TCPSegment.h"

namespace inet {

namespace tcp {

Register_Class(Sack);

bool Sack::empty() const
{
    return start == 0 && end == 0;
}

bool Sack::contains(const Sack& other) const
{
    return seqLE(start, other.start) && seqLE(other.end, end);
}

void Sack::clear()
{
    start = end = 0;
}

void Sack::setSegment(unsigned int start_par, unsigned int end_par)
{
    setStart(start_par);
    setEnd(end_par);
}

std::string Sack::str() const
{
    std::stringstream out;

    out << "[" << start << ".." << end << ")";
    return out.str();
}

Register_Class(TcpHeader);

#if 0   //FIXME KLUDGE
void TcpHeader::truncateSegment(uint32 firstSeqNo, uint32 endSeqNo)
{
    ASSERT(payloadLength > 0);

    // must have common part:
#ifndef NDEBUG
    if (!(seqLess(sequenceNo, endSeqNo) && seqLess(firstSeqNo, sequenceNo + payloadLength))) {
        throw cRuntimeError(this, "truncateSegment(%u,%u) called on [%u, %u) segment\n",
                firstSeqNo, endSeqNo, sequenceNo, sequenceNo + payloadLength);
    }
#endif // ifndef NDEBUG

    unsigned int truncleft = 0;
    unsigned int truncright = 0;

    if (seqLess(sequenceNo, firstSeqNo)) {
        truncleft = firstSeqNo - sequenceNo;
    }

    if (seqGreater(sequenceNo + payloadLength, endSeqNo)) {
        truncright = sequenceNo + payloadLength - endSeqNo;
    }

    truncateData(truncleft, truncright);
}
#endif

unsigned short TcpHeader::getHeaderOptionArrayLength()
{
    unsigned short usedLength = 0;

    for (uint i = 0; i < getHeaderOptionArraySize(); i++)
        usedLength += getHeaderOption(i)->getLength();

    return usedLength;
}

TcpHeader& TcpHeader::operator=(const TcpHeader& other)
{
    if (this == &other)
        return *this;
    clean();
    TcpHeader_Base::operator=(other);
    copy(other);
    return *this;
}

void TcpHeader::copy(const TcpHeader& other)
{
    for (const auto opt: other.headerOptionList)
        headerOptionList.push_back(opt->dup());
}

TcpHeader::~TcpHeader()
{
    for (auto opt : headerOptionList)
        delete opt;
}

void TcpHeader::clean()
{
    dropHeaderOptions();
    setHeaderLength(TCP_HEADER_OCTETS);
    setChunkLength(TCP_HEADER_OCTETS);
}

#if 0   //FIXME KLUDGE
void TcpHeader::truncateData(unsigned int truncleft, unsigned int truncright)
{
    ASSERT(payloadLength >= truncleft + truncright);

    if (0 != byteArray.getDataArraySize())
        byteArray.truncateData(truncleft, truncright);

    while (!payloadList.empty() && (payloadList.front().endSequenceNo - sequenceNo) <= truncleft) {
        cPacket *msg = payloadList.front().msg;
        payloadList.pop_front();
        dropAndDelete(msg);
    }

    sequenceNo += truncleft;
    payloadLength -= truncleft + truncright;
    addChunkByteLength(-(truncleft + truncright));

    // truncate payload data correctly
    while (!payloadList.empty() && (payloadList.back().endSequenceNo - sequenceNo) > payloadLength) {
        cPacket *msg = payloadList.back().msg;
        payloadList.pop_back();
        dropAndDelete(msg);
    }
}
#endif

void TcpHeader::parsimPack(cCommBuffer *b) PARSIMPACK_CONST
{
    TcpHeader_Base::parsimPack(b);
    b->pack((int)headerOptionList.size());
    for (const auto opt: headerOptionList) {
        b->packObject(opt);
    }
}

void TcpHeader::parsimUnpack(cCommBuffer *b)
{
    TcpHeader_Base::parsimUnpack(b);
    int i, n;
    b->unpack(n);
    for (i = 0; i < n; i++) {
        TCPOption *opt = check_and_cast<TCPOption*>(b->unpackObject());
        headerOptionList.push_back(opt);
    }
}

void TcpHeader::addHeaderOption(TCPOption *option)
{
    handleChange();
    headerOptionList.push_back(option);
    headerLength += option->getLength();
    setChunkLength(headerLength);
}

void TcpHeader::setHeaderOptionArraySize(unsigned int size)
{
    throw cRuntimeError(this, "setHeaderOptionArraySize() not supported, use addHeaderOption()");
}

unsigned int TcpHeader::getHeaderOptionArraySize() const
{
    return headerOptionList.size();
}

TCPOptionPtr& TcpHeader::getHeaderOption(unsigned int k)
{
    return headerOptionList.at(k);
}

void TcpHeader::setHeaderOption(unsigned int k, const TCPOptionPtr& headerOption)
{
    throw cRuntimeError(this, "setHeaderOption() not supported, use addHeaderOption()");
}

void TcpHeader::dropHeaderOptions()
{
    for (auto opt : headerOptionList)
        delete opt;
    headerOptionList.clear();
    setHeaderLength(TCP_HEADER_OCTETS);
    setChunkLength(TCP_HEADER_OCTETS);
}


} // namespace tcp

} // namespace inet

