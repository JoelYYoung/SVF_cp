//===- SVFIRAdapter.cpp -- SVFIR to abstract-domain symbols ------------===//

#include "AE/Svfexe/SVFIRAdapter.h"

#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFType.h"
#include "SVFIR/SVFVariables.h"
#include "Util/SVFUtil.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace SVF
{

using AbstractDomain::Location;
using AbstractDomain::MemoryLayout;
using AbstractDomain::Variable;

namespace
{

Variable nextVariable(std::uint64_t& next)
{
    if (next > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("too many abstract-domain variables");
    return Variable(static_cast<std::uint32_t>(next++));
}

Location nextLocation(std::uint64_t& next)
{
    if (next > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("too many abstract-domain locations");
    return Location(static_cast<std::uint32_t>(next++));
}

} // namespace

SVFIRAdapter::SVFIRAdapter(const SVFIR& svfir)
{
    std::uint64_t nextVariableId = 1;
    std::uint64_t nextLocationId = 1;
    std::map<Location, Variable> cells;

    for (auto iterator = svfir.begin(); iterator != svfir.end(); ++iterator)
    {
        const SVFVar* svfVariable = iterator->second;
        if (const auto* value = SVFUtil::dyn_cast<ValVar>(svfVariable))
        {
            if (value->isConstDataOrAggDataButNotNullPtr())
                continue;
            if (!value->isPointer() &&
                !SVFUtil::isa<SVFIntegerType>(value->getType()))
                continue;

            const Variable variable = nextVariable(nextVariableId);
            variables_.emplace(value, variable);
            valuesByVariableId_.resize(variable.id() + 1);
            valuesByVariableId_[variable.id()] = value;
            continue;
        }

        const auto* object = SVFUtil::dyn_cast<ObjVar>(svfVariable);
        if (!object)
            continue;
        const Location location = nextLocation(nextLocationId);
        const Variable content = nextVariable(nextVariableId);
        locations_.emplace(object, location);
        objects_.emplace(location, object);
        contentVariables_.emplace(object, content);
        contentObjectsByVariableId_.resize(content.id() + 1);
        contentObjectsByVariableId_[content.id()] = object;
        cells.emplace(location, content);
    }

    memoryLayout_ = MemoryLayout(std::move(cells));
}

bool SVFIRAdapter::contains(const ValVar& value) const
{
    return variables_.count(&value) != 0;
}

bool SVFIRAdapter::contains(const ObjVar& object) const
{
    return locations_.count(&object) != 0;
}

Variable SVFIRAdapter::variable(const ValVar& value) const
{
    const auto iterator = variables_.find(&value);
    if (iterator == variables_.end())
        throw std::invalid_argument("ValVar is not tracked by this adapter");
    return iterator->second;
}

const ValVar* SVFIRAdapter::value(Variable variable) const
{
    return variable.id() < valuesByVariableId_.size()
               ? valuesByVariableId_[variable.id()]
               : nullptr;
}

Location SVFIRAdapter::location(const ObjVar& object) const
{
    const auto iterator = locations_.find(&object);
    if (iterator == locations_.end())
        throw std::invalid_argument("ObjVar is not tracked by this adapter");
    return iterator->second;
}

Variable SVFIRAdapter::contentVariable(const ObjVar& object) const
{
    const auto iterator = contentVariables_.find(&object);
    if (iterator == contentVariables_.end())
        throw std::invalid_argument("ObjVar is not tracked by this adapter");
    return iterator->second;
}

const ObjVar* SVFIRAdapter::contentObject(Variable variable) const
{
    return variable.id() < contentObjectsByVariableId_.size()
               ? contentObjectsByVariableId_[variable.id()]
               : nullptr;
}

bool SVFIRAdapter::isPointer(Variable variable) const
{
    if (const ValVar* scalar = value(variable))
        return scalar->isPointer();
    if (const ObjVar* object = contentObject(variable))
        return object->isPointer();
    throw std::invalid_argument("Variable is not tracked by this adapter");
}

const ObjVar& SVFIRAdapter::object(Location location) const
{
    const auto iterator = objects_.find(location);
    if (iterator == objects_.end())
        throw std::invalid_argument("location is not tracked by this adapter");
    return *iterator->second;
}

} // namespace SVF
