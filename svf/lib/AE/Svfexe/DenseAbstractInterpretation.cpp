//===- DenseAbstractInterpretation.cpp -- Domain-backed dense AE --------===//

#include "AE/Svfexe/DenseAbstractInterpretation.h"

#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace SVF
{

namespace AD = AbstractDomain;

namespace
{

std::vector<const ICFGEdge*> orderedIncomingEdges(const ICFGNode* node)
{
    std::vector<const ICFGEdge*> edges(node->getInEdges().begin(),
                                       node->getInEdges().end());
    std::sort(edges.begin(), edges.end(),
              [](const ICFGEdge* lhs, const ICFGEdge* rhs) {
                  return std::make_tuple(lhs->getSrcID(),
                                         lhs->getEdgeKindWithoutMask()) <
                         std::make_tuple(rhs->getSrcID(),
                                         rhs->getEdgeKindWithoutMask());
              });
    return edges;
}

AD::ConstraintKind negatePredicate(AD::ConstraintKind kind)
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

bool constraintKind(u32_t predicate, AD::ConstraintKind& kind)
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

} // namespace

DenseAbstractInterpretation::DenseAbstractInterpretation() : adapter_(*svfir) {}

void DenseAbstractInterpretation::handleGlobalNode()
{
    const ICFGNode* node = icfg->getGlobalICFGNode();
    denseTrace_.insert_or_assign(node, topState());
    for (const SVFStmt* statement : node->getSVFStmts())
        handleSVFStatement(statement);

    if (const auto* variable = SVFUtil::dyn_cast<ValVar>(
            svfir->getGNode(PAG::getPAG()->getBlkPtr())))
        updateValue(variable, AD::Interval::top(), AD::AddressSet::top(), node);
}

void DenseAbstractInterpretation::initializeObjectValue(
    const ObjVar* object, AD::Interval& interval, AD::AddressSet& addresses,
    const ICFGNode* node)
{
    interval = AD::Interval::bottom();
    addresses = AD::AddressSet::bottom();
    DenseState& denseState = ensureState(node);
    denseState.allocate(adapter_.location(*object));

    const BaseObjVar* base = PAG::getPAG()->getBaseObject(object->getId());
    if (base->isConstDataOrConstGlobal() || base->isConstantArray() ||
        base->isConstantStruct())
    {
        if (const auto* integer = SVFUtil::dyn_cast<ConstIntObjVar>(object))
            interval =
                AD::Interval::singleton(AD::Rational(integer->getSExtValue()));
        else if (const auto* floating =
                     SVFUtil::dyn_cast<ConstFPObjVar>(object))
            interval = AD::Interval::singleton(
                AD::Rational::fromDouble(floating->getFPValue()));
        else if (SVFUtil::isa<ConstNullPtrObjVar>(object))
            addresses = AD::AddressSet::singleton(AD::Location::null());
        else if (!SVFUtil::isa<GlobalObjVar>(object))
            interval = AD::Interval::top();
        if (!interval.isBottom() || !addresses.isBottom())
            return;
    }
    addresses = AD::AddressSet::singleton(adapter_.location(*object));
}

const AbstractDomain::AbstractDomain& DenseAbstractInterpretation::
    getAbstractState(const ICFGNode* node) const
{
    return state(node);
}

bool DenseAbstractInterpretation::hasAbsState(const ICFGNode* node) const
{
    return denseTrace_.count(node) != 0;
}

AD::Location DenseAbstractInterpretation::locationOf(const ObjVar* object) const
{
    return object ? adapter_.location(*object) : AD::Location::null();
}

const ObjVar* DenseAbstractInterpretation::objectAt(AD::Location location) const
{
    return location.isNull() ? nullptr : &adapter_.object(location);
}

DenseAbstractInterpretation::DenseState DenseAbstractInterpretation::topState()
    const
{
    return DenseState(AD::BoxDomain::top(), adapter_.memoryLayout());
}

DenseAbstractInterpretation::DenseState DenseAbstractInterpretation::
    bottomState() const
{
    return DenseState(AD::BoxDomain::bottom(), adapter_.memoryLayout());
}

DenseAbstractInterpretation::DenseState& DenseAbstractInterpretation::
    ensureState(const ICFGNode* node)
{
    auto iterator = denseTrace_.find(node);
    if (iterator == denseTrace_.end())
        iterator = denseTrace_.emplace(node, topState()).first;
    return iterator->second;
}

const DenseAbstractInterpretation::DenseState& DenseAbstractInterpretation::
    state(const ICFGNode* node) const
{
    const auto iterator = denseTrace_.find(node);
    if (iterator == denseTrace_.end())
        throw std::out_of_range("no dense abstract state for ICFG node");
    return iterator->second;
}

void DenseAbstractInterpretation::resetAbstractState(const ICFGNode* node)
{
    denseTrace_.insert_or_assign(node, topState());
}

void DenseAbstractInterpretation::copyAbstractState(const ICFGNode* source,
                                                    const ICFGNode* destination)
{
    denseTrace_.insert_or_assign(destination, state(source));
}

std::unique_ptr<AbstractDomain::AbstractDomain> DenseAbstractInterpretation::
    cloneAbstractState(const ICFGNode* node) const
{
    return state(node).clone();
}

bool DenseAbstractInterpretation::isAbstractStateEquivalent(
    const ICFGNode* node, const AbstractDomain::AbstractDomain& snapshot) const
{
    return state(node).isEquivalentTo(snapshot) ==
           AbstractDomain::CheckResult::True;
}

std::unique_ptr<AbstractDomain::AbstractDomain> DenseAbstractInterpretation::
    cloneCycleHeadState(const ICFGCycleWTO* cycle)
{
    return cloneAbstractState(cycle->head()->getICFGNode());
}

bool DenseAbstractInterpretation::widenCycleState(
    const AbstractDomain::AbstractDomain& previous,
    const AbstractDomain::AbstractDomain& current, const ICFGCycleWTO* cycle)
{
    const DenseState& previousDense = static_cast<const DenseState&>(previous);
    const DenseState& currentDense = static_cast<const DenseState&>(current);
    DenseState next = previousDense;
    next.widenWith(currentDense);
    const bool fixpoint =
        next.isEquivalentTo(previousDense) == AbstractDomain::CheckResult::True;
    const ICFGNode* head = cycle->head()->getICFGNode();
    denseTrace_.insert_or_assign(head, std::move(next));
    return fixpoint;
}

bool DenseAbstractInterpretation::narrowCycleState(
    const AbstractDomain::AbstractDomain& previous,
    const AbstractDomain::AbstractDomain& current, const ICFGCycleWTO* cycle)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    if (!shouldApplyNarrowing(head->getFun()))
        return true;
    const DenseState& previousDense = static_cast<const DenseState&>(previous);
    DenseState currentDense = static_cast<const DenseState&>(current);
    // Sparse transfers may materialize a new MemorySSA/cycle facet during the
    // descending phase. Enforce narrowing's generic next <= current contract.
    // The normal descending path already satisfies that contract. Avoid
    // rebuilding and closing a relational meet when the lattice check proves
    // that the meet would be exactly currentDense. False and Unknown retain
    // the original conservative meet.
    if (currentDense.isSubsetOf(previousDense) != AD::CheckResult::True)
        currentDense.meetWith(previousDense);
    DenseState next = previousDense;
    next.narrowWith(currentDense);
    const bool fixpoint =
        next.isEquivalentTo(previousDense) == AbstractDomain::CheckResult::True;
    if (!fixpoint)
        denseTrace_.insert_or_assign(head, std::move(next));
    return fixpoint;
}

void DenseAbstractInterpretation::assignInterval(DenseState& denseState,
                                                 AD::Variable variable,
                                                 const AD::Interval& interval)
{
    denseState.numerical().forget(variable);
    constrainInterval(denseState, variable, interval);
}

void DenseAbstractInterpretation::constrainInterval(
    DenseState& denseState, AD::Variable variable, const AD::Interval& interval)
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

void DenseAbstractInterpretation::assignValue(DenseState& denseState,
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

void DenseAbstractInterpretation::assignMemoryValue(
    DenseState& denseState, AD::Variable content,
    const AD::Interval& interval, const AD::AddressSet& addresses)
{
    if (interval.isBottom())
        denseState.numerical().forget(content);
    else
        assignInterval(denseState, content, interval);
    denseState.addresses().assign(content, addresses);
}

void DenseAbstractInterpretation::materializeValue(DenseState&, const ValVar*,
                                                   const ICFGNode*)
{
}

void DenseAbstractInterpretation::forgetValue(DenseState& denseState,
                                              AD::Variable variable) const
{
    denseState.numerical().forget(variable);
    denseState.addresses().forget(variable);
}

AD::Interval DenseAbstractInterpretation::getInterval(const ValVar* var,
                                                      const ICFGNode* node)
{
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(var))
        return AD::Interval::singleton(AD::Rational(integer->getSExtValue()));
    if (!adapter_.contains(*var))
        return AD::Interval::top();

    const DenseState& denseState = ensureState(node);
    const AD::Variable variable = adapter_.variable(*var);
    if (var->isPointer())
        return AD::Interval::bottom();
    return denseState.numerical().bound(variable);
}

AD::Interval DenseAbstractInterpretation::getInterval(const ObjVar* var,
                                                      const ICFGNode* node)
{
    const DenseState& denseState = ensureState(node);
    const AD::Variable content = adapter_.contentVariable(*var);
    return denseState.numerical().bound(content);
}

AD::Interval DenseAbstractInterpretation::getInterval(const SVFVar* var,
                                                      const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getInterval(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getInterval(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

AD::AddressSet DenseAbstractInterpretation::getAddressSet(const ValVar* var,
                                                          const ICFGNode* node)
{
    if (!adapter_.contains(*var))
        return AD::AddressSet::top();
    if (!var->isPointer())
        return AD::AddressSet::bottom();
    const DenseState& denseState = ensureState(node);
    const AD::Variable variable = adapter_.variable(*var);
    return denseState.addresses().addressSet(variable);
}

AD::AddressSet DenseAbstractInterpretation::getAddressSet(const ObjVar* var,
                                                          const ICFGNode* node)
{
    const DenseState& denseState = ensureState(node);
    const AD::Variable content = adapter_.contentVariable(*var);
    return denseState.addresses().addressSet(content);
}

AD::AddressSet DenseAbstractInterpretation::getAddressSet(const SVFVar* var,
                                                          const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getAddressSet(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getAddressSet(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

bool DenseAbstractInterpretation::hasAbsValue(const ValVar* var,
                                              const ICFGNode* node) const
{
    if (SVFUtil::isa<ConstIntValVar>(var))
        return true;
    return denseTrace_.count(node) != 0 && adapter_.contains(*var);
}

bool DenseAbstractInterpretation::hasAbsValue(const ObjVar* var,
                                              const ICFGNode* node) const
{
    (void)var;
    return denseTrace_.count(node) != 0;
}

bool DenseAbstractInterpretation::hasAbsValue(const SVFVar* var,
                                              const ICFGNode* node) const
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return hasAbsValue(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return hasAbsValue(value, node);
    return false;
}

void DenseAbstractInterpretation::updateValue(const ValVar* var,
                                              const AD::Interval& interval,
                                              const AD::AddressSet& addresses,
                                              const ICFGNode* node)
{
    if (adapter_.contains(*var))
        assignValue(ensureState(node), adapter_.variable(*var), interval,
                    addresses);
}

void DenseAbstractInterpretation::updateValue(const ObjVar* var,
                                              const AD::Interval& interval,
                                              const AD::AddressSet& addresses,
                                              const ICFGNode* node)
{
    assignMemoryValue(ensureState(node), adapter_.contentVariable(*var),
                      interval, addresses);
}

AD::Interval DenseAbstractInterpretation::getMemoryInterval(
    AD::Location location, const ICFGNode* node)
{
    if (location.isNull())
        return AD::Interval::bottom();
    return getInterval(&adapter_.object(location), node);
}

AD::AddressSet DenseAbstractInterpretation::getMemoryAddressSet(
    AD::Location location, const ICFGNode* node)
{
    if (location.isNull())
        return AD::AddressSet::bottom();
    return getAddressSet(&adapter_.object(location), node);
}

bool DenseAbstractInterpretation::hasMemoryValue(AD::Location location,
                                                 const ICFGNode* node) const
{
    return !location.isNull() && hasAbsValue(&adapter_.object(location), node);
}

void DenseAbstractInterpretation::updateMemoryValue(
    AD::Location location, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (!location.isNull())
        updateValue(&adapter_.object(location), interval, addresses, node);
}

void DenseAbstractInterpretation::markFreedMemory(AD::Location location,
                                                  const ICFGNode* node)
{
    if (!location.isNull())
        ensureState(node).lifetimes().release(location);
}

bool DenseAbstractInterpretation::isFreedMemory(AD::Location location,
                                                const ICFGNode* node) const
{
    if (denseTrace_.count(node) == 0 || location.isNull())
        return false;
    return state(node).lifetimes().mayBeFreed(location);
}

void DenseAbstractInterpretation::updateValue(const SVFVar* var,
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

void DenseAbstractInterpretation::loadValue(const ValVar* pointer,
                                            AD::Interval& interval,
                                            AD::AddressSet& addresses,
                                            const ICFGNode* node)
{
    if (!adapter_.contains(*pointer))
    {
        AbstractInterpretation::loadValue(pointer, interval, addresses, node);
        return;
    }
    DenseState& denseState = ensureState(node);
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

void DenseAbstractInterpretation::storeValue(const ValVar* pointer,
                                             const AD::Interval& interval,
                                             const AD::AddressSet& addresses,
                                             const ICFGNode* node)
{
    if (!adapter_.contains(*pointer))
    {
        AbstractInterpretation::storeValue(pointer, interval, addresses, node);
        return;
    }
    DenseState& denseState = ensureState(node);
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

void DenseAbstractInterpretation::assumeBranch(const IntraCFGEdge* edge,
                                               DenseState& denseState)
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
        if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(variable))
        {
            expression =
                AD::LinearExpression(AD::Rational(integer->getSExtValue()));
            return true;
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

bool DenseAbstractInterpretation::mergeStatesFromPredecessors(
    const ICFGNode* node)
{
    DenseState merged = bottomState();
    bool hasFeasiblePredecessor = false;

    for (const ICFGEdge* edge : orderedIncomingEdges(node))
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (denseTrace_.count(predecessor) == 0)
            continue;

        bool shouldMerge = false;
        const IntraCFGEdge* conditional = SVFUtil::dyn_cast<IntraCFGEdge>(edge);
        if (conditional)
            shouldMerge = true;
        else if (SVFUtil::isa<CallCFGEdge>(edge))
        {
            shouldMerge = true;
        }
        else if (SVFUtil::isa<RetCFGEdge>(edge))
        {
            shouldMerge = Options::HandleRecur() == TOP;
            if (!shouldMerge)
            {
                const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node);
                shouldMerge =
                    returnSite &&
                    denseTrace_.count(returnSite->getCallICFGNode()) != 0;
            }
        }
        if (!shouldMerge)
            continue;

        DenseState source = state(predecessor);
        if (conditional && conditional->getCondition())
        {
            assumeBranch(conditional, source);
            collectBranchRefinement(conditional, source);
        }
        if (source.isBottom())
            continue;

        merged.joinWith(source);
        hasFeasiblePredecessor = true;
    }

    if (!hasFeasiblePredecessor)
        return false;
    denseTrace_.insert_or_assign(node, std::move(merged));
    return true;
}

void DenseAbstractInterpretation::recordBranchRefinement(
    NodeID objectId, const AD::Interval& narrowed,
    AD::AbstractDomain& abstractState, const ICFGNode*, const ICFGNode*)
{
    const auto* object = SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(objectId));
    if (!object)
        return;

    DenseState& denseState = static_cast<DenseState&>(abstractState);
    const AD::Variable content = adapter_.contentVariable(*object);
    if (object->isPointer())
        return;
    AD::Interval refined = denseState.numerical().bound(content);
    refined.meetWith(narrowed);
    assignInterval(denseState, content, refined);
}

bool DenseAbstractInterpretation::isBranchEdgeFeasibleAt(
    const IntraCFGEdge* edge, const ICFGNode* predecessor)
{
    DenseState candidate = state(predecessor);
    assumeBranch(edge, candidate);
    return !candidate.isBottom();
}

} // namespace SVF
