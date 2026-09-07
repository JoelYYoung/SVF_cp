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
    auto addScalars = [&](bool pointers) {
        for (auto iterator = svfir.begin(); iterator != svfir.end(); ++iterator)
        {
            const SVFVar* svfVariable = iterator->second;
            const auto* value = SVFUtil::dyn_cast<ValVar>(svfVariable);
            if (!value || value->isPointer() != pointers)
                continue;
            if (value->isConstDataOrAggDataButNotNullPtr())
                continue;
            if (!pointers && !SVFUtil::isa<SVFIntegerType>(value->getType()))
                continue;

            const Variable variable = nextVariable(nextVariableId_);
            variables_.emplace(value, variable);
            valuesByVariableId_.resize(variable.id() + 1);
            valuesByVariableId_[variable.id()] = value;
        }
    };

    auto addObjectContents = [&](bool pointers) {
        for (auto iterator = svfir.begin(); iterator != svfir.end(); ++iterator)
        {
            const SVFVar* svfVariable = iterator->second;
            const auto* object = SVFUtil::dyn_cast<ObjVar>(svfVariable);
            if (!object || object->isPointer() != pointers)
                continue;

            registerObject(*object);
        }
    };

    // One stable analysis-wide ID space doubles as the sparse storage
    // coordinate. Pack it by Semi-Sparse carrier first and domain second:
    //
    //   numerical ValVars | pointer ValVars | numerical contents | pointer
    //   contents
    //
    // Every scalar/flow domain slice is a contiguous range. Dense states use
    // two compact ranges per domain; the sparse page directory does not
    // allocate their intervening range. Keeping all ValVars before contents
    // also keeps both O(1) reverse-index vectors compact.
    addScalars(false);
    addScalars(true);
    addObjectContents(false);
    addObjectContents(true);
}

void SVFIRAdapter::registerObject(const ObjVar& object) const
{
    if (locations_.count(&object) != 0)
        return;

    const Location location = nextLocation(nextLocationId_);
    const Variable content = nextVariable(nextVariableId_);
    locations_.emplace(&object, location);
    objects_.emplace(location, &object);
    contentVariables_.emplace(&object, content);
    contentObjectsByVariableId_.resize(content.id() + 1);
    contentObjectsByVariableId_[content.id()] = &object;
    memoryLayout_.extend(location, content);
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
    registerObject(object);
    const auto iterator = locations_.find(&object);
    return iterator->second;
}

Variable SVFIRAdapter::contentVariable(const ObjVar& object) const
{
    registerObject(object);
    const auto iterator = contentVariables_.find(&object);
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
