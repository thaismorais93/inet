//
// Copyright (C) OpenSim Ltd.
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

#ifndef __INET_PHYSICALENVIRONMENTVISUALIZERBASE_H
#define __INET_PHYSICALENVIRONMENTVISUALIZERBASE_H

#include "inet/environment/contract/IPhysicalEnvironment.h"
#include "inet/visualizer/base/VisualizerBase.h"

namespace inet {

namespace visualizer {

using namespace inet::physicalenvironment;

class INET_API PhysicalEnvironmentVisualizerBase : public VisualizerBase
{
  protected:
    /** @name Parameters */
    //@{
    const IPhysicalEnvironment *physicalEnvironment = nullptr;
    //@}

  protected:
    virtual void initialize(int stage) override;
};

} // namespace visualizer

} // namespace inet

#endif // ifndef __INET_PHYSICALENVIRONMENTVISUALIZERBASE_H

