//===- NativeSparseAbstractInterpretation.cpp -- Domain sparse AE -------===//

#include "AE/Svfexe/NativeSparseAbstractInterpretation.h"

#include "Graphs/SVFG.h"
#include "MSSA/SVFGBuilder.h"
#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>

namespace SVF
{

namespace AD = AbstractDomain;

namespace
{

template <typename MetricT> class PhaseTimer
{
public:
    PhaseTimer(MetricT& metric, bool enabled)
        : metric_(metric), enabled_(enabled)
    {
        if (enabled_)
            start_ = Clock::now();
    }

    ~PhaseTimer()
    {
        if (!enabled_)
            return;
        ++metric_.calls;
        metric_.nanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                 start_)
                .count());
    }

private:
    using Clock = std::chrono::steady_clock;
    MetricT& metric_;
    bool enabled_;
    Clock::time_point start_{};
};

std::set<AD::Variable> nonDefaultVariables(
    const NativeSemiSparseAbstractInterpretation::DenseState& state)
{
    std::set<AD::Variable> variables;
    for (AD::Variable variable : state.numerical().constrainedVariables())
        variables.insert(variable);
    for (AD::Variable variable : state.addresses().nonDefaultVariables())
        variables.insert(variable);
    return variables;
}

} // namespace

NativeSemiSparseAbstractInterpretation::NativeSemiSparseAbstractInterpretation()
{
    this->preAnalysis->initCycleValVars();
}

NativeSemiSparseAbstractInterpretation::DenseState
NativeSemiSparseAbstractInterpretation::flowState(bool bottom) const
{
    return DenseState(bottom ? AD::BoxDomain::bottom() : AD::BoxDomain::top(),
                      this->adapter_.memoryLayout());
}

NativeSemiSparseAbstractInterpretation::DenseState&
NativeSemiSparseAbstractInterpretation::scalarState()
{
    if (!scalarState_)
        scalarState_.emplace(AD::BoxDomain::top(),
                             this->adapter_.memoryLayout());
    return *scalarState_;
}

const NativeSemiSparseAbstractInterpretation::DenseState*
NativeSemiSparseAbstractInterpretation::findScalarState() const
{
    return scalarState_ ? &*scalarState_ : nullptr;
}

const AD::AbstractDomain* NativeSemiSparseAbstractInterpretation::
    getScalarAbstractState() const
{
    return findScalarState();
}

void NativeSemiSparseAbstractInterpretation::handleGlobalNode()
{
    Base::handleGlobalNode();
    finalizeAbstractState(this->icfg->getGlobalICFGNode());
}

void NativeSemiSparseAbstractInterpretation::runOnModule()
{
    {
        PhaseTimer timer(sparseProfile_.total, Options::AESparseProfile());
        Base::runOnModule();
    }
    if (Options::AESparseProfile())
        reportSparseProfile();
}

const char* NativeSemiSparseAbstractInterpretation::sparseProfileMode() const
{
    return "semi";
}

void NativeSemiSparseAbstractInterpretation::reportSparseProfile() const
{
    const std::ios::fmtflags previousFlags = std::cout.flags();
    const std::streamsize previousPrecision = std::cout.precision();
    auto report = [&](const char* phase, const PhaseMetric& metric) {
        const double seconds =
            static_cast<double>(metric.nanoseconds) / 1'000'000'000.0;
        const double nanosecondsPerCall =
            metric.calls == 0 ? 0.0
                              : static_cast<double>(metric.nanoseconds) /
                                    static_cast<double>(metric.calls);
        std::cout << "AE_SPARSE_PHASE mode=" << sparseProfileMode()
                  << " phase=" << phase << " calls=" << metric.calls
                  << " seconds=" << std::fixed << std::setprecision(6)
                  << seconds << " ns_per_call=" << std::setprecision(1)
                  << nanosecondsPerCall << '\n';
    };
    report("total", sparseProfile_.total);
    report("state-copy", sparseProfile_.stateCopy);
    report("state-merge", sparseProfile_.stateMerge);
    report("state-join", sparseProfile_.stateJoin);
    report("state-equivalence", sparseProfile_.stateEquivalence);
    report("scalar-materialization", sparseProfile_.scalarMaterialization);
    report("scalar-refinement", sparseProfile_.scalarRefinement);
    report("state-filtering", sparseProfile_.stateFiltering);
    report("cycle", sparseProfile_.cycle);
    report("svfg-build", sparseProfile_.svfgBuild);
    report("object-pull", sparseProfile_.objectPull);
    report("path-feasibility", sparseProfile_.pathFeasibility);
    report("memory-refinement", sparseProfile_.memoryRefinement);
    std::cout.flags(previousFlags);
    std::cout.precision(previousPrecision);
}

AD::Interval NativeSemiSparseAbstractInterpretation::getInterval(
    const ValVar* value, const ICFGNode* node)
{
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(value))
        return AD::Interval::singleton(AD::Rational(integer->getSExtValue()));
    if (!value || !this->adapter_.contains(*value))
        return AD::Interval::top();

    const DenseState& scalars = scalarState();
    const AD::Variable variable = this->adapter_.variable(*value);
    if (value->isPointer())
        return AD::Interval::bottom();
    AD::Interval result = scalars.numerical().bound(variable);
    // Conditional-edge refinement is intentionally local to the ICFG state.
    // Read it in addition to the definition-site scalar carrier so transfer
    // functions observe path constraints without copying all SSA values into
    // every program point.
    if (node && this->hasAbsState(node))
    {
        const DenseState& local = this->state(node);
        const AD::Interval refined = local.numerical().bound(variable);
        if (!refined.isTop())
        {
            if (result.isBottom())
                result = refined;
            else
                result.meetWith(refined);
        }
    }
    return result;
}

AD::AddressSet NativeSemiSparseAbstractInterpretation::getAddressSet(
    const ValVar* value, const ICFGNode* node)
{
    (void)node;
    if (!value || !this->adapter_.contains(*value))
        return AD::AddressSet::top();
    if (!value->isPointer())
        return AD::AddressSet::bottom();
    const DenseState& scalars = scalarState();
    const AD::Variable variable = this->adapter_.variable(*value);
    return scalars.addresses().addressSet(variable);
}

bool NativeSemiSparseAbstractInterpretation::hasAbsValue(
    const ValVar* value, const ICFGNode* node) const
{
    (void)node;
    if (SVFUtil::isa<ConstIntValVar>(value))
        return true;
    if (!value || !this->adapter_.contains(*value))
        return false;
    return this->adapter_.contains(*value);
}

void NativeSemiSparseAbstractInterpretation::updateValue(
    const ValVar* value, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    (void)node;
    if (value && this->adapter_.contains(*value))
        this->assignValue(scalarState(), this->adapter_.variable(*value),
                          interval, addresses);
}

void NativeSemiSparseAbstractInterpretation::copyAbstractState(
    const ICFGNode* source, const ICFGNode* destination)
{
    PhaseTimer timer(sparseProfile_.stateCopy, Options::AESparseProfile());
    this->denseTrace_.insert_or_assign(destination, this->state(source));
}

void NativeSemiSparseAbstractInterpretation::resetAbstractState(
    const ICFGNode* node)
{
    this->denseTrace_.insert_or_assign(node, flowState());
}

void NativeSemiSparseAbstractInterpretation::finalizeAbstractState(
    const ICFGNode* node)
{
    PhaseTimer timer(sparseProfile_.stateFiltering, Options::AESparseProfile());
    DenseState& denseState = this->ensureState(node);
    forgetActiveScalarValues(denseState);
}

bool NativeSemiSparseAbstractInterpretation::isAbstractStateEquivalent(
    const ICFGNode* node, const AD::AbstractDomain& snapshot) const
{
    PhaseTimer timer(sparseProfile_.stateEquivalence,
                     Options::AESparseProfile());
    return Base::isAbstractStateEquivalent(node, snapshot);
}

void NativeSemiSparseAbstractInterpretation::forgetActiveScalarValues(
    DenseState& denseState) const
{
    const AD::Variable contentBegin =
        this->adapter_.firstObjectContentVariable();
    for (AD::Variable variable :
         denseState.numerical().constrainedVariablesBefore(contentBegin))
        denseState.numerical().forget(variable);
    for (AD::Variable variable :
         denseState.addresses().nonDefaultVariablesBefore(contentBegin))
        denseState.addresses().forget(variable);
}

void NativeSemiSparseAbstractInterpretation::forgetMemoryValues(
    DenseState& denseState) const
{
    for (AD::Variable variable : nonDefaultVariables(denseState))
    {
        if (this->adapter_.contentObject(variable))
            this->forgetValue(denseState, variable);
    }
}

void NativeSemiSparseAbstractInterpretation::applyScalarRefinement(
    DenseState& denseState, const DenseState& checkpoint)
{
    PhaseTimer timer(sparseProfile_.scalarRefinement,
                     Options::AESparseProfile());
    for (AD::Variable variable : checkpoint.numerical().constrainedVariables())
    {
        const ValVar* value = this->adapter_.value(variable);
        if (!value || value->isPointer())
            continue;
        this->constrainInterval(denseState, variable,
                                checkpoint.numerical().bound(variable));
        denseState.addresses().forget(variable);
    }
}

void NativeSemiSparseAbstractInterpretation::materializeValue(
    DenseState& denseState, const ValVar* value, const ICFGNode* node)
{
    PhaseTimer timer(sparseProfile_.scalarMaterialization,
                     Options::AESparseProfile());
    if (!value || !this->adapter_.contains(*value))
        return;
    if (!value->isPointer())
        return;
    const AD::Variable variable = this->adapter_.variable(*value);
    denseState.addresses().assign(variable, getAddressSet(value, node));
}

void NativeSemiSparseAbstractInterpretation::loadValue(
    const ValVar* pointer, AD::Interval& interval, AD::AddressSet& addresses,
    const ICFGNode* node)
{
    Base::loadValue(pointer, interval, addresses, node);
    if (pointer && this->adapter_.contains(*pointer))
        this->forgetValue(this->ensureState(node),
                          this->adapter_.variable(*pointer));
}

void NativeSemiSparseAbstractInterpretation::storeValue(
    const ValVar* pointer, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    Base::storeValue(pointer, interval, addresses, node);
    if (pointer && this->adapter_.contains(*pointer))
        this->forgetValue(this->ensureState(node),
                          this->adapter_.variable(*pointer));
}

void NativeSemiSparseAbstractInterpretation::filterPropagatedState(
    DenseState& denseState) const
{
    (void)denseState;
}

void NativeSemiSparseAbstractInterpretation::collectMemoryBranchRefinement(
    const IntraCFGEdge* edge, DenseState& state)
{
    this->collectBranchRefinement(edge, state);
}

bool NativeSemiSparseAbstractInterpretation::mergeStatesFromPredecessors(
    const ICFGNode* node)
{
    PhaseTimer timer(sparseProfile_.stateMerge, Options::AESparseProfile());
    DenseState merged = flowState(true);
    std::optional<DenseState> mergedRefinement;
    bool refinementIsTop = false;
    bool hasFeasiblePredecessor = false;

    for (const ICFGEdge* edge : node->getInEdges())
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (!this->hasAbsState(predecessor))
            continue;

        bool shouldMerge = false;
        const auto* conditional = SVFUtil::dyn_cast<IntraCFGEdge>(edge);
        if (conditional || SVFUtil::isa<CallCFGEdge>(edge))
        {
            shouldMerge = true;
        }
        else if (SVFUtil::isa<RetCFGEdge>(edge))
        {
            shouldMerge = Options::HandleRecur() == Base::TOP;
            if (!shouldMerge)
            {
                const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node);
                shouldMerge = returnSite &&
                              this->hasAbsState(returnSite->getCallICFGNode());
            }
        }
        if (!shouldMerge)
            continue;

        const auto refinementIterator = refinementTrace_.find(predecessor);
        const bool hasConditional = conditional && conditional->getCondition();
        const bool needsRefinement =
            hasConditional || refinementIterator != refinementTrace_.end();
        std::optional<DenseState> refinement;
        if (needsRefinement)
        {
            refinement = refinementIterator != refinementTrace_.end()
                             ? refinementIterator->second
                             : this->topState();
            if (hasConditional)
                this->assumeBranch(conditional, *refinement);
            if (refinement->isBottom())
                continue;
        }

        DenseState source = this->state(predecessor);
        filterPropagatedState(source);
        if (hasConditional)
            collectMemoryBranchRefinement(conditional, source);

        {
            PhaseTimer joinTimer(sparseProfile_.stateJoin,
                                 Options::AESparseProfile());
            merged.joinWith(source);
        }
        if (!refinement || refinement->isTop())
        {
            refinementIsTop = true;
            mergedRefinement.reset();
        }
        else if (!refinementIsTop)
        {
            forgetMemoryValues(*refinement);
            if (!mergedRefinement)
                mergedRefinement = std::move(*refinement);
            else
                mergedRefinement->joinWith(*refinement);
        }
        hasFeasiblePredecessor = true;
    }

    if (!hasFeasiblePredecessor)
        return false;
    if (mergedRefinement && !refinementIsTop && !mergedRefinement->isTop())
    {
        refinementTrace_.insert_or_assign(node, *mergedRefinement);
        applyScalarRefinement(merged, *mergedRefinement);
    }
    else
    {
        refinementTrace_.erase(node);
    }
    this->denseTrace_.insert_or_assign(node, std::move(merged));
    return true;
}

std::unique_ptr<AD::AbstractDomain> NativeSemiSparseAbstractInterpretation::
    cloneCycleHeadState(const ICFGCycleWTO* cycle)
{
    PhaseTimer timer(sparseProfile_.cycle, Options::AESparseProfile());
    const ICFGNode* head = cycle->head()->getICFGNode();
    DenseState snapshot = this->state(head);
    for (const ValVar* value : this->preAnalysis->getCycleValVars(cycle))
    {
        if (!value || !this->adapter_.contains(*value))
            continue;
        this->assignValue(snapshot, this->adapter_.variable(*value),
                          getInterval(value, head), getAddressSet(value, head));
    }
    return std::make_unique<DenseState>(std::move(snapshot));
}

void NativeSemiSparseAbstractInterpretation::scatterCycleValues(
    const ICFGCycleWTO* cycle, const DenseState& cycleState)
{
    for (const ValVar* value : this->preAnalysis->getCycleValVars(cycle))
    {
        if (!value || !this->adapter_.contains(*value))
            continue;
        const AD::Variable variable = this->adapter_.variable(*value);
        updateValue(value,
                    value->isPointer() ? AD::Interval::bottom()
                                       : cycleState.numerical().bound(variable),
                    value->isPointer()
                        ? cycleState.addresses().addressSet(variable)
                        : AD::AddressSet::bottom(),
                    cycle->head()->getICFGNode());
    }
}

bool NativeSemiSparseAbstractInterpretation::widenCycleState(
    const AD::AbstractDomain& previous, const AD::AbstractDomain& current,
    const ICFGCycleWTO* cycle)
{
    PhaseTimer timer(sparseProfile_.cycle, Options::AESparseProfile());
    const bool fixpoint = Base::widenCycleState(previous, current, cycle);
    scatterCycleValues(cycle, this->state(cycle->head()->getICFGNode()));
    finalizeAbstractState(cycle->head()->getICFGNode());
    return fixpoint;
}

bool NativeSemiSparseAbstractInterpretation::narrowCycleState(
    const AD::AbstractDomain& previous, const AD::AbstractDomain& current,
    const ICFGCycleWTO* cycle)
{
    PhaseTimer timer(sparseProfile_.cycle, Options::AESparseProfile());
    const bool fixpoint = Base::narrowCycleState(previous, current, cycle);
    if (!fixpoint)
    {
        scatterCycleValues(cycle, this->state(cycle->head()->getICFGNode()));
    }
    finalizeAbstractState(cycle->head()->getICFGNode());
    return fixpoint;
}

namespace
{

bool hasRedefinitionOf(const ICFGNode* node, const IndirectSVFGEdge* edge)
{
    for (const VFGNode* valueFlowNode : node->getVFGNodes())
    {
        if (SVFUtil::isa<StoreVFGNode>(valueFlowNode) &&
            valueFlowNode->getDefSVFVars().intersects(edge->getPointsTo()))
            return true;
    }
    return false;
}

} // namespace

NativeFullSparseAbstractInterpretation::NativeFullSparseAbstractInterpretation()
{
    PhaseTimer timer(this->sparseProfile_.svfgBuild,
                     Options::AESparseProfile());
    svfgBuilder_ = std::make_unique<SVFGBuilder>(true);
    svfgBuilder_->buildFullSVFG(this->preAnalysis->getPointerAnalysis());
}

NativeFullSparseAbstractInterpretation::
    ~NativeFullSparseAbstractInterpretation() = default;

const char* NativeFullSparseAbstractInterpretation::sparseProfileMode() const
{
    return "full";
}

void NativeFullSparseAbstractInterpretation::filterPropagatedState(
    DenseState& denseState) const
{
    PhaseTimer timer(this->sparseProfile_.stateFiltering,
                     Options::AESparseProfile());
    this->forgetActiveScalarValues(denseState);
    for (AD::Variable variable : nonDefaultVariables(denseState))
    {
        const ObjVar* object = this->adapter_.contentObject(variable);
        if (object && !SVFUtil::isa<GepObjVar>(object))
            this->forgetValue(denseState, variable);
    }
}

void NativeFullSparseAbstractInterpretation::collectMemoryBranchRefinement(
    const IntraCFGEdge* edge, DenseState& state)
{
    this->collectBranchRefinement(edge, state);
}

void NativeFullSparseAbstractInterpretation::recordBranchRefinement(
    NodeID objectId, const AD::Interval& narrowed, AD::AbstractDomain&,
    const ICFGNode*, const ICFGNode* successor)
{
    if (narrowed.isBottom())
        return;
    auto& refinements = memoryRefinementTrace_[successor];
    const auto iterator = refinements.find(objectId);
    if (iterator == refinements.end())
        refinements.emplace(objectId, narrowed);
    else
        iterator->second.joinWith(narrowed);
}

void NativeFullSparseAbstractInterpretation::storeValue(
    const ValVar* pointer, const AD::Interval& interval,
    const AD::AddressSet& valueAddresses, const ICFGNode* node)
{
    const AD::AddressSet addresses = Base::getAddressSet(pointer, node);
    auto refinement = memoryRefinementTrace_.find(node);
    if (refinement != memoryRefinementTrace_.end() && !addresses.isTop())
    {
        for (AD::Location location : addresses)
            if (const ObjVar* object = this->objectAt(location))
                refinement->second.erase(object->getId());
    }
    Base::storeValue(pointer, interval, valueAddresses, node);
}

bool NativeFullSparseAbstractInterpretation::mergeStatesFromPredecessors(
    const ICFGNode* node)
{
    memoryRefinementTrace_.erase(node);
    if (!Base::mergeStatesFromPredecessors(node))
        return false;
    // A direct object constraint collected from one incoming branch cannot be
    // applied after another incoming path has joined without that constraint.
    // Inherited constraints below already implement the precise all-preds
    // intersection rule; discard edge-local constraints at explicit merges.
    if (node->getInEdges().size() > 1)
        memoryRefinementTrace_.erase(node);
    pullObjectValueFlows(node);
    propagateAndApplyMemoryRefinement(node);
    return true;
}

void NativeFullSparseAbstractInterpretation::pullObjectValueFlows(
    const ICFGNode* node)
{
    PhaseTimer timer(this->sparseProfile_.objectPull,
                     Options::AESparseProfile());
    NodeBS denseLocalObjects;
    const DenseState& destination = this->state(node);
    for (AD::Variable variable : nonDefaultVariables(destination))
    {
        const ObjVar* object = this->adapter_.contentObject(variable);
        if (object && SVFUtil::isa<GepObjVar>(object))
            denseLocalObjects.set(object->getId());
    }
    NodeBS pulledObjects;

    for (const VFGNode* valueFlowNode : node->getVFGNodes())
    {
        for (auto edgeIterator = valueFlowNode->InEdgeBegin();
             edgeIterator != valueFlowNode->InEdgeEnd(); ++edgeIterator)
        {
            const auto* indirect =
                SVFUtil::dyn_cast<IndirectSVFGEdge>(*edgeIterator);
            if (!indirect ||
                !isIndirectSVFGEdgeFeasible(indirect, valueFlowNode))
                continue;

            const auto* sourceNode =
                SVFUtil::dyn_cast<SVFGNode>(indirect->getSrcNode());
            assert(sourceNode && sourceNode->getICFGNode() &&
                   "SVFG source must have an ICFG node");
            const ICFGNode* source = sourceNode->getICFGNode();
            if (!this->hasAbsState(source))
                continue;

            for (NodeID objectId : indirect->getPointsTo())
            {
                SVFVar* graphNode = this->svfir->getGNode(objectId);
                NodeBS objectsToPull;
                if (SVFUtil::isa<GepObjVar>(graphNode))
                    objectsToPull.set(objectId);
                else if (auto* base = SVFUtil::dyn_cast<BaseObjVar>(graphNode))
                    objectsToPull = this->svfir->getAllFieldsObjVars(base);
                else
                    objectsToPull.set(objectId);

                for (NodeID fieldId : objectsToPull)
                {
                    if (denseLocalObjects.test(fieldId))
                        continue;
                    const auto* object = SVFUtil::dyn_cast<ObjVar>(
                        this->svfir->getGNode(fieldId));
                    if (!object)
                        continue;

                    AD::Interval interval = AD::Interval::bottom();
                    AD::AddressSet addresses = AD::AddressSet::bottom();
                    if (pulledObjects.test(fieldId))
                    {
                        interval = Base::getInterval(object, node);
                        addresses = Base::getAddressSet(object, node);
                    }
                    interval.joinWith(Base::getInterval(object, source));
                    addresses.joinWith(Base::getAddressSet(object, source));
                    Base::updateValue(object, interval, addresses, node);
                    pulledObjects.set(fieldId);
                }
            }
        }
    }
}

bool NativeFullSparseAbstractInterpretation::isIntraEdgeBranchFeasible(
    const IntraCFGEdge* edge, const ICFGNode* source)
{
    return !edge->getCondition() || !this->hasAbsState(source) ||
           this->isBranchEdgeFeasibleAt(edge, source);
}

bool NativeFullSparseAbstractInterpretation::isIndirectSVFGEdgeFeasible(
    const IndirectSVFGEdge* edge, const VFGNode* destination)
{
    PhaseTimer timer(this->sparseProfile_.pathFeasibility,
                     Options::AESparseProfile());
    assert(edge && destination && "SVFG edge and destination must exist");
    const auto* sourceNode = SVFUtil::dyn_cast<SVFGNode>(edge->getSrcNode());
    assert(sourceNode && "indirect SVFG edge must have an SVFG source");
    const ICFGNode* source = sourceNode->getICFGNode();
    const ICFGNode* target = destination->getICFGNode();
    assert(source && target && "SVFG endpoints must have ICFG nodes");

    const FunObjVar* function = source->getFun();
    if (source == target || !function || function != target->getFun())
        return true;

    std::deque<const ICFGNode*> worklist;
    Set<const ICFGNode*> visited;
    worklist.push_back(source);
    visited.insert(source);
    while (!worklist.empty())
    {
        const ICFGNode* current = worklist.front();
        worklist.pop_front();
        if (current != source && hasRedefinitionOf(current, edge))
            continue;

        if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(current))
        {
            const ICFGNode* successor = call->getRetICFGNode();
            if (successor && successor->getFun() == function)
            {
                if (successor == target)
                    return true;
                if (visited.insert(successor).second)
                    worklist.push_back(successor);
            }
        }

        for (const ICFGEdge* cfgEdge : current->getOutEdges())
        {
            const auto* intra = SVFUtil::dyn_cast<IntraCFGEdge>(cfgEdge);
            const ICFGNode* successor = intra ? intra->getDstNode() : nullptr;
            if (!successor || successor->getFun() != function ||
                !isIntraEdgeBranchFeasible(intra, current))
                continue;
            if (successor == target)
                return true;
            if (visited.insert(successor).second)
                worklist.push_back(successor);
        }
    }
    return false;
}

void NativeFullSparseAbstractInterpretation::propagateAndApplyMemoryRefinement(
    const ICFGNode* node)
{
    PhaseTimer timer(this->sparseProfile_.memoryRefinement,
                     Options::AESparseProfile());
    Map<NodeID, AD::Interval> inherited;
    bool canInherit = true;
    bool first = true;
    for (const ICFGEdge* edge : node->getInEdges())
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (!this->hasAbsState(predecessor))
            continue;
        const auto predecessorRefinement =
            memoryRefinementTrace_.find(predecessor);
        if (predecessorRefinement == memoryRefinementTrace_.end())
        {
            canInherit = false;
            break;
        }
        if (first)
        {
            inherited = predecessorRefinement->second;
            first = false;
            continue;
        }
        for (auto iterator = inherited.begin(); iterator != inherited.end();)
        {
            const auto incoming =
                predecessorRefinement->second.find(iterator->first);
            if (incoming == predecessorRefinement->second.end())
                iterator = inherited.erase(iterator);
            else
            {
                iterator->second.joinWith(incoming->second);
                ++iterator;
            }
        }
    }

    if (canInherit && !first)
    {
        auto& refinements = memoryRefinementTrace_[node];
        for (const auto& [objectId, constraint] : inherited)
        {
            const auto current = refinements.find(objectId);
            if (current == refinements.end())
                refinements.emplace(objectId, constraint);
            else
                current->second.meetWith(constraint);
        }
    }

    const auto refinements = memoryRefinementTrace_.find(node);
    if (refinements == memoryRefinementTrace_.end())
        return;
    DenseState& denseState = this->ensureState(node);
    for (const auto& [objectId, constraint] : refinements->second)
    {
        const auto* object =
            SVFUtil::dyn_cast<ObjVar>(this->svfir->getGNode(objectId));
        if (!object)
            continue;
        const AD::Variable content = this->adapter_.contentVariable(*object);
        if (!object->isPointer())
            this->constrainInterval(denseState, content, constraint);
    }
}

} // namespace SVF
