//===- AbstractStateManager.cpp -- AE domain projection helpers --------===//

#include "AE/Svfexe/AbstractInterpretation.h"

#include "SVFIR/SVFIR.h"

#include <algorithm>

namespace SVF
{

namespace AD = AbstractDomain;

namespace
{

s64_t finiteEndpoint(const AD::Bound& bound, s64_t fallback)
{
    if (!bound.isFinite())
        return fallback;
    try
    {
        return bound.value().toInt64();
    }
    catch (const std::exception&)
    {
        return fallback;
    }
}

AD::Interval finiteInterval(s64_t lower, s64_t upper)
{
    return AD::Interval::closed(AD::Rational(lower), AD::Rational(upper));
}

bool constantInterval(const ValVar* value, AD::Interval& interval)
{
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(value))
    {
        interval =
            AD::Interval::singleton(AD::Rational(integer->getSExtValue()));
        return true;
    }
    if (const auto* floating = SVFUtil::dyn_cast<ConstFPValVar>(value))
    {
        interval = AD::Interval::singleton(
            AD::Rational::fromDouble(floating->getFPValue()));
        return true;
    }
    return false;
}

} // namespace

const AD::AbstractDomain* AbstractInterpretation::getScalarAbstractState() const
{
    return nullptr;
}

void AbstractInterpretation::finalizeAbstractState(const ICFGNode*) {}

void AbstractInterpretation::updateInterval(const SVFVar* variable,
                                            const AD::Interval& interval,
                                            const ICFGNode* node)
{
    updateValue(variable, interval, AD::AddressSet::bottom(), node);
}

void AbstractInterpretation::updateAddressSet(const SVFVar* variable,
                                              const AD::AddressSet& addresses,
                                              const ICFGNode* node)
{
    updateValue(variable, AD::Interval::bottom(), addresses, node);
}

AD::Interval AbstractInterpretation::getGepElementIndex(const GepStmt* gep)
{
    const ICFGNode* node = gep->getICFGNode();
    if (gep->isConstantOffset())
        return AD::Interval::singleton(
            AD::Rational(static_cast<s64_t>(gep->accumulateConstantOffset())));

    AD::Interval result = AD::Interval::singleton(AD::Rational());
    for (int index =
             static_cast<int>(gep->getOffsetVarAndGepTypePairVec().size()) - 1;
         index >= 0; --index)
    {
        const ValVar* variable =
            gep->getOffsetVarAndGepTypePairVec()[index].first;
        const SVFType* type =
            gep->getOffsetVarAndGepTypePairVec()[index].second;

        s64_t lower = 0;
        s64_t upper = 0;
        if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(variable))
        {
            lower = upper = integer->getSExtValue();
        }
        else
        {
            const AD::Interval value = getInterval(variable, node);
            lower = finiteEndpoint(value.lower(), 0);
            upper = finiteEndpoint(value.upper(), Options::MaxFieldLimit());
        }

        if (SVFUtil::isa<SVFPointerType>(type))
        {
            const u32_t elements = gep->getAccessPath().getElementNum(
                gep->getAccessPath().gepSrcPointeeType());
            lower = (double)Options::MaxFieldLimit() / elements < lower
                        ? Options::MaxFieldLimit()
                        : lower * elements;
            upper = (double)Options::MaxFieldLimit() / elements < upper
                        ? Options::MaxFieldLimit()
                        : upper * elements;
        }
        else if (Options::ModelArrays())
        {
            const std::vector<u32_t>& flattened =
                PAG::getPAG()->getTypeInfo(type)->getFlattenedElemIdxVec();
            if (flattened.empty() ||
                upper >= static_cast<APOffset>(flattened.size()) || lower < 0)
            {
                lower = upper = 0;
            }
            else
            {
                lower = PAG::getPAG()->getFlattenedElemIdx(type, lower);
                upper = PAG::getPAG()->getFlattenedElemIdx(type, upper);
            }
        }
        else
        {
            lower = upper = 0;
        }
        result = AD::add(result, finiteInterval(lower, upper));
    }
    result.meetWith(
        finiteInterval(0, static_cast<s64_t>(Options::MaxFieldLimit())));
    return result.isBottom() ? AD::Interval::singleton(AD::Rational()) : result;
}

AD::Interval AbstractInterpretation::getGepByteOffset(const GepStmt* gep)
{
    const ICFGNode* node = gep->getICFGNode();
    if (gep->isConstantOffset())
        return AD::Interval::singleton(AD::Rational(
            static_cast<s64_t>(gep->accumulateConstantByteOffset())));

    AD::Interval result = AD::Interval::singleton(AD::Rational());
    for (int index =
             static_cast<int>(gep->getOffsetVarAndGepTypePairVec().size()) - 1;
         index >= 0; --index)
    {
        const ValVar* variable =
            gep->getOffsetVarAndGepTypePairVec()[index].first;
        const SVFType* type =
            gep->getOffsetVarAndGepTypePairVec()[index].second;

        if (SVFUtil::isa<SVFArrayType>(type) ||
            SVFUtil::isa<SVFPointerType>(type))
        {
            u32_t elementSize = 1;
            if (const auto* array = SVFUtil::dyn_cast<SVFArrayType>(type))
                elementSize = array->getTypeOfElement()->getByteSize();
            else
                elementSize =
                    gep->getAccessPath().gepSrcPointeeType()->getByteSize();

            s64_t lower = 0;
            s64_t upper = 0;
            if (const auto* integer =
                    SVFUtil::dyn_cast<ConstIntValVar>(variable))
            {
                lower = upper = integer->getSExtValue();
            }
            else
            {
                const AD::Interval value = getInterval(variable, node);
                lower = finiteEndpoint(value.lower(), 0);
                upper = finiteEndpoint(value.upper(), Options::MaxFieldLimit());
            }
            lower = std::max<s64_t>(0, lower);
            upper = std::max<s64_t>(0, upper);
            lower = (double)Options::MaxFieldLimit() / elementSize >= lower
                        ? lower * elementSize
                        : Options::MaxFieldLimit();
            upper = (double)Options::MaxFieldLimit() / elementSize >= upper
                        ? upper * elementSize
                        : Options::MaxFieldLimit();
            result = AD::add(result, finiteInterval(lower, upper));
        }
        else if (const auto* structure = SVFUtil::dyn_cast<SVFStructType>(type))
        {
            const s64_t offset =
                gep->getAccessPath().getStructFieldOffset(variable, structure);
            result =
                AD::add(result, AD::Interval::singleton(AD::Rational(offset)));
        }
        else
        {
            throw std::invalid_argument(
                "GEP type pair must be array, pointer, or structure");
        }
    }
    return result;
}

AD::AddressSet AbstractInterpretation::getGepObjAddrs(
    const ValVar* pointer, const AD::Interval& offset, const ICFGNode* node)
{
    const AD::AddressSet bases = getAddressSet(pointer, node);
    if (bases.isTop())
        return AD::AddressSet::top();
    if (offset.isBottom())
        return AD::AddressSet::bottom();

    auto integerEndpoint = [](const AD::Bound& bound,
                              bool lower) -> std::optional<APOffset> {
        if (!bound.isFinite())
            return std::nullopt;
        const AD::Rational integer =
            lower ? (bound.isStrict() ? bound.value().floor() + AD::Rational(1)
                                      : bound.value().ceil())
                  : (bound.isStrict() ? bound.value().ceil() - AD::Rational(1)
                                      : bound.value().floor());
        try
        {
            return integer.toInt64();
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    };

    std::optional<APOffset> lower = integerEndpoint(offset.lower(), true);
    std::optional<APOffset> upper = integerEndpoint(offset.upper(), false);
    if (lower && upper && *lower > *upper)
        return AD::AddressSet::bottom();

    // A finite narrow range must retain its signed offsets: negative GEPs are
    // common in C++ vtable and subobject adjustment. SVFIR canonicalizes each
    // signed offset modulo the base object's field limit. For an unbounded or
    // wider range, enumerating one complete global field window covers every
    // possible canonical field without an unbounded loop.
    bool enumerateFieldUniverse = !lower || !upper;
    if (!enumerateFieldUniverse)
    {
        enumerateFieldUniverse =
            AD::Rational(*upper) - AD::Rational(*lower) >
            AD::Rational(static_cast<s64_t>(Options::MaxFieldLimit()));
    }
    if (enumerateFieldUniverse)
    {
        lower = 0;
        upper = static_cast<APOffset>(Options::MaxFieldLimit());
    }

    AD::AddressSet result = AD::AddressSet::bottom();
    for (APOffset index = *lower;; ++index)
    {
        for (AD::Location base : bases)
        {
            if (base.isNull())
                continue;
            const ObjVar* object = objectAt(base);
            if (!object)
                continue;
            const NodeID gepObject =
                svfir->getGepObjVar(object->getId(), index);
            const auto* gepVariable =
                SVFUtil::dyn_cast<ObjVar>(svfir->getSVFVar(gepObject));
            if (gepVariable)
                result.insert(locationOf(gepVariable));
        }
        if (index == *upper)
            break;
    }
    return result;
}

u32_t AbstractInterpretation::getAllocaInstByteSize(const AddrStmt* address)
{
    const ICFGNode* node = address->getICFGNode();
    const auto* object = SVFUtil::dyn_cast<ObjVar>(address->getRHSVar());
    if (!object)
        throw std::invalid_argument("Addr rhs value is not ObjVar");
    const BaseObjVar* base = svfir->getBaseObject(object->getId());
    if (!base)
        return Options::MaxFieldLimit();
    if (base->isConstantByteSize())
        return base->getByteSizeOfObj();

    u64_t result = 1;
    for (const SVFVar* value : address->getArrSize())
    {
        const AD::Interval size = getInterval(value, node);
        const u64_t upper = static_cast<u64_t>(std::clamp<s64_t>(
            finiteEndpoint(size.upper(), Options::MaxFieldLimit()), 0,
            Options::MaxFieldLimit()));
        result = upper != 0 && result > Options::MaxFieldLimit() / upper
                     ? Options::MaxFieldLimit()
                     : result * upper;
    }
    return static_cast<u32_t>(result);
}

static AD::ConstraintKind negatePredicate(AD::ConstraintKind kind)
{
    switch (kind)
    {
    case AD::ConstraintKind::Equal:
        return AD::ConstraintKind::NotEqual;
    case AD::ConstraintKind::NotEqual:
        return AD::ConstraintKind::Equal;
    case AD::ConstraintKind::LessThan:
        return AD::ConstraintKind::GreaterEqual;
    case AD::ConstraintKind::LessEqual:
        return AD::ConstraintKind::GreaterThan;
    case AD::ConstraintKind::GreaterThan:
        return AD::ConstraintKind::LessEqual;
    case AD::ConstraintKind::GreaterEqual:
        return AD::ConstraintKind::LessThan;
    }
    return kind;
}

static bool constraintKind(u32_t predicate, AD::ConstraintKind& kind)
{
    switch (predicate)
    {
    case CmpStmt::ICMP_EQ:
        kind = AD::ConstraintKind::Equal;
        return true;
    case CmpStmt::ICMP_NE:
        kind = AD::ConstraintKind::NotEqual;
        return true;
    case CmpStmt::ICMP_SLT:
        kind = AD::ConstraintKind::LessThan;
        return true;
    case CmpStmt::ICMP_SLE:
        kind = AD::ConstraintKind::LessEqual;
        return true;
    case CmpStmt::ICMP_SGT:
        kind = AD::ConstraintKind::GreaterThan;
        return true;
    case CmpStmt::ICMP_SGE:
        kind = AD::ConstraintKind::GreaterEqual;
        return true;
    default:
        return false;
    }
}

const AbstractDomain::AbstractDomain& AbstractInterpretation::
    getAbstractState(const ICFGNode* node) const
{
    return state(node);
}

bool AbstractInterpretation::hasAbsState(const ICFGNode* node) const
{
    return stateTrace_.count(node) != 0;
}

AD::Location AbstractInterpretation::locationOf(const ObjVar* object) const
{
    return object ? adapter_.location(*object) : AD::Location::null();
}

const ObjVar* AbstractInterpretation::objectAt(AD::Location location) const
{
    return location.isNull() ? nullptr : &adapter_.object(location);
}

AbstractInterpretation::State AbstractInterpretation::topState()
    const
{
    return State(AD::BoxDomain::top(), adapter_.memoryLayout());
}

AbstractInterpretation::State AbstractInterpretation::
    bottomState() const
{
    return State(AD::BoxDomain::bottom(), adapter_.memoryLayout());
}

AbstractInterpretation::State& AbstractInterpretation::
    ensureState(const ICFGNode* node)
{
    auto iterator = stateTrace_.find(node);
    if (iterator == stateTrace_.end())
        iterator = stateTrace_.emplace(node, topState()).first;
    return iterator->second;
}

const AbstractInterpretation::State& AbstractInterpretation::
    state(const ICFGNode* node) const
{
    const auto iterator = stateTrace_.find(node);
    if (iterator == stateTrace_.end())
        throw std::out_of_range("no dense abstract state for ICFG node");
    return iterator->second;
}

void AbstractInterpretation::resetAbstractState(const ICFGNode* node)
{
    stateTrace_.insert_or_assign(node, topState());
}

void AbstractInterpretation::copyAbstractState(const ICFGNode* source,
                                                    const ICFGNode* destination)
{
    stateTrace_.insert_or_assign(destination, state(source));
}

std::unique_ptr<AbstractDomain::AbstractDomain> AbstractInterpretation::
    cloneAbstractState(const ICFGNode* node) const
{
    return state(node).clone();
}

bool AbstractInterpretation::isAbstractStateEquivalent(
    const ICFGNode* node, const AbstractDomain::AbstractDomain& snapshot) const
{
    return state(node).isEquivalentTo(snapshot) ==
           AbstractDomain::CheckResult::True;
}

void AbstractInterpretation::assignInterval(State& denseState,
                                                 AD::Variable variable,
                                                 const AD::Interval& interval)
{
    // A singleton is an exact affine assignment.  Committing it directly
    // avoids encoding the same fact as two inequalities and running the
    // generic constraint-propagation fixpoint.  Constant-heavy global
    // initializers exercise this path once per aggregate element.
    if (interval.isSingleton())
    {
        denseState.numerical().assign(
            variable, AD::LinearExpression(interval.singletonValue()));
        return;
    }
    denseState.numerical().forget(variable);
    constrainInterval(denseState, variable, interval);
}

void AbstractInterpretation::constrainInterval(
    State& denseState, AD::Variable variable, const AD::Interval& interval)
{
    if (interval.isBottom())
        return;

    AD::LinearConstraintSet constraints;
    AD::LinearExpression expression(variable);
    if (interval.lower().isFinite())
    {
        constraints.emplace_back(
            expression - AD::LinearExpression(interval.lower().value()),
            interval.lower().isStrict() ? AD::ConstraintKind::GreaterThan
                                        : AD::ConstraintKind::GreaterEqual);
    }
    if (interval.upper().isFinite())
    {
        constraints.emplace_back(
            expression - AD::LinearExpression(interval.upper().value()),
            interval.upper().isStrict() ? AD::ConstraintKind::LessThan
                                        : AD::ConstraintKind::LessEqual);
    }
    denseState.numerical().assumeAll(constraints);
}

void AbstractInterpretation::assignValue(State& denseState,
                                              AD::Variable variable,
                                              const AD::Interval& interval,
                                              const AD::AddressSet& addresses)
{
    if (adapter_.isPointer(variable))
    {
        denseState.numerical().forget(variable);
        denseState.addresses().assign(variable, addresses);
        return;
    }

    if (interval.isBottom())
        denseState.numerical().forget(variable);
    else
        assignInterval(denseState, variable, interval);
    denseState.addresses().forget(variable);
}

void AbstractInterpretation::assignMemoryValue(
    State& denseState, AD::Variable content,
    const AD::Interval& interval, const AD::AddressSet& addresses)
{
    if (interval.isBottom())
        denseState.numerical().forget(content);
    else
        assignInterval(denseState, content, interval);
    if (addresses.isBottom())
        denseState.addresses().forget(content);
    else
        denseState.addresses().assign(content, addresses);
}

void AbstractInterpretation::materializeValue(State&, const ValVar*,
                                                   const ICFGNode*)
{
}

void AbstractInterpretation::forgetValue(State& denseState,
                                              AD::Variable variable) const
{
    denseState.numerical().forget(variable);
    denseState.addresses().forget(variable);
}

AD::Interval AbstractInterpretation::getInterval(const ValVar* var,
                                                      const ICFGNode* node)
{
    AD::Interval constant;
    if (constantInterval(var, constant))
        return constant;
    if (var->isPointer())
        return AD::Interval::bottom();
    if (!adapter_.contains(*var))
        return AD::Interval::top();

    const State& denseState = ensureState(node);
    const AD::Variable variable = adapter_.variable(*var);
    return denseState.numerical().bound(variable);
}

AD::Interval AbstractInterpretation::getInterval(const ObjVar* var,
                                                      const ICFGNode* node)
{
    const State& denseState = ensureState(node);
    const AD::Variable content = adapter_.contentVariable(*var);
    return denseState.numerical().bound(content);
}

AD::Interval AbstractInterpretation::getInterval(const SVFVar* var,
                                                      const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getInterval(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getInterval(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

AD::AddressSet AbstractInterpretation::getAddressSet(const ValVar* var,
                                                          const ICFGNode* node)
{
    if (!var->isPointer())
        return AD::AddressSet::bottom();
    if (!adapter_.contains(*var))
        return AD::AddressSet::top();
    const State& denseState = ensureState(node);
    const AD::Variable variable = adapter_.variable(*var);
    return denseState.addresses().addressSet(variable);
}

AD::AddressSet AbstractInterpretation::getAddressSet(const ObjVar* var,
                                                          const ICFGNode* node)
{
    const State& denseState = ensureState(node);
    const AD::Variable content = adapter_.contentVariable(*var);
    return denseState.addresses().addressSet(content);
}

AD::AddressSet AbstractInterpretation::getAddressSet(const SVFVar* var,
                                                          const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getAddressSet(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getAddressSet(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

bool AbstractInterpretation::hasAbsValue(const ValVar* var,
                                              const ICFGNode* node) const
{
    if (SVFUtil::isa<ConstIntValVar>(var) ||
        SVFUtil::isa<ConstFPValVar>(var))
        return true;
    return stateTrace_.count(node) != 0 && adapter_.contains(*var);
}

bool AbstractInterpretation::hasAbsValue(const ObjVar* var,
                                              const ICFGNode* node) const
{
    (void)var;
    return stateTrace_.count(node) != 0;
}

bool AbstractInterpretation::hasAbsValue(const SVFVar* var,
                                              const ICFGNode* node) const
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return hasAbsValue(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return hasAbsValue(value, node);
    return false;
}

void AbstractInterpretation::updateValue(const ValVar* var,
                                              const AD::Interval& interval,
                                              const AD::AddressSet& addresses,
                                              const ICFGNode* node)
{
    if (adapter_.contains(*var))
        assignValue(ensureState(node), adapter_.variable(*var), interval,
                    addresses);
}

void AbstractInterpretation::updateValue(const ObjVar* var,
                                              const AD::Interval& interval,
                                              const AD::AddressSet& addresses,
                                              const ICFGNode* node)
{
    assignMemoryValue(ensureState(node), adapter_.contentVariable(*var),
                      interval, addresses);
}

AD::Interval AbstractInterpretation::getMemoryInterval(
    AD::Location location, const ICFGNode* node)
{
    if (location.isNull())
        return AD::Interval::bottom();
    return getInterval(&adapter_.object(location), node);
}

AD::AddressSet AbstractInterpretation::getMemoryAddressSet(
    AD::Location location, const ICFGNode* node)
{
    if (location.isNull())
        return AD::AddressSet::bottom();
    return getAddressSet(&adapter_.object(location), node);
}

bool AbstractInterpretation::hasMemoryValue(AD::Location location,
                                                 const ICFGNode* node) const
{
    return !location.isNull() && hasAbsValue(&adapter_.object(location), node);
}

void AbstractInterpretation::updateMemoryValue(
    AD::Location location, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (!location.isNull())
        updateValue(&adapter_.object(location), interval, addresses, node);
}

void AbstractInterpretation::markFreedMemory(AD::Location location,
                                                  const ICFGNode* node)
{
    if (!location.isNull())
        ensureState(node).lifetimes().release(location);
}

bool AbstractInterpretation::isFreedMemory(AD::Location location,
                                                const ICFGNode* node) const
{
    if (stateTrace_.count(node) == 0 || location.isNull())
        return false;
    return state(node).lifetimes().mayBeFreed(location);
}

void AbstractInterpretation::updateValue(const SVFVar* var,
                                              const AD::Interval& interval,
                                              const AD::AddressSet& addresses,
                                              const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        updateValue(object, interval, addresses, node);
    else if (const auto* scalar = SVFUtil::dyn_cast<ValVar>(var))
        updateValue(scalar, interval, addresses, node);
    else
        throw std::invalid_argument("unsupported SVF variable kind");
}


void AbstractInterpretation::loadValue(const ValVar* pointer,
                                            AD::Interval& interval,
                                            AD::AddressSet& addresses,
                                            const ICFGNode* node)
{
    if (!adapter_.contains(*pointer))
    {
        interval = AD::Interval::bottom();
        addresses = AD::AddressSet::bottom();
        const AD::AddressSet pointees = getAddressSet(pointer, node);
        if (pointees.isTop())
        {
            interval = AD::Interval::top();
            addresses = AD::AddressSet::top();
            return;
        }
        for (AD::Location location : pointees)
        {
            interval.joinWith(getMemoryInterval(location, node));
            addresses.joinWith(getMemoryAddressSet(location, node));
        }
        return;
    }
    State& denseState = ensureState(node);
    materializeValue(denseState, pointer, node);
    const AD::AddressSet pointees = getAddressSet(pointer, node);
    if (pointees.isTop())
    {
        interval = AD::Interval::top();
        addresses = AD::AddressSet::top();
        return;
    }

    interval = AD::Interval::bottom();
    addresses = AD::AddressSet::bottom();
    for (AD::Location location : pointees.locations())
    {
        if (denseState.lifetimes().mayBeFreed(location))
        {
            interval.joinWith(AD::Interval::top());
            addresses.joinWith(AD::AddressSet::top());
            continue;
        }
        if (denseState.memoryLayout().contains(location))
        {
            const ObjVar* object = objectAt(location);
            if (!object)
                continue;
            interval.joinWith(getInterval(object, node));
            addresses.joinWith(getAddressSet(object, node));
        }
    }
}

void AbstractInterpretation::storeValue(const ValVar* pointer,
                                             const AD::Interval& interval,
                                             const AD::AddressSet& addresses,
                                             const ICFGNode* node)
{
    if (!adapter_.contains(*pointer))
    {
        const AD::AddressSet pointees = getAddressSet(pointer, node);
        if (!pointees.isTop())
        {
            for (AD::Location location : pointees)
                updateMemoryValue(location, interval, addresses, node);
        }
        return;
    }
    State& denseState = ensureState(node);
    materializeValue(denseState, pointer, node);
    const AD::AddressSet pointees = getAddressSet(pointer, node);
    const bool strong = pointees.isSingleton();
    auto write = [&](AD::Location location) {
        if (!denseState.memoryLayout().contains(location))
            return;
        const AD::Variable content =
            denseState.memoryLayout().contentOf(location);
        const ObjVar* object = objectAt(location);
        if (!object)
            return;
        if (strong)
        {
            assignMemoryValue(denseState, content, interval, addresses);
            return;
        }
        AD::Interval joinedInterval = getInterval(object, node);
        AD::AddressSet joinedAddresses = getAddressSet(object, node);
        joinedInterval.joinWith(interval);
        joinedAddresses.joinWith(addresses);
        assignMemoryValue(denseState, content, joinedInterval,
                          joinedAddresses);
    };

    if (pointees.isTop())
    {
        for (const auto& [location, content] :
             denseState.memoryLayout().cells())
        {
            (void)content;
            write(location);
        }
    }
    else
    {
        for (AD::Location location : pointees.locations())
            write(location);
    }
}


void AbstractInterpretation::assumeBranch(const IntraCFGEdge* edge,
                                               State& denseState)
{
    const SVFVar* condition = edge->getCondition();
    if (!condition || condition->getInEdges().empty())
        return;
    const auto* comparison =
        SVFUtil::dyn_cast<CmpStmt>(*condition->getInEdges().begin());
    if (!comparison)
    {
        const auto* value = SVFUtil::dyn_cast<ValVar>(condition);
        if (!value || !adapter_.contains(*value))
            return;
        materializeValue(denseState, value, edge->getSrcNode());
        denseState.assume(AD::equal(
            AD::LinearExpression(adapter_.variable(*value)),
            AD::LinearExpression(AD::Rational(edge->getSuccessorCondValue()))));
        return;
    }

    AD::ConstraintKind kind;
    if (!constraintKind(comparison->getPredicate(), kind))
        return;
    if (edge->getSuccessorCondValue() == 0)
        kind = negatePredicate(kind);

    auto operand = [&](const SVFVar* variable,
                       AD::LinearExpression& expression) -> bool {
        if (const auto* value = SVFUtil::dyn_cast<ValVar>(variable))
        {
            AD::Interval constant;
            if (constantInterval(value, constant) && constant.isSingleton())
            {
                expression = AD::LinearExpression(constant.singletonValue());
                return true;
            }
        }
        const auto* value = SVFUtil::dyn_cast<ValVar>(variable);
        if (!value || !adapter_.contains(*value))
            return false;
        materializeValue(denseState, value, edge->getSrcNode());
        expression = AD::LinearExpression(adapter_.variable(*value));
        return true;
    };

    AD::LinearExpression lhs;
    AD::LinearExpression rhs;
    if (!operand(comparison->getOpVar(0), lhs) ||
        !operand(comparison->getOpVar(1), rhs))
        return;
    denseState.assume(AD::LinearConstraint(lhs - rhs, kind));
}


bool AbstractInterpretation::isBranchEdgeFeasibleAt(
    const IntraCFGEdge* edge, const ICFGNode* predecessor)
{
    State candidate = state(predecessor);
    assumeBranch(edge, candidate);
    return !candidate.isBottom();
}

} // namespace SVF
