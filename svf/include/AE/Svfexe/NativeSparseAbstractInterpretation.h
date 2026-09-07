//===- NativeSparseAbstractInterpretation.h -- Domain sparse AE -*- C++ -*-===//

#ifndef SVF_AE_NATIVE_SPARSE_ABSTRACT_INTERPRETATION_H
#define SVF_AE_NATIVE_SPARSE_ABSTRACT_INTERPRETATION_H

#include <cstdint>
#include <memory>
#include <optional>

#include "AE/Svfexe/DenseAbstractInterpretation.h"

namespace SVF
{

class IndirectSVFGEdge;
class SVFGBuilder;
class VFGNode;

/// Semi-sparse AE backed by BoxProgramState. Box values use one module-wide
/// scalar carrier. Persistent ICFG states carry memory and lifetime values,
/// while transfers materialize scalar operands only temporarily.
class NativeSemiSparseAbstractInterpretation
    : public DenseAbstractInterpretation
{
public:
    using Base = DenseAbstractInterpretation;
    using DenseState = typename Base::DenseState;

    NativeSemiSparseAbstractInterpretation();
    ~NativeSemiSparseAbstractInterpretation() override = default;
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

    void materializeValue(DenseState& state, const ValVar* value,
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
    virtual void filterPropagatedState(DenseState& state) const;

    /// Optional memory refinement hook after a conditional edge has been
    /// proven feasible by the native numerical state.
    virtual void collectMemoryBranchRefinement(const IntraCFGEdge* edge,
                                               DenseState& state);

    DenseState& scalarState();
    const DenseState* findScalarState() const;
    DenseState flowState(bool bottom = false) const;
    void forgetActiveScalarValues(DenseState& state) const;
    void forgetMemoryValues(DenseState& state) const;
    void applyScalarRefinement(DenseState& state, const DenseState& checkpoint);
    void scatterCycleValues(const ICFGCycleWTO* cycle, const DenseState& state);
    virtual const char* sparseProfileMode() const;
    void reportSparseProfile() const;

    Map<const ICFGNode*, DenseState> refinementTrace_;
    std::optional<DenseState> scalarState_;
    mutable SparsePhaseProfile sparseProfile_;
};

/// Full-sparse AE backed by BoxProgramState. Scalar SSA values remain at
/// definition sites as in semi-sparse mode. Base/Dummy ObjVar contents move
/// along MemorySSA/SVFG def-use edges; GepObjVar snapshots and lifetime facts
/// continue to flow along the ICFG because they are not fully represented by
/// those edges.
class NativeFullSparseAbstractInterpretation
    : public NativeSemiSparseAbstractInterpretation
{
public:
    using Base = NativeSemiSparseAbstractInterpretation;
    using DenseState = typename Base::DenseState;

    NativeFullSparseAbstractInterpretation();
    ~NativeFullSparseAbstractInterpretation() override;

protected:
    bool mergeStatesFromPredecessors(const ICFGNode* node) override;
    void storeValue(const ValVar* pointer,
                    const AbstractDomain::Interval& interval,
                    const AbstractDomain::AddressSet& addresses,
                    const ICFGNode* node) override;
    void filterPropagatedState(DenseState& state) const override;
    void collectMemoryBranchRefinement(const IntraCFGEdge* edge,
                                       DenseState& state) override;
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

#endif // SVF_AE_NATIVE_SPARSE_ABSTRACT_INTERPRETATION_H
