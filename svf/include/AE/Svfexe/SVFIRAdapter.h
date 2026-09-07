//===- SVFIRAdapter.h -- SVFIR to abstract-domain symbols ----*- C++ -*-===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//
// Contributors: Xiao Cheng, Jiawei Wang
//
//===----------------------------------------------------------------------===//

#ifndef SVF_AE_SVFIR_ADAPTER_H
#define SVF_AE_SVFIR_ADAPTER_H

#include "AE/Core/BoxAddressDomain.h"

#include <cstdint>
#include <map>
#include <vector>

namespace SVF
{

class FunObjVar;
class ObjVar;
class SVFIR;
class ValVar;

/// Owns the IR-specific identity mapping. Abstract-domain states only see
/// Variable and Location; they never depend on SVF NodeID or SVFIR classes.
class SVFIRAdapter
{
public:
    explicit SVFIRAdapter(const SVFIR& svfir);

    bool contains(const ValVar& value) const;
    bool contains(const ObjVar& object) const;

    AbstractDomain::Variable variable(const ValVar& value) const;
    const ValVar* value(AbstractDomain::Variable variable) const;
    AbstractDomain::Location location(const ObjVar& object) const;
    AbstractDomain::Variable contentVariable(const ObjVar& object) const;
    const ObjVar* contentObject(AbstractDomain::Variable variable) const;
    bool isPointer(AbstractDomain::Variable variable) const;
    const ObjVar& object(AbstractDomain::Location location) const;
    AbstractDomain::Variable firstObjectContentVariable() const
    {
        return AbstractDomain::Variable(firstObjectContentVariableId_);
    }

    const AbstractDomain::MemoryLayout& memoryLayout() const
    {
        return memoryLayout_;
    }

private:
    void registerObject(const ObjVar& object) const;

    std::map<const ValVar*, AbstractDomain::Variable> variables_;
    std::vector<const ValVar*> valuesByVariableId_;
    mutable std::map<const ObjVar*, AbstractDomain::Location> locations_;
    mutable std::map<AbstractDomain::Location, const ObjVar*> objects_;
    mutable std::map<const ObjVar*, AbstractDomain::Variable>
    contentVariables_;
    mutable std::vector<const ObjVar*> contentObjectsByVariableId_;
    mutable AbstractDomain::MemoryLayout memoryLayout_;
    mutable std::uint64_t nextVariableId_ = 1;
    mutable std::uint64_t nextLocationId_ = 1;
    std::uint32_t firstObjectContentVariableId_ = 1;
};

} // namespace SVF

#endif // SVF_AE_SVFIR_ADAPTER_H
