//
// Copyright (C) 2019 Marcel Marek
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
// 

package inet.queueing.marker;

import inet.queueing.filter.RedDropper;

//
// Random early detection (RED) marker which sets the ECN bits in the IP header.
//
simple RedMarker extends RedDropper
{
    parameters:
        @class(RedMarker);
        useEcn = true;
        packetCapacity = default(120);
}

