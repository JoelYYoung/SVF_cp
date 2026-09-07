//===- SparseAbstractInterpretation.h -- Sparse box/address AE -*- C++ -*-===//
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

#ifndef SVF_AE_SPARSE_ABSTRACT_INTERPRETATION_H
#define SVF_AE_SPARSE_ABSTRACT_INTERPRETATION_H

#include <cstdint>
#include <memory>
#include <optional>

#include "AE/Svfexe/AbstractInterpretation.h"

namespace SVF
{

class IndirectSVFGEdge;
class SVFGBuilder;
class VFGNode;

/// Semi-sparse AE backed by BoxAddressDomain. Box values use one module-wide
/// scalar carrier. Persistent ICFG states carry memory and lifetime values,
/// while transfers materialize scalar operands only temporarily.
class SemiSparseAbstractInterpretation : public AbstractInterpretation
{
public:
    using Base = AbstractInterpretation;
    using State = typename Base::State;

    SemiSparseAbstractInterpretation();
    ~SemiSparseAbstractInterpretation() override = default;
    void runOnModule() override;
    const AbstractDomain::AbstractDomain* getScalarAbstractState()
        const override;

protected:
    void handleGlobalNode() override;
    struct PhaseMetric
    {
        std::uint64_t calls = 0;
        std::uint64_t nanoseconds = 0;
    };

    struct SparsePhaseProfile
    {
        PhaseMetric total;
        PhaseMetric globalInitialization;
        PhaseMetric statementTransfer;
        PhaseMetric addressTransfer;
        PhaseMetric copyTransfer;
        PhaseMetric gepTransfer;
        PhaseMetric loadTransfer;
        PhaseMetric storeTransfer;
        PhaseMetric otherTransfer;
        PhaseMetric memoryStore;
        PhaseMetric stateCopy;
        PhaseMetric stateMerge;
        PhaseMetric stateJoin;
        PhaseMetric stateEquivalence;
        PhaseMetric scalarMaterialization;
        PhaseMetric scalarRefinement;
        PhaseMetric stateFiltering;
        PhaseMetric cycle;
        PhaseMetric svfgBuild;
        PhaseMetric objectPull;
        PhaseMetric pathFeasibility;
        PhaseMetric memoryRefinement;
        std::uint64_t unknownStoreCalls = 0;
        std::uint64_t unknownStoreCells = 0;
    };

    AbstractDomain::Interval getInterval(const ValVar* var,
                                         const ICFGNode* node) override;
    AbstractDomain::AddressSet getAddressSet(const ValVar* var,
                                             const ICFGNode* node) override;
    using Base::getAddressSet;
    using Base::getInterval;
    bool hasAbsValue(const ValVar* var, const ICFGNode* node) const override;
    using Base::hasAbsValue;
    void updateValue(const ValVar* var,
                     const AbstractDomain::Interval& interval,
                     const AbstractDomain::AddressSet& addresses,
                     const ICFGNode* node) override;
    using Base::updateValue;
    void handleSVFStatement(const SVFStmt* statement) override;

    void copyAbstractState(const ICFGNode* source,
                           const ICFGNode* destination) override;
    void resetAbstractState(const ICFGNode* node) override;
    void finalizeAbstractState(const ICFGNode* node) override;
    bool mergeStatesFromPredecessors(const ICFGNode* node) override;
    bool isAbstractStateEquivalent(
        const ICFGNode* node,
        const AbstractDomain::AbstractDomain& snapshot) const override;

    std::unique_ptr<AbstractDomain::AbstractDomain> cloneCycleHeadState(
        const ICFGCycleWTO* cycle) override;
    bool widenCycleState(const AbstractDomain::AbstractDomain& previous,
                         const AbstractDomain::AbstractDomain& current,
                         const ICFGCycleWTO* cycle) override;
    bool narrowCycleState(const AbstractDomain::AbstractDomain& previous,
                          const AbstractDomain::AbstractDomain& current,
                          const ICFGCycleWTO* cycle) override;

    void materializeValue(State& state, const ValVar* value,
                          const ICFGNode* node) override;
    void loadValue(const ValVar* pointer, AbstractDomain::Interval& interval,
                   AbstractDomain::AddressSet& addresses,
                   const ICFGNode* node) override;
    void storeValue(const ValVar* pointer,
                    const AbstractDomain::Interval& interval,
                    const AbstractDomain::AddressSet& addresses,
                    const ICFGNode* node) override;

    /// Keep only the state facets that should flow along ordinary ICFG
    /// edges. Full-sparse overrides this to remove MemorySSA-managed objects.
    virtual void filterPropagatedState(State& state) const;

    /// Optional memory refinement hook after a conditional edge has been
    /// proven feasible by the native numerical state.
    virtual void collectMemoryBranchRefinement(const IntraCFGEdge* edge,
                                               State& state);

    State& scalarState();
    const State* findScalarState() const;
    State flowState(bool bottom = false) const;
    void forgetActiveScalarValues(State& state) const;
    void forgetMemoryValues(State& state) const;
    void applyScalarRefinement(State& state, const State& checkpoint);
    void scatterCycleValues(const ICFGCycleWTO* cycle, const State& state);
    virtual const char* sparseProfileMode() const;
    void reportSparseProfile() const;

    Map<const ICFGNode*, State> refinementTrace_;
    std::optional<State> scalarState_;
    mutable SparsePhaseProfile sparseProfile_;
};

/// Full-sparse AE backed by BoxAddressDomain. Scalar SSA values remain at
/// definition sites as in semi-sparse mode. Base/Dummy ObjVar contents move
/// along MemorySSA/SVFG def-use edges; GepObjVar snapshots and lifetime facts
/// continue to flow along the ICFG because they are not fully represented by
/// those edges.
class FullSparseAbstractInterpretation
    : public SemiSparseAbstractInterpretation
{
public:
    using Base = SemiSparseAbstractInterpretation;
    using State = typename Base::State;

    FullSparseAbstractInterpretation();
    ~FullSparseAbstractInterpretation() override;

protected:
    bool mergeStatesFromPredecessors(const ICFGNode* node) override;
    void storeValue(const ValVar* pointer,
                    const AbstractDomain::Interval& interval,
                    const AbstractDomain::AddressSet& addresses,
                    const ICFGNode* node) override;
    void filterPropagatedState(State& state) const override;
    void collectMemoryBranchRefinement(const IntraCFGEdge* edge,
                                       State& state) override;
    void recordBranchRefinement(NodeID objectId,
                                const AbstractDomain::Interval& narrowed,
                                AbstractDomain::AbstractDomain& state,
                                const ICFGNode* loadNode,
                                const ICFGNode* successor) override;

private:
    const char* sparseProfileMode() const override;
    void pullObjectValueFlows(const ICFGNode* node);
    bool isIndirectSVFGEdgeFeasible(const IndirectSVFGEdge* edge,
                                    const VFGNode* destination);
    bool isIntraEdgeBranchFeasible(const IntraCFGEdge* edge,
                                   const ICFGNode* source);
    void propagateAndApplyMemoryRefinement(const ICFGNode* node);

    Map<const ICFGNode*, Map<NodeID, AbstractDomain::Interval>>
        memoryRefinementTrace_;
    std::unique_ptr<SVFGBuilder> svfgBuilder_;
};

} // namespace SVF

#endif // SVF_AE_SPARSE_ABSTRACT_INTERPRETATION_H
