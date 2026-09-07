//===- BoxAddressAbstractInterpretation.h -- Box/address AE -*- C++ -*-===//
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
// Contributors: Xiao Cheng, Jiawei Wang, Jiawei Yang
//
//===----------------------------------------------------------------------===//

#ifndef SVF_AE_BOX_ADDRESS_ABSTRACT_INTERPRETATION_H
#define SVF_AE_BOX_ADDRESS_ABSTRACT_INTERPRETATION_H

#include "AE/Core/BoxAddressDomain.h"
#include "AE/Core/NumericalDomain.h"
#include "AE/Svfexe/AbstractInterpretation.h"
#include "AE/Svfexe/SVFIRAdapter.h"

namespace SVF
{

/// Box/address transfer and state implementation. Its default placement keeps
/// one complete state per ICFG node; sparse subclasses reuse the same transfer
/// semantics while overriding where scalar and memory facts are stored.
class BoxAddressAbstractInterpretation : public AbstractInterpretation
{
public:
    using State = AbstractDomain::BoxAddressDomain;

    BoxAddressAbstractInterpretation();
    ~BoxAddressAbstractInterpretation() override = default;
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
    State& ensureState(const ICFGNode* node);
    const State& state(const ICFGNode* node) const;
    State topState() const;
    State bottomState() const;

    void assignValue(State& state, AbstractDomain::Variable variable,
                     const AbstractDomain::Interval& interval,
                     const AbstractDomain::AddressSet& addresses);
    void assignMemoryValue(State& state,
                           AbstractDomain::Variable content,
                           const AbstractDomain::Interval& interval,
                           const AbstractDomain::AddressSet& addresses);
    void assignInterval(State& state, AbstractDomain::Variable variable,
                        const AbstractDomain::Interval& interval);
    void constrainInterval(State& state, AbstractDomain::Variable variable,
                           const AbstractDomain::Interval& interval);
    virtual void materializeValue(State& state, const ValVar* value,
                                  const ICFGNode* node);
    void forgetValue(State& state,
                     AbstractDomain::Variable variable) const;
    void assumeBranch(const IntraCFGEdge* edge, State& state);

    SVFIRAdapter adapter_;
    Map<const ICFGNode*, State> stateTrace_;
};

} // namespace SVF

#endif // SVF_AE_BOX_ADDRESS_ABSTRACT_INTERPRETATION_H
