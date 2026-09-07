//===- DenseAbstractInterpretation.h -- Domain-backed dense AE -*- C++ -*-===//

#ifndef SVF_AE_DENSE_ABSTRACT_INTERPRETATION_H
#define SVF_AE_DENSE_ABSTRACT_INTERPRETATION_H

#include "AE/Core/BoxProgramState.h"
#include "AE/Core/NumericalDomain.h"
#include "AE/Svfexe/AbstractInterpretation.h"
#include "AE/Svfexe/SVFIRAdapter.h"

namespace SVF
{

/// Native dense AE storage backed by one complete AbstractDomain state per
/// ICFG node. Values, memory, lifetimes, joins, widening, and
/// fixpoint checks all operate on BoxProgramState; no compatibility trace
/// is maintained by this implementation.
class DenseAbstractInterpretation : public AbstractInterpretation
{
public:
    using DenseState = AbstractDomain::BoxProgramState;

    DenseAbstractInterpretation();
    ~DenseAbstractInterpretation() override = default;
    const AbstractDomain::AbstractDomain& getAbstractState(
        const ICFGNode* node) const override;
    bool hasAbsState(const ICFGNode* node) const override;
    AbstractDomain::Location locationOf(const ObjVar* object) const override;
    const ObjVar* objectAt(AbstractDomain::Location location) const override;

    AbstractDomain::Interval getInterval(const ValVar* var,
                                         const ICFGNode* node) override;
    AbstractDomain::Interval getInterval(const ObjVar* var,
                                         const ICFGNode* node) override;
    AbstractDomain::Interval getInterval(const SVFVar* var,
                                         const ICFGNode* node) override;
    AbstractDomain::AddressSet getAddressSet(const ValVar* var,
                                             const ICFGNode* node) override;
    AbstractDomain::AddressSet getAddressSet(const ObjVar* var,
                                             const ICFGNode* node) override;
    AbstractDomain::AddressSet getAddressSet(const SVFVar* var,
                                             const ICFGNode* node) override;

    bool hasAbsValue(const ValVar* var, const ICFGNode* node) const override;
    bool hasAbsValue(const ObjVar* var, const ICFGNode* node) const override;
    bool hasAbsValue(const SVFVar* var, const ICFGNode* node) const override;

    void updateValue(const ValVar* var,
                     const AbstractDomain::Interval& interval,
                     const AbstractDomain::AddressSet& addresses,
                     const ICFGNode* node) override;
    void updateValue(const ObjVar* var,
                     const AbstractDomain::Interval& interval,
                     const AbstractDomain::AddressSet& addresses,
                     const ICFGNode* node) override;
    void updateValue(const SVFVar* var,
                     const AbstractDomain::Interval& interval,
                     const AbstractDomain::AddressSet& addresses,
                     const ICFGNode* node) override;

    AbstractDomain::Interval getMemoryInterval(
        AbstractDomain::Location location, const ICFGNode* node) override;
    AbstractDomain::AddressSet getMemoryAddressSet(
        AbstractDomain::Location location, const ICFGNode* node) override;
    bool hasMemoryValue(AbstractDomain::Location location,
                        const ICFGNode* node) const override;
    void updateMemoryValue(AbstractDomain::Location location,
                           const AbstractDomain::Interval& interval,
                           const AbstractDomain::AddressSet& addresses,
                           const ICFGNode* node) override;
    void markFreedMemory(AbstractDomain::Location location,
                         const ICFGNode* node) override;
    bool isFreedMemory(AbstractDomain::Location location,
                       const ICFGNode* node) const override;

    void loadValue(const ValVar* pointer, AbstractDomain::Interval& interval,
                   AbstractDomain::AddressSet& addresses,
                   const ICFGNode* node) override;
    void storeValue(const ValVar* pointer,
                    const AbstractDomain::Interval& interval,
                    const AbstractDomain::AddressSet& addresses,
                    const ICFGNode* node) override;

protected:
    void handleGlobalNode() override;
    void initializeObjectValue(const ObjVar* object,
                               AbstractDomain::Interval& interval,
                               AbstractDomain::AddressSet& addresses,
                               const ICFGNode* node) override;
    void resetAbstractState(const ICFGNode* node) override;
    void copyAbstractState(const ICFGNode* source,
                           const ICFGNode* destination) override;
    std::unique_ptr<AbstractDomain::AbstractDomain> cloneAbstractState(
        const ICFGNode* node) const override;
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
    bool mergeStatesFromPredecessors(const ICFGNode* node) override;
    bool isBranchEdgeFeasibleAt(const IntraCFGEdge* edge,
                                const ICFGNode* predecessor) override;
    void recordBranchRefinement(NodeID objectId,
                                const AbstractDomain::Interval& narrowed,
                                AbstractDomain::AbstractDomain& state,
                                const ICFGNode* loadNode,
                                const ICFGNode* successor) override;

protected:
    DenseState& ensureState(const ICFGNode* node);
    const DenseState& state(const ICFGNode* node) const;
    DenseState topState() const;
    DenseState bottomState() const;

    void assignValue(DenseState& state, AbstractDomain::Variable variable,
                     const AbstractDomain::Interval& interval,
                     const AbstractDomain::AddressSet& addresses);
    void assignMemoryValue(DenseState& state,
                           AbstractDomain::Variable content,
                           const AbstractDomain::Interval& interval,
                           const AbstractDomain::AddressSet& addresses);
    void assignInterval(DenseState& state, AbstractDomain::Variable variable,
                        const AbstractDomain::Interval& interval);
    void constrainInterval(DenseState& state, AbstractDomain::Variable variable,
                           const AbstractDomain::Interval& interval);
    virtual void materializeValue(DenseState& state, const ValVar* value,
                                  const ICFGNode* node);
    void forgetValue(DenseState& state,
                     AbstractDomain::Variable variable) const;
    void assumeBranch(const IntraCFGEdge* edge, DenseState& state);

    SVFIRAdapter adapter_;
    Map<const ICFGNode*, DenseState> denseTrace_;
};

} // namespace SVF

#endif // SVF_AE_DENSE_ABSTRACT_INTERPRETATION_H
