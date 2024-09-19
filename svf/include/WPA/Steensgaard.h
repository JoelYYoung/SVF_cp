/*
 * Steensgaard.h
 *
 *  Created on: 2 Feb. 2021
 *      Author: Yulei Sui
 */

#ifndef INCLUDE_WPA_STEENSGAARD_H_
#define INCLUDE_WPA_STEENSGAARD_H_

#include "WPA/Andersen.h"

namespace SVF
{
/*!
 * Equivalence-based Pointer Analysis
 */
typedef WPASolver<ConstraintGraph*> WPAConstraintSolver;

class Steensgaard : public AndersenBase
{

public:
    typedef Map<NodeID, NodeID> NodeToEquivClassMap;
    typedef Map<NodeID, Set<NodeID>> NodeToSubsMap;
    typedef OrderedMap<const CallICFGNode*, NodeID> CallSite2DummyValPN;

    /// Constructor
    Steensgaard(SVFIR* _pag) : AndersenBase(_pag, Steensgaard_WPA, true) {}

    /// Create an singleton instance
    static Steensgaard* createSteensgaard(SVFIR* _pag)
    {
        if (steens == nullptr)
        {
            steens = new Steensgaard(_pag);
            steens->analyze();
            return steens;
        }
        return steens;
    }
    static void releaseSteensgaard()
    {
        if (steens)
            delete steens;
        steens = nullptr;
    }

    virtual void solveWorklist() override;

    void processAllAddr();

    void ecUnion(NodeID id, NodeID ec);

    /// Methods for support type inquiry through isa, cast, and dyn_cast:
    //@{
    static inline bool classof(const Steensgaard*)
    {
        return true;
    }
    static inline bool classof(const AndersenBase* pta)
    {
        return (pta->getAnalysisTy() == Steensgaard_WPA);
    }
    static inline bool classof(const PointerAnalysis* pta)
    {
        return (pta->getAnalysisTy() == Steensgaard_WPA);
    }
    //@}

    /// Operation of points-to set
    virtual inline const PointsTo& getPts(NodeID id) override
    {
        return getPTDataTy()->getPts(getEC(id));
    }
    /// pts(id) = pts(id) U target
    virtual inline bool unionPts(NodeID id, const PointsTo& target) override
    {
        id = getEC(id);
        return getPTDataTy()->unionPts(id, target);
    }
    /// pts(id) = pts(id) U pts(ptd)
    virtual inline bool unionPts(NodeID id, NodeID ptd) override
    {
        id = getEC(id);
        ptd = getEC(ptd);
        return getPTDataTy()->unionPts(id, ptd);
    }

    /// API for equivalence class operations
    /// Every constraint node maps to an unique equivalence class EC
    /// An equivalence class has a set of sub constraint nodes.
    inline NodeID getEC(NodeID id) const
    {
        NodeToEquivClassMap::const_iterator it = nodeToECMap.find(id);
        if (it == nodeToECMap.end())
            return id;
        else
            return it->second;
    }
    void setEC(NodeID node, NodeID rep);

    inline Set<NodeID>& getSubNodes(NodeID id)
    {
        nodeToSubsMap[id].insert(id);
        return nodeToSubsMap[id];
    }
    inline void addSubNode(NodeID node, NodeID sub)
    {
        nodeToSubsMap[node].insert(sub);
    }

    /// Add copy edge on constraint graph
    virtual inline bool addCopyEdge(NodeID src, NodeID dst)
    {
        return consCG->addCopyCGEdge(src, dst);
    }

    /// Worklist operations
    //@{
    inline NodeID popFromWorklist() override
    {
        return getEC(worklist.pop());
    }

    inline void pushIntoWorklist(NodeID id) override
    {
        worklist.push(getEC(id));
    }
    //@}

protected:
    CallSite2DummyValPN
    callsite2DummyValPN; ///< Map an instruction to a dummy obj which
    ///< created at an indirect callsite, which invokes
    ///< a heap allocator
    void heapAllocatorViaIndCall(const CallICFGNode* cs, NodePairSet& cpySrcNodes);

    /// Update call graph for the input indirect callsites
    virtual bool updateCallGraph(const CallSiteToFunPtrMap& callsites) override;

    /// Connect formal and actual parameters for indirect callsites
    void connectCaller2CalleeParams(const CallICFGNode* cs, const SVFFunction* F,
                                    NodePairSet& cpySrcNodes);

    /// Connect formal and actual parameters for indirect forksites
    void connectCaller2ForkedFunParams(const CallICFGNode* cs, const SVFFunction* F,
                                    NodePairSet& cpySrcNodes);

private:
    static Steensgaard* steens; // static instance
    NodeToEquivClassMap nodeToECMap;
    NodeToSubsMap nodeToSubsMap;
};

} // namespace SVF

#endif /* INCLUDE_WPA_STEENSGAARD_H_ */
