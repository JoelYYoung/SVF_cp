//===- BoxAEIntegrationTest.cpp -- Box-backed AE integration test -------===//
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

#include "AE/Core/BoxAddressDomain.h"
#include "AE/Core/NumericalDomain.h"
#include "AE/Svfexe/AbstractInterpretation.h"
#include "AE/Svfexe/SVFIRAdapter.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace SVF;

namespace
{
namespace AD = SVF::AbstractDomain;
using BoxAddressDomain = AD::BoxAddressDomain;

const SVFVar* findValue(const SVFIR& graph, const std::string& name)
{
    for (auto iterator = graph.begin(); iterator != graph.end(); ++iterator)
    {
        const SVFVar* value = iterator->second;
        const std::string& candidate = value->getValueName();
        if (candidate == name || candidate.rfind(name + " ", 0) == 0)
            return value;
    }
    return nullptr;
}

const BoxAddressDomain& requireBoxAddressDomain(
    const AD::AbstractDomain& state)
{
    if (!state.isDomain<BoxAddressDomain>())
        throw std::runtime_error("AE property is not BoxAddressDomain-backed");
    return static_cast<const BoxAddressDomain&>(state);
}

const BoxAddressDomain& stateForValue(AbstractInterpretation& analysis,
                                      const ValVar* value, const ICFGNode* node)
{
    if (const AD::AbstractDomain* scalar = analysis.getScalarAbstractState())
        return requireBoxAddressDomain(*scalar);
    return requireBoxAddressDomain(analysis.getAbstractState(node));
}

bool hasFiniteBounds(const AD::Interval& interval, s64_t lower, s64_t upper)
{
    return interval.lower().isFinite() && interval.upper().isFinite() &&
           interval.lower().value() == AD::Rational(lower) &&
           interval.upper().value() == AD::Rational(upper);
}

void validateAuthoritativeStorage(AbstractInterpretation& analysis)
{
    if (analysis.getAnalyzedNodes().empty())
        throw std::runtime_error("Box/address AE analyzed no ICFG nodes");
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
        requireBoxAddressDomain(analysis.getAbstractState(node));
}

struct StorageObservation
{
    std::size_t states = 0;
    std::size_t numericalFacts = 0;
    std::size_t numericalPages = 0;
    std::size_t addressFacts = 0;
    std::size_t addressPages8 = 0;
    std::size_t addressPages16 = 0;
    std::size_t addressPages32 = 0;
    std::size_t addressStatesAbove16 = 0;
    std::size_t addressFactsAbove16 = 0;
    std::size_t addressPages16Above16 = 0;
    std::size_t finitePointees = 0;
    std::size_t largestAddressSet = 0;
    std::vector<std::size_t> addressFactsPerState;
    std::vector<std::size_t> addressSetSizes;

    void observe(const BoxAddressDomain& state)
    {
        ++states;
        const std::vector<AD::Variable> numerical =
            state.numerical().constrainedVariables();
        numericalFacts += numerical.size();
        std::set<std::uint32_t> pages;
        for (AD::Variable variable : numerical)
            pages.insert(variable.id() / 64);
        numericalPages += pages.size();

        const std::vector<AD::Variable> pointers =
            state.addresses().nonDefaultVariables();
        addressFacts += pointers.size();
        addressFactsPerState.push_back(pointers.size());
        std::set<std::uint32_t> pages8;
        std::set<std::uint32_t> pages16;
        std::set<std::uint32_t> pages32;
        for (AD::Variable variable : pointers)
        {
            pages8.insert(variable.id() / 8);
            pages16.insert(variable.id() / 16);
            pages32.insert(variable.id() / 32);
            const AD::AddressSet addresses =
                state.addresses().addressSet(variable);
            if (addresses.isTop())
                throw std::runtime_error(
                    "Address non-default support contains Top");
            finitePointees += addresses.size();
            largestAddressSet = std::max(largestAddressSet, addresses.size());
            addressSetSizes.push_back(addresses.size());
        }
        addressPages8 += pages8.size();
        addressPages16 += pages16.size();
        addressPages32 += pages32.size();
        if (pointers.size() > 16)
        {
            ++addressStatesAbove16;
            addressFactsAbove16 += pointers.size();
            addressPages16Above16 += pages16.size();
        }
    }

    std::size_t percentile(double fraction) const
    {
        if (addressFactsPerState.empty())
            return 0;
        std::vector<std::size_t> sorted = addressFactsPerState;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t index = static_cast<std::size_t>(
                                      fraction * static_cast<double>(sorted.size() - 1));
        return sorted[index];
    }

    std::size_t setSizePercentile(double fraction) const
    {
        if (addressSetSizes.empty())
            return 0;
        std::vector<std::size_t> sorted = addressSetSizes;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t index = static_cast<std::size_t>(
                                      fraction * static_cast<double>(sorted.size() - 1));
        return sorted[index];
    }
};

struct StorageObservations
{
    StorageObservation flow;
    StorageObservation scalar;
};

struct VariablePopulation
{
    std::size_t pointerScalars = 0;
    std::size_t pointerContents = 0;
};

VariablePopulation observeVariablePopulation(const SVFIR& graph)
{
    VariablePopulation population;
    for (auto iterator = graph.begin(); iterator != graph.end(); ++iterator)
    {
        const SVFVar* value = iterator->second;
        if (const auto* scalar = SVFUtil::dyn_cast<ValVar>(value))
        {
            if (scalar->isPointer() &&
                    !scalar->isConstDataOrAggDataButNotNullPtr())
                ++population.pointerScalars;
        }
        else if (const auto* object = SVFUtil::dyn_cast<ObjVar>(value))
        {
            if (object->isPointer())
                ++population.pointerContents;
        }
    }
    return population;
}

StorageObservations observeStorage(AbstractInterpretation& analysis)
{
    StorageObservations observations;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
        observations.flow.observe(
            requireBoxAddressDomain(analysis.getAbstractState(node)));
    if (const AD::AbstractDomain* scalar = analysis.getScalarAbstractState())
        observations.scalar.observe(requireBoxAddressDomain(*scalar));
    return observations;
}

std::uint64_t semanticChecksum(AbstractInterpretation& analysis)
{
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    auto consume = [&](const std::string& value)
    {
        for (unsigned char byte : value)
        {
            hash ^= byte;
            hash *= prime;
        }
        hash ^= 0xffU;
        hash *= prime;
    };

    std::vector<const ICFGNode*> nodes(analysis.getAnalyzedNodes().begin(),
                                       analysis.getAnalyzedNodes().end());
    std::sort(nodes.begin(), nodes.end(),
              [](const ICFGNode* lhs, const ICFGNode* rhs)
    {
        return lhs->getId() < rhs->getId();
    });
    for (const ICFGNode* node : nodes)
    {
        consume(std::to_string(node->getId()));
        consume(analysis.getAbstractState(node).toString());
    }
    if (const AD::AbstractDomain* scalar = analysis.getScalarAbstractState())
    {
        consume("scalar");
        consume(scalar->toString());
    }
    return hash;
}

struct ResultChecksum
{
    std::uint64_t value;
    std::size_t records;
};

std::string keyPart(const std::string& value)
{
    return std::to_string(value.size()) + ':' + value;
}

std::string functionKey(const FunObjVar* function)
{
    return function ? keyPart(function->getName()) : "0:";
}

std::string floatingKey(double value)
{
    std::ostringstream stream;
    stream << std::hexfloat << value;
    return stream.str();
}

bool hasUpstreamComparableType(const SVFVar& value)
{
    return value.isPointer() ||
           (value.getType() &&
            value.getType()->getKind() == SVFType::SVFIntegerTy);
}

std::size_t basicBlockOrdinal(const SVFBasicBlock* block)
{
    if (!block)
        return 0;
    const FunObjVar* function = block->getFunction();
    std::size_t ordinal = 0;
    for (auto iterator = function->begin(); iterator != function->end();
            ++iterator, ++ordinal)
    {
        if (iterator->second == block)
            return ordinal;
    }
    return 0;
}

std::size_t instructionOrdinal(const ICFGNode* node)
{
    const SVFBasicBlock* block = node->getBB();
    if (!block)
        return 0;
    const auto& nodes = block->getICFGNodeList();
    const auto iterator = std::find(nodes.begin(), nodes.end(), node);
    if (iterator == nodes.end())
        return 0;
    return static_cast<std::size_t>(std::distance(nodes.begin(), iterator));
}

/// Identify an ICFG point without using allocation-order-dependent NodeIDs.
std::string programPointKey(const ICFGNode* node)
{
    if (!node)
        return "no-site";
    if (SVFUtil::isa<GlobalICFGNode>(node))
        return "global";
    std::string result = "fun=" + functionKey(node->getFun());
    result += ";kind=" + std::to_string(node->getNodeKind());
    if (SVFUtil::isa<FunEntryICFGNode>(node) ||
            SVFUtil::isa<FunExitICFGNode>(node))
        return result;
    if (const auto* ret = SVFUtil::dyn_cast<RetICFGNode>(node))
        return result + ";call=" +
               keyPart(programPointKey(ret->getCallICFGNode()));
    if (node->getBB())
    {
        result += ";bb=" + std::to_string(basicBlockOrdinal(node->getBB()));
        result += ";inst=" + std::to_string(instructionOrdinal(node));
    }
    return result;
}

/// Identify a memory object by its program meaning rather than its ObjVar ID.
std::string memoryObjectKey(const ObjVar* object)
{
    if (const auto* gep = SVFUtil::dyn_cast<GepObjVar>(object))
    {
        return "gep(" + memoryObjectKey(gep->getBaseObj()) + "," +
               std::to_string(gep->getConstantFieldIdx()) + ')';
    }
    const auto* base = SVFUtil::dyn_cast<BaseObjVar>(object);
    if (!base)
        return "obj(kind=" + std::to_string(object->getNodeKind()) +
               ";name=" + keyPart(object->getValueName()) + ')';
    if (SVFUtil::isa<ConstNullPtrObjVar>(base))
        return "null";
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntObjVar>(base))
        return "const-int(s=" + std::to_string(integer->getSExtValue()) +
               ";z=" + std::to_string(integer->getZExtValue()) + ')';
    if (const auto* floating = SVFUtil::dyn_cast<ConstFPObjVar>(base))
        return "const-fp(" + floatingKey(floating->getFPValue()) + ')';
    return "base(kind=" + std::to_string(base->getNodeKind()) +
           ";name=" + keyPart(base->getName()) +
           ";site=" + keyPart(programPointKey(base->getICFGNode())) + ')';
}

std::string canonicalAddressSet(const AD::AddressSet& addresses,
                                const AbstractInterpretation& analysis)
{
    if (addresses.isTop())
        return "top";
    if (addresses.isBottom())
        return "bottom";
    std::vector<std::string> objects;
    objects.reserve(addresses.size());
    for (AD::Location location : addresses.locations())
    {
        if (location.isNull())
        {
            objects.push_back("null");
            continue;
        }
        const ObjVar* object = analysis.objectAt(location);
        if (!object)
            throw std::runtime_error(
                "analysis address has no Location-to-ObjVar mapping");
        objects.push_back(memoryObjectKey(object));
    }
    std::sort(objects.begin(), objects.end());
    std::string result = "{";
    for (const std::string& object : objects)
    {
        if (result.size() != 1)
            result += ',';
        result += keyPart(object);
    }
    return result + '}';
}

std::string canonicalInterval(const AD::Interval& interval)
{
    if (interval.isTop())
        return "top";
    if (interval.isBottom())
        return "bottom";
    return interval.toString();
}

std::string valueVariableKey(const ValVar* value)
{
    if (const auto* argument = SVFUtil::dyn_cast<ArgValVar>(value))
    {
        return "arg(fun=" + functionKey(argument->getFunction()) +
               ";index=" + std::to_string(argument->getArgNo()) + ')';
    }
    if (const auto* gep = SVFUtil::dyn_cast<GepValVar>(value))
    {
        return "gep(" + valueVariableKey(gep->getBaseNode()) + "," +
               std::to_string(gep->getConstantFieldIdx()) + ')';
    }
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(value))
        return "const-int(s=" + std::to_string(integer->getSExtValue()) +
               ";z=" + std::to_string(integer->getZExtValue()) + ')';
    if (const auto* floating = SVFUtil::dyn_cast<ConstFPValVar>(value))
        return "const-fp(" + floatingKey(floating->getFPValue()) + ')';
    if (SVFUtil::isa<ConstNullPtrValVar>(value))
        return "null";
    return "var(kind=" + std::to_string(value->getNodeKind()) +
           ";name=" + keyPart(value->getValueName()) +
           ";fun=" + functionKey(value->getFunction()) +
           ";site=" + keyPart(programPointKey(value->getICFGNode())) + ')';
}

void addAnchorVariable(std::map<std::string, const ValVar*>& variables,
                       const SVFVar* value)
{
    if (const auto* scalar = SVFUtil::dyn_cast<ValVar>(value))
    {
        // Constants are immutable program inputs, not facts computed by the
        // analysis. Their domain-specific encoding is covered by dedicated
        // transfer tests and must not make the state-result projection differ.
        if (scalar->isConstDataOrAggDataButNotNullPtr() ||
                SVFUtil::isa<ConstNullPtrValVar>(scalar) ||
                SVFUtil::isa<DummyValVar>(scalar) ||
                !hasUpstreamComparableType(*scalar))
            return;
        const std::string key = valueVariableKey(scalar);
        const auto [iterator, inserted] = variables.emplace(key, scalar);
        if (!inserted && iterator->second != scalar &&
                !SVFUtil::isa<GepValVar>(iterator->second) &&
                !SVFUtil::isa<GepValVar>(scalar))
            throw std::runtime_error("non-unique semantic value key: " + key);
    }
}

std::map<std::string, const ValVar*> anchorVariables(const ICFGNode* node)
{
    std::map<std::string, const ValVar*> variables;
    for (const SVFStmt* statement : node->getSVFStmts())
    {
        if (const auto* assignment =
                    SVFUtil::dyn_cast<AssignStmt>(statement))
        {
            addAnchorVariable(variables, assignment->getRHSVar());
            addAnchorVariable(variables, assignment->getLHSVar());
        }
        else if (const auto* multi =
                     SVFUtil::dyn_cast<MultiOpndStmt>(statement))
        {
            addAnchorVariable(variables, multi->getRes());
            for (const ValVar* operand : multi->getOpndVars())
                addAnchorVariable(variables, operand);
            if (const auto* select =
                        SVFUtil::dyn_cast<SelectStmt>(statement))
                addAnchorVariable(variables, select->getCondition());
        }
        else if (const auto* unary =
                     SVFUtil::dyn_cast<UnaryOPStmt>(statement))
        {
            addAnchorVariable(variables, unary->getOpVar());
            addAnchorVariable(variables, unary->getRes());
        }
        else if (const auto* branch =
                     SVFUtil::dyn_cast<BranchStmt>(statement))
        {
            addAnchorVariable(variables, branch->getCondition());
            addAnchorVariable(variables, branch->getBranchInst());
        }
    }
    return variables;
}

using MemoryQuery = std::tuple<std::string, std::string, bool>;

std::map<MemoryQuery, const ObjVar*> anchorMemoryObjects(
    const SVFIR& graph, AndersenWaveDiff& pointerAnalysis,
    const ICFGNode* node)
{
    std::map<MemoryQuery, const ObjVar*> objects;
    for (const SVFStmt* statement : node->getSVFStmts())
    {
        const ValVar* pointer = nullptr;
        const char* accessKind = nullptr;
        bool pointerContent = false;
        bool comparableContent = false;
        if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement))
        {
            pointer = load->getRHSVar();
            accessKind = "load";
            pointerContent = load->getLHSVar()->isPointer();
            comparableContent =
                hasUpstreamComparableType(*load->getLHSVar());
        }
        else if (const auto* store =
                     SVFUtil::dyn_cast<StoreStmt>(statement))
        {
            pointer = store->getLHSVar();
            accessKind = "store";
            pointerContent = store->getRHSVar()->isPointer();
            comparableContent =
                hasUpstreamComparableType(*store->getRHSVar());
        }
        if (!pointer)
            continue;
        for (NodeID objectId : pointerAnalysis.getPts(pointer->getId()))
        {
            if (const auto* object =
                        SVFUtil::dyn_cast<ObjVar>(graph.getSVFVar(objectId)))
            {
                if (!pointerContent && !comparableContent)
                    continue;
                objects.emplace(MemoryQuery{std::string(accessKind) +
                                            ";pointer=" +
                                            keyPart(valueVariableKey(pointer)),
                                            memoryObjectKey(object),
                                            pointerContent},
                                object);
            }
        }
    }
    return objects;
}

void validateDynamicObjectRegistration(SVFIR& graph)
{
    SVFIRAdapter adapter(graph);
    const AD::MemoryLayout layoutSnapshot = adapter.memoryLayout();
    const ObjVar* seed = nullptr;
    for (auto iterator = graph.begin(); iterator != graph.end(); ++iterator)
    {
        if ((seed = SVFUtil::dyn_cast<ObjVar>(iterator->second)))
            break;
    }
    if (!seed)
        throw std::runtime_error("dynamic ObjVar fixture has no seed object");

    const NodeID id = graph.addDummyObjNode(seed->getType());
    const auto* object = SVFUtil::dyn_cast<ObjVar>(graph.getSVFVar(id));
    if (!object || adapter.contains(*object))
        throw std::runtime_error("dynamic ObjVar fixture was not new");

    const AD::Location location = adapter.location(*object);
    const AD::Variable content = adapter.contentVariable(*object);
    const ObjVar* reverseContent = adapter.contentObject(content);
    if (location.isNull() || !layoutSnapshot.contains(location) ||
            layoutSnapshot.contentOf(location) != content ||
            adapter.object(location).getId() != object->getId() ||
            !reverseContent || reverseContent->getId() != object->getId())
    {
        throw std::runtime_error(
            "dynamic ObjVar registration did not extend the shared memory "
            "schema");
    }
}

void validateDynamicAnalysisRegistration(
    SVFIR& graph, AbstractInterpretation& analysis)
{
    const ObjVar* seed = nullptr;
    for (auto iterator = graph.begin(); iterator != graph.end(); ++iterator)
    {
        if ((seed = SVFUtil::dyn_cast<ObjVar>(iterator->second)))
            break;
    }
    if (!seed)
        throw std::runtime_error(
            "dynamic analysis fixture has no seed object");

    const NodeID id = graph.addDummyObjNode(seed->getType());
    const auto* object = SVFUtil::dyn_cast<ObjVar>(graph.getSVFVar(id));
    if (!object || analysis.locationOf(object).isNull())
        throw std::runtime_error(
            "analysis mapped a dynamically created object to null");
}

void validateGlobalGepInitializers(
    SVFIR& graph, AbstractInterpretation& analysis)
{
    const ICFGNode* global = graph.getICFG()->getGlobalICFGNode();
    std::size_t checked = 0;
    for (const SVFStmt* statement : global->getSVFStmts())
    {
        const auto* gep = SVFUtil::dyn_cast<GepStmt>(statement);
        if (!gep || !gep->getLHSVar()->isPointer() ||
                !gep->getLHSVar()->isConstDataOrAggDataButNotNullPtr())
            continue;
        const AD::AddressSet addresses =
            analysis.getAddressSet(gep->getLHSVar(), global);
        if (addresses.isTop() || addresses.isBottom() ||
                addresses.contains(AD::Location::null()))
            throw std::runtime_error(
                "global constant GEP lost its non-null address");
        ++checked;
    }
    if (checked == 0)
        throw std::runtime_error(
            "global GEP initializer fixture contains no pointer constant GEP");
}

/// Hash representation-independent answers at stable semantic query anchors.
/// Anchors come only from the common SVFIR and Andersen points-to solution;
/// neither a Box page nor an upstream trace entry can create a record.
ResultChecksum resultChecksum(const SVFIR& graph,
                              AbstractInterpretation& analysis,
                              AndersenWaveDiff& pointerAnalysis)
{
    std::vector<std::string> records;
    std::vector<std::string> pointOrder;
    std::map<std::string, const ICFGNode*> pointOwners;
    auto numericalRecord = [&](const char* kind, const std::string& point,
                               const std::string& query,
                               const AD::Interval& value)
    {
        records.push_back(std::string(kind) + '|' + keyPart(point) + '|' +
                          keyPart(query) + "|N|" +
                          canonicalInterval(value));
    };
    auto addressRecord = [&](const char* kind, const std::string& point,
                             const std::string& query,
                             const AD::AddressSet& value)
    {
        records.push_back(std::string(kind) + '|' + keyPart(point) + '|' +
                          keyPart(query) + "|A|" +
                          canonicalAddressSet(value, analysis));
    };

    std::vector<const ICFGNode*> nodes;
    for (auto iterator = graph.getICFG()->begin();
            iterator != graph.getICFG()->end(); ++iterator)
        nodes.push_back(iterator->second);
    std::sort(nodes.begin(), nodes.end(),
              [](const ICFGNode* lhs, const ICFGNode* rhs)
    {
        return lhs->getId() < rhs->getId();
    });
    for (const ICFGNode* node : nodes)
    {
        const std::string point = programPointKey(node);
        const auto [owner, inserted] = pointOwners.emplace(point, node);
        if (!inserted && owner->second != node)
            throw std::runtime_error("non-unique semantic point key: " +
                                     point);
        pointOrder.push_back(point);
        const bool reachable = analysis.hasAbsState(node);
        records.push_back("R|" + keyPart(point) + '|' +
                          (reachable ? '1' : '0'));
        if (!reachable)
            continue;

        for (const auto& [query, value] : anchorVariables(node))
        {
            const NodeID id = value->getId();
            if (id == IRGraph::NullPtr || id == graph.getBlkPtr())
                continue;
            if (value->isPointer())
            {
                const AD::AddressSet answer = analysis.hasAbsValue(value, node)
                                              ? analysis.getAddressSet(
                                                  value, node)
                                              : AD::AddressSet::top();
                addressRecord("V", point, query, answer);
            }
            else
            {
                const AD::Interval answer = analysis.hasAbsValue(value, node)
                                            ? analysis.getInterval(value,
                                                node)
                                            : AD::Interval::top();
                numericalRecord("V", point, query, answer);
            }
        }

        for (const auto& [query, object] :
                anchorMemoryObjects(graph, pointerAnalysis, node))
        {
            const auto& [statement, objectKey, pointerContent] = query;
            if (pointerContent)
            {
                const AD::AddressSet answer = analysis.hasAbsValue(object, node)
                                              ? analysis.getAddressSet(
                                                  object, node)
                                              : AD::AddressSet::top();
                addressRecord("M", point,
                              statement + ";object=" + keyPart(objectKey),
                              answer);
            }
            else
            {
                const AD::Interval answer = analysis.hasAbsValue(object, node)
                                            ? analysis.getInterval(object,
                                                node)
                                            : AD::Interval::top();
                numericalRecord("M", point,
                                statement + ";object=" + keyPart(objectKey),
                                answer);
            }
            records.push_back(
                "F|" + keyPart(point) + '|' +
                keyPart(statement + ";object=" + keyPart(objectKey)) + '|' +
                (analysis.isFreedMemory(analysis.locationOf(object), node)
                 ? '1'
                 : '0'));
        }
    }

    std::sort(records.begin(), records.end());
    if (std::getenv("SVF_AE_RESULT_POINTS"))
    {
        for (std::size_t index = 0; index < pointOrder.size(); ++index)
            std::cout << "AE_RESULT_POINT " << index << '|'
                      << keyPart(pointOrder[index]) << '\n';
    }
    if (std::getenv("SVF_AE_RESULT_RECORDS"))
    {
        for (const std::string& record : records)
            std::cout << "AE_RESULT_RECORD " << record << '\n';
    }
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (const std::string& record : records)
    {
        for (unsigned char byte : record)
        {
            hash ^= byte;
            hash *= prime;
        }
        hash ^= 0xffU;
        hash *= prime;
    }
    return {hash, records.size()};
}

std::string stateShape(const BoxAddressDomain& state)
{
    if (state.isBottom())
        return "bottom";
    std::vector<std::string> numerical;
    for (AD::Variable variable : state.numerical().constrainedVariables())
        numerical.push_back(state.numerical().bound(variable).toString());
    std::sort(numerical.begin(), numerical.end());

    std::vector<std::string> addresses;
    for (AD::Variable variable : state.addresses().nonDefaultVariables())
    {
        const AD::AddressSet value = state.addresses().addressSet(variable);
        const bool containsNull = value.contains(AD::Location::null());
        addresses.push_back(std::to_string(value.size()) +
                            (containsNull ? "n" : "x"));
    }
    std::sort(addresses.begin(), addresses.end());

    std::string shape;
    auto append = [&](char kind, const std::vector<std::string>& values)
    {
        shape += kind;
        shape += std::to_string(values.size());
        shape += ':';
        for (const std::string& value : values)
        {
            shape += std::to_string(value.size());
            shape += '#';
            shape += value;
        }
    };
    append('N', numerical);
    append('A', addresses);
    return shape;
}

std::uint64_t semanticShapeChecksum(AbstractInterpretation& analysis)
{
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::vector<std::string> shapes;
    shapes.reserve(analysis.getAnalyzedNodes().size());
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
        shapes.push_back(
            stateShape(
                requireBoxAddressDomain(analysis.getAbstractState(node))));
    std::sort(shapes.begin(), shapes.end());
    if (const AD::AbstractDomain* scalar = analysis.getScalarAbstractState())
        shapes.push_back("scalar:" +
                         stateShape(requireBoxAddressDomain(*scalar)));

    std::uint64_t hash = offset;
    for (const std::string& shape : shapes)
    {
        for (unsigned char byte : shape)
        {
            hash ^= byte;
            hash *= prime;
        }
        hash ^= 0xffU;
        hash *= prime;
    }
    return hash;
}

void validateVariableIdLayout(const SVFIR& graph)
{
    SVFIRAdapter adapter(graph);
    std::vector<std::uint32_t> numericalScalarIds;
    std::vector<std::uint32_t> numericalContentIds;
    std::vector<std::uint32_t> pointerScalarIds;
    std::vector<std::uint32_t> pointerContentIds;
    for (auto iterator = graph.begin(); iterator != graph.end(); ++iterator)
    {
        const SVFVar* value = iterator->second;
        if (const auto* scalar = SVFUtil::dyn_cast<ValVar>(value))
        {
            if (adapter.contains(*scalar))
                (scalar->isPointer() ? pointerScalarIds : numericalScalarIds)
                .push_back(adapter.variable(*scalar).id());
        }
        else if (const auto* object = SVFUtil::dyn_cast<ObjVar>(value))
        {
            if (adapter.contains(*object))
                (object->isPointer() ? pointerContentIds : numericalContentIds)
                .push_back(adapter.contentVariable(*object).id());
        }
    }

    std::uint32_t expected = 1;
    auto requireContiguousRange = [&](const char* name, auto& ids)
    {
        std::sort(ids.begin(), ids.end());
        for (std::uint32_t id : ids)
        {
            if (id != expected++)
                throw std::runtime_error(std::string(name) +
                                         " Variable IDs are not contiguous");
        }
    };
    requireContiguousRange("numerical scalar", numericalScalarIds);
    requireContiguousRange("pointer scalar", pointerScalarIds);
    requireContiguousRange("numerical content", numericalContentIds);
    requireContiguousRange("pointer content", pointerContentIds);
}

void validateProjection(const SVFIR& graph, AbstractInterpretation& analysis)
{
    const bool loopFixture = findValue(graph, "loop_result") != nullptr;
    const SVFVar* result = findValue(graph, loopFixture ? "loop_result" : "z");
    if (!result)
        return;
    const auto* scalar = SVFUtil::dyn_cast<ValVar>(result);
    if (!scalar)
        throw std::runtime_error("Box fixture result is not an SSA value");

    const s64_t expectedLower = loopFixture ? 4 : 1;
    const s64_t expectedUpper = loopFixture ? 4 : 11;
    SVFIRAdapter adapter(graph);
    const AD::Variable variable = adapter.variable(*scalar);
    bool observed = false;
    std::string lastProjection = "<absent>";
    std::string lastStored = "<absent>";
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.hasAbsValue(scalar, node))
            continue;
        const AD::Interval projected = analysis.getInterval(scalar, node);
        const BoxAddressDomain& state = stateForValue(analysis, scalar, node);
        lastProjection = projected.toString();
        const AD::Interval stored = state.numerical().bound(variable);
        lastStored = stored.toString();
        if (hasFiniteBounds(projected, expectedLower, expectedUpper) &&
                hasFiniteBounds(stored, expectedLower, expectedUpper))
            observed = true;
    }
    if (!observed)
        throw std::runtime_error(
            "Box numerical state and AE value projection diverged: projected=" +
            lastProjection + ", stored=" + lastStored);
}

void validateSparseMemoryRefinement(const SVFIR& graph,
                                    AbstractInterpretation& analysis)
{
    const SVFVar* result = findValue(graph, "memory_result");
    if (!result)
        return;
    bool observedPositive = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        if (!analysis.hasAbsValue(result, node))
            continue;
        const AD::Interval value = analysis.getInterval(result, node);
        observedPositive |= value.lower().isFinite() &&
                            value.lower().value() == AD::Rational(1);
    }
    if (!observedPositive)
        throw std::runtime_error(
            "Box sparse memory refinement did not reach the second load");
}

void validateConservativeUnknownCasts(const SVFIR& graph,
                                      AbstractInterpretation& analysis)
{
    auto findScalar = [&](const char* name) -> const ValVar*
    {
        const SVFVar* value = findValue(graph, name);
        return value ? SVFUtil::dyn_cast<ValVar>(value) : nullptr;
    };
    const ValVar* unknownInteger = findScalar("unknown_integer");
    const ValVar* unknownPointer = findScalar("unknown_pointer");
    const ValVar* nullPointer = findScalar("null_pointer");
    if (!unknownInteger && !unknownPointer && !nullPointer)
        return;
    if (!unknownInteger || !unknownPointer || !nullPointer)
        throw std::runtime_error("unknown-cast fixture is incomplete");

    bool sawUnknownInteger = false;
    bool sawUnknownPointer = false;
    bool sawNullPointer = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        sawUnknownInteger |= analysis.getInterval(unknownInteger, node).isTop();
        sawUnknownPointer |=
            analysis.getAddressSet(unknownPointer, node).isTop();
        const AD::AddressSet nulls = analysis.getAddressSet(nullPointer, node);
        sawNullPointer |=
            nulls.isSingleton() && nulls.contains(AD::Location::null());
    }
    if (!sawUnknownInteger || !sawUnknownPointer || !sawNullPointer)
        throw std::runtime_error(
            "typed Top or inttoptr conservative fallback was not preserved");
}

void validateFloatingConstants(const SVFIR& graph,
                               AbstractInterpretation& analysis)
{
    const SVFVar* candidate = findValue(graph, "floating_result");
    const SVFVar* fractionalCandidate = findValue(graph, "fractional_result");
    const SVFVar* comparisonCandidate = findValue(graph, "is_exact");
    const SVFVar* memoryCandidate =
        findValue(graph, "floating_memory_result");
    const SVFVar* memoryComparisonCandidate =
        findValue(graph, "memory_is_exact");
    const auto* result = candidate ? SVFUtil::dyn_cast<ValVar>(candidate)
                         : nullptr;
    const auto* fractional =
        fractionalCandidate
        ? SVFUtil::dyn_cast<ValVar>(fractionalCandidate)
        : nullptr;
    const auto* comparison =
        comparisonCandidate
        ? SVFUtil::dyn_cast<ValVar>(comparisonCandidate)
        : nullptr;
    const auto* memory =
        memoryCandidate ? SVFUtil::dyn_cast<ValVar>(memoryCandidate) : nullptr;
    const auto* memoryComparison =
        memoryComparisonCandidate
        ? SVFUtil::dyn_cast<ValVar>(memoryComparisonCandidate)
        : nullptr;
    if (!result && !fractional && !comparison && !memory && !memoryComparison)
        return;
    if (!result || !fractional || !comparison || !memory || !memoryComparison)
        throw std::runtime_error("floating-constant fixture is incomplete");
    const AD::Variable fractionalVariable =
        SVFIRAdapter(graph).variable(*fractional);
    const AD::NumericType& type = fractionalVariable.type();
    if (type.kind != AD::NumericKind::IEEEFloat ||
            type.floatFormat.exponentBits != 11 ||
            type.floatFormat.significandBits != 53)
        throw std::runtime_error(
            "double ValVar was not mapped to IEEE binary64");
    bool observedIntegral = false;
    bool observedFractional = false;
    bool observedComparison = false;
    bool observedMemory = false;
    bool observedMemoryComparison = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        const AD::Interval value = analysis.getInterval(result, node);
        observedIntegral |=
            hasFiniteBounds(value, 2147483648LL, 2147483648LL);
        const AD::Interval fraction = analysis.getInterval(fractional, node);
        observedFractional |= fraction.isSingleton() &&
                              fraction.singletonValue() ==
                              AD::Rational::fromDouble(0.5);
        observedComparison |=
            hasFiniteBounds(analysis.getInterval(comparison, node), 1, 1);
        const AD::Interval memoryValue = analysis.getInterval(memory, node);
        observedMemory |= memoryValue.isSingleton() &&
                          memoryValue.singletonValue() ==
                          AD::Rational::fromDouble(0.5);
        observedMemoryComparison |= hasFiniteBounds(
                                        analysis.getInterval(memoryComparison, node), 1, 1);
    }
    if (!observedIntegral || !observedFractional || !observedComparison ||
            !observedMemory || !observedMemoryComparison)
    {
        const ICFGNode* point = comparison->getICFGNode();
        throw std::runtime_error(
            "floating constants or their exact comparison lost precision: "
            "operand-at-cmp=" +
            analysis.getInterval(result, point).toString() +
            ", result-at-cmp=" +
            analysis.getInterval(comparison, point).toString() +
            ", flow-bottom=" +
            (analysis.getAbstractState(point).isBottom() ? "true" : "false"));
    }
}

void validatePointerArgumentFlow(const SVFIR& graph,
                                 AbstractInterpretation& analysis)
{
    const SVFVar* pointerCandidate = findValue(graph, "pointer_argument");
    const SVFVar* resultCandidate =
        findValue(graph, "pointer_argument_result");
    if (!pointerCandidate && !resultCandidate)
        return;
    const auto* pointer =
        pointerCandidate ? SVFUtil::dyn_cast<ValVar>(pointerCandidate) : nullptr;
    const auto* result =
        resultCandidate ? SVFUtil::dyn_cast<ValVar>(resultCandidate) : nullptr;
    if (!pointer || !result)
        throw std::runtime_error("pointer-argument fixture is incomplete");

    bool observedPointer = false;
    bool observedResult = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        const AD::AddressSet addresses = analysis.getAddressSet(pointer, node);
        observedPointer |= addresses.isSingleton() &&
                           !addresses.contains(AD::Location::null());
        observedResult |=
            hasFiniteBounds(analysis.getInterval(result, node), 11, 11);
    }
    if (!observedPointer || !observedResult)
        throw std::runtime_error(
            "direct-call pointer argument lost its singleton object or "
            "memory value");
}

void validateMultiCallerPointerFlow(const SVFIR& graph,
                                    AbstractInterpretation& analysis)
{
    const SVFVar* pointerCandidate =
        findValue(graph, "multi_pointer_argument");
    const SVFVar* resultCandidate =
        findValue(graph, "multi_pointer_argument_result");
    if (!pointerCandidate && !resultCandidate)
        return;
    const auto* pointer =
        pointerCandidate ? SVFUtil::dyn_cast<ValVar>(pointerCandidate) : nullptr;
    const auto* result =
        resultCandidate ? SVFUtil::dyn_cast<ValVar>(resultCandidate) : nullptr;
    if (!pointer || !result)
        throw std::runtime_error("multi-caller pointer fixture is incomplete");

    bool observedJoinedPointer = false;
    bool observedJoinedResult = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        const AD::AddressSet addresses = analysis.getAddressSet(pointer, node);
        observedJoinedPointer |= !addresses.isTop() && addresses.size() == 2;
        observedJoinedResult |=
            hasFiniteBounds(analysis.getInterval(result, node), 7, 11);
    }
    if (!observedJoinedPointer || !observedJoinedResult)
        throw std::runtime_error(
            "multi-caller pointer argument did not join both call sites");
}

void validateUnknownCallerPointerFlow(const SVFIR& graph,
                                      AbstractInterpretation& analysis)
{
    const SVFVar* pointerCandidate =
        findValue(graph, "unknown_pointer_argument");
    const SVFVar* resultCandidate =
        findValue(graph, "unknown_pointer_argument_result");
    if (!pointerCandidate && !resultCandidate)
        return;
    const auto* pointer =
        pointerCandidate ? SVFUtil::dyn_cast<ValVar>(pointerCandidate) : nullptr;
    const auto* result =
        resultCandidate ? SVFUtil::dyn_cast<ValVar>(resultCandidate) : nullptr;
    if (!pointer || !result)
        throw std::runtime_error("unknown-caller pointer fixture is incomplete");

    bool observedUnknownPointer = false;
    bool observedUnknownResult = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        observedUnknownPointer |=
            analysis.getAddressSet(pointer, node).isTop();
        observedUnknownResult |= analysis.getInterval(result, node).isTop();
    }
    if (!observedUnknownPointer || !observedUnknownResult)
        throw std::runtime_error(
            "known and unknown callers did not conservatively join to Top");
}

void validatePointerOrderingFlow(const SVFIR& graph,
                                 AbstractInterpretation& analysis)
{
    const SVFVar* differentCandidate = findValue(graph, "known_different");
    const SVFVar* sameEqualityCandidate = findValue(graph, "known_same");
    const SVFVar* branchCandidate =
        findValue(graph, "pointer_branch_result");
    const SVFVar* overlapCandidate = findValue(graph, "overlapping_equality");
    const SVFVar* nullCandidate = findValue(graph, "null_equality");
    const SVFVar* unknownCandidate = findValue(graph, "opaque_equality");
    const SVFVar* orderingCandidate =
        findValue(graph, "pointer_ordering_result");
    const SVFVar* sameCandidate =
        findValue(graph, "same_pointer_ordering_result");
    const SVFVar* zextCandidate = findValue(graph, "result");
    if (!differentCandidate && !orderingCandidate && !sameCandidate)
        return;
    const auto* different =
        differentCandidate ? SVFUtil::dyn_cast<ValVar>(differentCandidate)
        : nullptr;
    const auto* sameEquality =
        sameEqualityCandidate
        ? SVFUtil::dyn_cast<ValVar>(sameEqualityCandidate)
        : nullptr;
    const auto* branch =
        branchCandidate ? SVFUtil::dyn_cast<ValVar>(branchCandidate) : nullptr;
    const auto* overlap =
        overlapCandidate ? SVFUtil::dyn_cast<ValVar>(overlapCandidate) : nullptr;
    const auto* nullEquality =
        nullCandidate ? SVFUtil::dyn_cast<ValVar>(nullCandidate) : nullptr;
    const auto* unknown =
        unknownCandidate ? SVFUtil::dyn_cast<ValVar>(unknownCandidate) : nullptr;
    const auto* ordering = orderingCandidate
                           ? SVFUtil::dyn_cast<ValVar>(orderingCandidate)
                           : nullptr;
    const auto* same =
        sameCandidate ? SVFUtil::dyn_cast<ValVar>(sameCandidate) : nullptr;
    const auto* zext =
        zextCandidate ? SVFUtil::dyn_cast<ValVar>(zextCandidate) : nullptr;
    if (!different || !sameEquality || !branch || !overlap ||
            !nullEquality || !unknown || !ordering || !same || !zext)
        throw std::runtime_error("pointer-comparison fixture is incomplete");

    bool observedKnownDifferent = false;
    bool observedKnownSame = false;
    bool observedPrunedBranches = false;
    bool observedOverlappingTargets = false;
    bool observedNullEquality = false;
    bool observedUnknownTarget = false;
    bool observedUnknownOrdering = false;
    bool observedKnownSameOrdering = false;
    bool observedZExtRange = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
    {
        observedKnownDifferent |=
            hasFiniteBounds(analysis.getInterval(different, node), 1, 1);
        observedKnownSame |=
            hasFiniteBounds(analysis.getInterval(sameEquality, node), 1, 1);
        observedPrunedBranches |=
            hasFiniteBounds(analysis.getInterval(branch, node), 7, 7);
        observedOverlappingTargets |=
            hasFiniteBounds(analysis.getInterval(overlap, node), 0, 1);
        observedNullEquality |=
            hasFiniteBounds(analysis.getInterval(nullEquality, node), 1, 1);
        observedUnknownTarget |=
            hasFiniteBounds(analysis.getInterval(unknown, node), 0, 1);
        observedUnknownOrdering |=
            hasFiniteBounds(analysis.getInterval(ordering, node), 0, 1);
        observedKnownSameOrdering |=
            hasFiniteBounds(analysis.getInterval(same, node), 1, 1);
        observedZExtRange |=
            hasFiniteBounds(analysis.getInterval(zext, node), 0, 1);
    }
    std::vector<std::string> missing;
    auto requireObserved = [&](bool observed, const char* property)
    {
        if (!observed)
            missing.emplace_back(property);
    };
    requireObserved(observedKnownDifferent, "known-different");
    requireObserved(observedKnownSame, "known-same");
    requireObserved(observedPrunedBranches, "branch-feasibility");
    requireObserved(observedOverlappingTargets, "overlapping-targets");
    requireObserved(observedNullEquality, "null-equality");
    requireObserved(observedUnknownTarget, "unknown-target");
    requireObserved(observedUnknownOrdering, "distinct-ordering");
    requireObserved(observedKnownSameOrdering, "same-ordering");
    requireObserved(observedZExtRange, "ordering-zext");
    if (!missing.empty())
    {
        std::ostringstream message;
        message << "pointer comparison validation missed";
        for (const std::string& property : missing)
            message << ' ' << property;
        throw std::runtime_error(message.str());
    }
}

void validateNegativeGepFlow(const SVFIR& graph,
                             AbstractInterpretation& analysis)
{
    const SVFVar* candidate = findValue(graph, "negative_gep_result");
    if (!candidate)
        return;
    const auto* result = SVFUtil::dyn_cast<ValVar>(candidate);
    if (!result)
        throw std::runtime_error("negative-GEP fixture is incomplete");

    bool observedExpectedValue = false;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
        observedExpectedValue |=
            hasFiniteBounds(analysis.getInterval(result, node), 30, 30);
    if (!observedExpectedValue)
        throw std::runtime_error(
            "signed negative GEP offset was clamped or mis-normalized");
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const std::vector<std::string> modules = OptionBase::parseOptions(
                argc, argv, "Box AE integration test", "[options] <input-bitcode>");
        LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
        SVFIRBuilder builder;
        SVFIR* graph = builder.build();
        if (std::getenv("SVF_AE_VALIDATE_DYNAMIC_ADAPTER"))
            validateDynamicObjectRegistration(*graph);
        AndersenWaveDiff* ander =
            AndersenWaveDiff::createAndersenWaveDiff(graph);
        builder.updateCallGraph(ander->getCallGraph());
        AbstractInterpretation& analysis =
            AbstractInterpretation::getAEInstance();
        analysis.runOnModule();
        if (std::getenv("SVF_AE_VALIDATE_DYNAMIC_ADAPTER"))
            validateDynamicAnalysisRegistration(*graph, analysis);
        if (std::getenv("SVF_AE_VALIDATE_GLOBAL_GEP_INITIALIZERS"))
            validateGlobalGepInitializers(*graph, analysis);
        validateAuthoritativeStorage(analysis);
        if (std::getenv("SVF_AE_VALIDATE_VARIABLE_ID_LAYOUT"))
            validateVariableIdLayout(*graph);
        validateProjection(*graph, analysis);
        validateSparseMemoryRefinement(*graph, analysis);
        validateConservativeUnknownCasts(*graph, analysis);
        validateFloatingConstants(*graph, analysis);
        validatePointerArgumentFlow(*graph, analysis);
        validateMultiCallerPointerFlow(*graph, analysis);
        validateUnknownCallerPointerFlow(*graph, analysis);
        validatePointerOrderingFlow(*graph, analysis);
        validateNegativeGepFlow(*graph, analysis);

        std::cout << "AE_GENERIC_OBSERVATION analyzed_nodes="
                  << analysis.getAnalyzedNodes().size() << '\n';
        if (std::getenv("SVF_AE_SEMANTIC_CHECKSUM"))
        {
            const ResultChecksum result =
                resultChecksum(*graph, analysis, *ander);
            std::cout << "AE_RESULT_HASH fnv1a64=" << std::hex << std::setw(16)
                      << std::setfill('0') << result.value << std::dec
                      << " records=" << result.records
                      << " contract=svf-query-projection-v7\n";
            std::cout << "AE_SEMANTIC_CHECKSUM fnv1a64=" << std::hex
                      << std::setw(16) << std::setfill('0')
                      << semanticChecksum(analysis) << std::dec << '\n';
            std::cout << "AE_SEMANTIC_SHAPE_CHECKSUM fnv1a64=" << std::hex
                      << std::setw(16) << std::setfill('0')
                      << semanticShapeChecksum(analysis) << std::dec << '\n';
        }
        if (std::getenv("SVF_AE_STORAGE_OBSERVATION"))
        {
            const StorageObservations storage = observeStorage(analysis);
            const VariablePopulation population =
                observeVariablePopulation(*graph);
            std::cout
                    << "AE_STORAGE_OBSERVATION flow_states=" << storage.flow.states
                    << " flow_numerical_facts=" << storage.flow.numericalFacts
                    << " flow_numerical_pages=" << storage.flow.numericalPages
                    << " flow_address_facts=" << storage.flow.addressFacts
                    << " flow_address_pages8=" << storage.flow.addressPages8
                    << " flow_address_pages16=" << storage.flow.addressPages16
                    << " flow_address_pages32=" << storage.flow.addressPages32
                    << " flow_address_states_above16="
                    << storage.flow.addressStatesAbove16
                    << " flow_address_facts_above16="
                    << storage.flow.addressFactsAbove16
                    << " flow_address_pages16_above16="
                    << storage.flow.addressPages16Above16
                    << " flow_address_p50=" << storage.flow.percentile(0.50)
                    << " flow_address_p95=" << storage.flow.percentile(0.95)
                    << " flow_address_p99=" << storage.flow.percentile(0.99)
                    << " flow_address_max=" << storage.flow.percentile(1.0)
                    << " flow_finite_pointees=" << storage.flow.finitePointees
                    << " flow_set_size_p50=" << storage.flow.setSizePercentile(0.50)
                    << " flow_set_size_p95=" << storage.flow.setSizePercentile(0.95)
                    << " flow_set_size_p99=" << storage.flow.setSizePercentile(0.99)
                    << " flow_largest_address_set="
                    << storage.flow.largestAddressSet
                    << " scalar_numerical_facts=" << storage.scalar.numericalFacts
                    << " scalar_address_facts=" << storage.scalar.addressFacts
                    << " scalar_address_pages8=" << storage.scalar.addressPages8
                    << " scalar_address_pages16=" << storage.scalar.addressPages16
                    << " scalar_address_pages32=" << storage.scalar.addressPages32
                    << " scalar_address_states_above16="
                    << storage.scalar.addressStatesAbove16
                    << " scalar_address_facts_above16="
                    << storage.scalar.addressFactsAbove16
                    << " scalar_address_pages16_above16="
                    << storage.scalar.addressPages16Above16
                    << " scalar_finite_pointees=" << storage.scalar.finitePointees
                    << " scalar_set_size_p50="
                    << storage.scalar.setSizePercentile(0.50)
                    << " scalar_set_size_p95="
                    << storage.scalar.setSizePercentile(0.95)
                    << " scalar_set_size_p99="
                    << storage.scalar.setSizePercentile(0.99)
                    << " scalar_largest_address_set="
                    << storage.scalar.largestAddressSet
                    << " pointer_scalar_variables=" << population.pointerScalars
                    << " pointer_content_variables=" << population.pointerContents
                    << " finite_pointees="
                    << storage.flow.finitePointees + storage.scalar.finitePointees
                    << " largest_address_set="
                    << std::max(storage.flow.largestAddressSet,
                                storage.scalar.largestAddressSet)
                    << '\n';
        }
        std::cout << "Box AE integration test: PASS\n";
        AndersenWaveDiff::releaseAndersenWaveDiff();
        LLVMModuleSet::releaseLLVMModuleSet();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Box AE integration test: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
