//===- BoxAEIntegrationTest.cpp -- Box-backed AE integration test -------===//

#include "AE/Core/BoxProgramState.h"
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
#include <stdexcept>
#include <string>
#include <vector>

using namespace SVF;

namespace
{
namespace AD = SVF::AbstractDomain;
using BoxProgramState = AD::BoxProgramState;

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

const BoxProgramState& requireBoxState(const AD::AbstractDomain& state)
{
    if (!state.isDomain<BoxProgramState>())
        throw std::runtime_error("AE property is not Box-backed");
    return static_cast<const BoxProgramState&>(state);
}

const BoxProgramState& stateForValue(AbstractInterpretation& analysis,
                                     const ValVar* value, const ICFGNode* node)
{
    if (const AD::AbstractDomain* scalar = analysis.getScalarAbstractState())
        return requireBoxState(*scalar);
    return requireBoxState(analysis.getAbstractState(node));
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
        throw std::runtime_error("Box AE analyzed no ICFG nodes");
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
        requireBoxState(analysis.getAbstractState(node));
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

    void observe(const BoxProgramState& state)
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
            requireBoxState(analysis.getAbstractState(node)));
    if (const AD::AbstractDomain* scalar = analysis.getScalarAbstractState())
        observations.scalar.observe(requireBoxState(*scalar));
    return observations;
}

std::uint64_t semanticChecksum(AbstractInterpretation& analysis)
{
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    auto consume = [&](const std::string& value) {
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
              [](const ICFGNode* lhs, const ICFGNode* rhs) {
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

std::string canonicalAddressSet(const AD::AddressSet& addresses,
                                const SVFIRAdapter& adapter)
{
    if (addresses.isTop())
        return "top";
    if (addresses.isBottom())
        return "bottom";
    std::vector<NodeID> objectIds;
    objectIds.reserve(addresses.size());
    for (AD::Location location : addresses.locations())
    {
        objectIds.push_back(
            location.isNull() ? 0U : adapter.object(location).getId());
    }
    std::sort(objectIds.begin(), objectIds.end());
    std::string result = "{";
    for (NodeID objectId : objectIds)
    {
        if (result.size() != 1)
            result += ',';
        result += std::to_string(objectId);
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

void addAnchorVariable(std::map<NodeID, const ValVar*>& variables,
                       const SVFVar* value)
{
    if (const auto* scalar = SVFUtil::dyn_cast<ValVar>(value))
        variables.emplace(scalar->getId(), scalar);
}

std::map<NodeID, const ValVar*> anchorVariables(const ICFGNode* node)
{
    std::map<NodeID, const ValVar*> variables;
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

std::map<NodeID, const ObjVar*> anchorMemoryObjects(
    const SVFIR& graph, AndersenWaveDiff& pointerAnalysis,
    const ICFGNode* node)
{
    std::map<NodeID, const ObjVar*> objects;
    for (const SVFStmt* statement : node->getSVFStmts())
    {
        const ValVar* pointer = nullptr;
        if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement))
            pointer = load->getRHSVar();
        else if (const auto* store =
                     SVFUtil::dyn_cast<StoreStmt>(statement))
            pointer = store->getLHSVar();
        if (!pointer)
            continue;
        for (NodeID objectId : pointerAnalysis.getPts(pointer->getId()))
        {
            if (const auto* object =
                    SVFUtil::dyn_cast<ObjVar>(graph.getSVFVar(objectId)))
                objects.emplace(objectId, object);
        }
    }
    return objects;
}

/// Hash representation-independent answers at stable semantic query anchors.
/// Anchors come only from the common SVFIR and Andersen points-to solution;
/// neither a Box page nor an upstream trace entry can create a record.
ResultChecksum resultChecksum(const SVFIR& graph,
                              AbstractInterpretation& analysis,
                              AndersenWaveDiff& pointerAnalysis)
{
    SVFIRAdapter adapter(graph);
    std::vector<std::string> records;
    auto numericalRecord = [&](const char* kind, NodeID point, NodeID id,
                               const AD::Interval& value) {
        records.push_back(std::string(kind) + '|' + std::to_string(point) +
                          '|' + std::to_string(id) + "|N|" +
                          canonicalInterval(value));
    };
    auto addressRecord = [&](const char* kind, NodeID point, NodeID id,
                             const AD::AddressSet& value) {
        records.push_back(std::string(kind) + '|' + std::to_string(point) +
                          '|' + std::to_string(id) + "|A|" +
                          canonicalAddressSet(value, adapter));
    };

    std::vector<const ICFGNode*> nodes;
    for (auto iterator = graph.getICFG()->begin();
         iterator != graph.getICFG()->end(); ++iterator)
        nodes.push_back(iterator->second);
    std::sort(nodes.begin(), nodes.end(),
              [](const ICFGNode* lhs, const ICFGNode* rhs) {
                  return lhs->getId() < rhs->getId();
              });
    for (const ICFGNode* node : nodes)
    {
        const NodeID point = node->getId();
        const bool reachable = analysis.hasAbsState(node);
        records.push_back("R|" + std::to_string(point) + '|' +
                          (reachable ? '1' : '0'));
        if (!reachable)
            continue;

        for (const auto& [id, value] : anchorVariables(node))
        {
            if (id == IRGraph::NullPtr || id == graph.getBlkPtr())
                continue;
            if (value->isPointer())
            {
                const AD::AddressSet answer = analysis.hasAbsValue(value, node)
                                                  ? analysis.getAddressSet(
                                                        value, node)
                                                  : AD::AddressSet::top();
                addressRecord("V", point, id, answer);
            }
            else
            {
                const AD::Interval answer = analysis.hasAbsValue(value, node)
                                                ? analysis.getInterval(value,
                                                                       node)
                                                : AD::Interval::top();
                numericalRecord("V", point, id, answer);
            }
        }

        for (const auto& [id, object] :
             anchorMemoryObjects(graph, pointerAnalysis, node))
        {
            if (object->isPointer())
            {
                const AD::AddressSet answer = analysis.hasAbsValue(object, node)
                                                  ? analysis.getAddressSet(
                                                        object, node)
                                                  : AD::AddressSet::top();
                addressRecord("M", point, id, answer);
            }
            else
            {
                const AD::Interval answer = analysis.hasAbsValue(object, node)
                                                ? analysis.getInterval(object,
                                                                       node)
                                                : AD::Interval::top();
                numericalRecord("M", point, id, answer);
            }
            records.push_back(
                "F|" + std::to_string(point) + '|' + std::to_string(id) +
                '|' +
                (analysis.isFreedMemory(analysis.locationOf(object), node)
                     ? '1'
                     : '0'));
        }
    }

    std::sort(records.begin(), records.end());
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

std::string stateShape(const BoxProgramState& state)
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
    auto append = [&](char kind, const std::vector<std::string>& values) {
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
            stateShape(requireBoxState(analysis.getAbstractState(node))));
    std::sort(shapes.begin(), shapes.end());
    if (const AD::AbstractDomain* scalar = analysis.getScalarAbstractState())
        shapes.push_back("scalar:" + stateShape(requireBoxState(*scalar)));

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
    auto requireContiguousRange = [&](const char* name, auto& ids) {
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
        const BoxProgramState& state = stateForValue(analysis, scalar, node);
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
    auto findScalar = [&](const char* name) -> const ValVar* {
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
        AndersenWaveDiff* ander =
            AndersenWaveDiff::createAndersenWaveDiff(graph);
        builder.updateCallGraph(ander->getCallGraph());

        AbstractInterpretation& analysis =
            AbstractInterpretation::getAEInstance();
        analysis.runOnModule();
        validateAuthoritativeStorage(analysis);
        if (std::getenv("SVF_AE_VALIDATE_VARIABLE_ID_LAYOUT"))
            validateVariableIdLayout(*graph);
        validateProjection(*graph, analysis);
        validateSparseMemoryRefinement(*graph, analysis);
        validateConservativeUnknownCasts(*graph, analysis);

        std::cout << "AE_GENERIC_OBSERVATION analyzed_nodes="
                  << analysis.getAnalyzedNodes().size() << '\n';
        if (std::getenv("SVF_AE_SEMANTIC_CHECKSUM"))
        {
            const ResultChecksum result =
                resultChecksum(*graph, analysis, *ander);
            std::cout << "AE_RESULT_HASH fnv1a64=" << std::hex << std::setw(16)
                      << std::setfill('0') << result.value << std::dec
                      << " records=" << result.records
                      << " contract=svf-query-projection-v3\n";
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
