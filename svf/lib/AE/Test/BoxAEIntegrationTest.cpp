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
#include <iostream>
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
    std::size_t finitePointees = 0;
    std::size_t largestAddressSet = 0;

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
        for (AD::Variable variable : pointers)
        {
            const AD::AddressSet addresses =
                state.addresses().addressSet(variable);
            if (addresses.isTop())
                throw std::runtime_error(
                    "Address non-default support contains Top");
            finitePointees += addresses.size();
            largestAddressSet = std::max(largestAddressSet, addresses.size());
        }
    }
};

StorageObservation observeStorage(AbstractInterpretation& analysis)
{
    StorageObservation observation;
    for (const ICFGNode* node : analysis.getAnalyzedNodes())
        observation.observe(requireBoxState(analysis.getAbstractState(node)));
    if (const AD::AbstractDomain* scalar = analysis.getScalarAbstractState())
        observation.observe(requireBoxState(*scalar));
    return observation;
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
        if (std::getenv("SVF_AE_STORAGE_OBSERVATION"))
        {
            const StorageObservation storage = observeStorage(analysis);
            std::cout << "AE_STORAGE_OBSERVATION states=" << storage.states
                      << " numerical_facts=" << storage.numericalFacts
                      << " numerical_pages=" << storage.numericalPages
                      << " address_facts=" << storage.addressFacts
                      << " finite_pointees=" << storage.finitePointees
                      << " largest_address_set=" << storage.largestAddressSet
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
