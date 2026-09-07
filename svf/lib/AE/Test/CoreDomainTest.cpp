//===- CoreDomainTest.cpp -- Abstract, Box, and Address domain tests ----===//

#include "AE/Core/AddressDomain.h"
#include "AE/Core/BoxProgramState.h"
#include "AE/Core/NumericalDomain.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace SVF::AbstractDomain;

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Action>
void requireThrows(Action&& action, const std::string& message)
{
    try
    {
        action();
    }
    catch (const std::exception&)
    {
        return;
    }
    throw std::runtime_error(message);
}

LinearConstraint atLeast(Variable variable, const Rational& value)
{
    return greaterEqual(LinearExpression(variable), LinearExpression(value));
}

LinearConstraint atMost(Variable variable, const Rational& value)
{
    return lessEqual(LinearExpression(variable), LinearExpression(value));
}

bool hasBounds(const Interval& interval, const Rational& lower,
               const Rational& upper)
{
    return interval.lower().isFinite() && interval.upper().isFinite() &&
           interval.lower().value() == lower &&
           interval.upper().value() == upper;
}

Interval integerInterval(std::int64_t value)
{
    return Interval::singleton(Rational(value));
}

void testScalarTransferOperations()
{
    require(Rational::fromDouble(0.0) == Rational(0) &&
                Rational::fromDouble(0.5) == Rational(Integer(1), Integer(2)),
            "native floating-to-rational conversion was not exact");
    const Interval two = integerInterval(2);
    const Interval four = integerInterval(4);
    require(add(two, four) == integerInterval(6) &&
                subtract(four, two) == two &&
                multiply(two, four) == integerInterval(8) &&
                divide(four, two) == two &&
                remainder(integerInterval(5), two) == integerInterval(1),
            "native scalar arithmetic produced an incorrect singleton");

    require(bitwiseAnd(integerInterval(6), integerInterval(3)) ==
                    integerInterval(2) &&
                bitwiseOr(integerInterval(4), integerInterval(1)) ==
                    integerInterval(5) &&
                bitwiseXor(integerInterval(7), integerInterval(3)) ==
                    integerInterval(4) &&
                shiftLeft(two, integerInterval(3)) == integerInterval(16) &&
                shiftRight(integerInterval(15), two) == integerInterval(3),
            "native bitwise or shift transfer produced an incorrect result");
    require(
        hasBounds(shiftLeft(Interval::closed(Rational(1), Rational(3)),
                            Interval::closed(Rational(1), Rational(2))),
                  Rational(2), Rational(12)) &&
            hasBounds(shiftRight(Interval::closed(Rational(-17), Rational(15)),
                                 Interval::closed(Rational(1), Rational(2))),
                      Rational(-9), Rational(7)) &&
            hasBounds(bitwiseOr(Interval::closed(Rational(1), Rational(3)),
                                Interval::closed(Rational(4), Rational(4))),
                      Rational(0), Rational(7)),
        "native range bitwise or shift transfer was not sound");

    const Interval low = Interval::closed(Rational(0), Rational(3));
    const Interval high = Interval::closed(Rational(5), Rational(8));
    require(equalTo(two, two) == integerInterval(1) &&
                notEqualTo(low, high) == integerInterval(1) &&
                lessThan(low, high) == integerInterval(1) &&
                greaterEqual(high, low) == integerInterval(1) &&
                equalTo(low, Interval::closed(Rational(2), Rational(6))) ==
                    Interval::closed(Rational(0), Rational(1)),
            "native comparison transfer lost definite or unknown outcomes");

    const Interval closedZeroOne = Interval::closed(Rational(0), Rational(1));
    const Interval openZeroOne(Bound::finite(Rational(0), true),
                               Bound::finite(Rational(1)));
    Interval joined = openZeroOne;
    joined.joinWith(closedZeroOne);
    Interval met = closedZeroOne;
    met.meetWith(openZeroOne);
    require(openZeroOne.isSubsetOf(closedZeroOne) && joined == closedZeroOne &&
                met == openZeroOne,
            "native interval lattice mishandled a strict lower bound");
    require(lessThan(Interval(Bound::finite(Rational(0)),
                              Bound::finite(Rational(1), true)),
                     Interval::closed(Rational(1), Rational(2))) ==
                integerInterval(1),
            "native interval comparison ignored a strict endpoint");
}

void testLatticeAndTransferSurface()
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3, NumericType::real());

    BoxDomain state = BoxDomain::top();
    require(state.kind() == DomainKind::Box,
            "Box property reported the wrong DomainKind");
    const std::unique_ptr<AbstractDomain> cloned = state.clone();
    require(cloned->isDomain<BoxDomain>() && cloned->kind() == DomainKind::Box,
            "AbstractDomain clone lost the concrete Box property type");
    state.assume(atLeast(x, Rational(0)));
    state.assume(atMost(x, Rational(10)));
    state.assume(
        greaterThan(LinearExpression(z), LinearExpression(Rational("1/2"))));
    state.assign(y, LinearExpression(x) + LinearExpression(Rational(2)));
    require(hasBounds(state.bound(x), Rational(0), Rational(10)) &&
                hasBounds(state.bound(y), Rational(2), Rational(12)),
            "Box assumptions and affine assignment lost interval bounds");
    require(hasBounds(state.bound(LinearExpression(x) + LinearExpression(y)),
                      Rational(2), Rational(22)),
            "Box expression bounds did not use all terms");
    require(state.bound(z).lower().isStrict() &&
                state.bound(z).lower().value() == Rational("1/2"),
            "Box applied integer tightening to a typed real variable");

    BoxDomain simultaneous = state;
    simultaneous.assignParallel(
        {{x, LinearExpression(y)}, {y, LinearExpression(x)}});
    require(hasBounds(simultaneous.bound(x), Rational(2), Rational(12)) &&
                hasBounds(simultaneous.bound(y), Rational(0), Rational(10)),
            "Box parallel assignment was not simultaneous");

    BoxDomain post = BoxDomain::top();
    post.assume(atLeast(y, Rational(5)));
    post.assume(atMost(y, Rational(7)));
    post.substitute(y, LinearExpression(x) + LinearExpression(Rational(1)));
    require(hasBounds(post.bound(x), Rational(4), Rational(6)),
            "Box backward substitution computed the wrong preimage");

    BoxDomain alternative = BoxDomain::top();
    alternative.assume(atLeast(x, Rational(5)));
    alternative.assume(atMost(x, Rational(20)));
    const BoxDomain joined = state.join(alternative);
    const BoxDomain met = state.meet(alternative);
    require(hasBounds(joined.bound(x), Rational(0), Rational(20)) &&
                hasBounds(met.bound(x), Rational(5), Rational(10)),
            "Box join/meet did not compute interval hull/intersection");
    require(state.isSubsetOf(joined) == CheckResult::True &&
                met.isSubsetOf(state) == CheckResult::True,
            "Box lattice ordering disagrees with join/meet");

    const BoxDomain widened = state.widen(alternative);
    require(widened.bound(x).upper().isPlusInfinity(),
            "Box widening did not extrapolate an unstable upper bound");
    require(widened.narrow(alternative).bound(x).upper().value() ==
                Rational(20),
            "Box narrowing did not recover the finite successor bound");

    BoxDomain contradiction = BoxDomain::top();
    contradiction.assume(atLeast(x, Rational(2)));
    contradiction.assume(atMost(x, Rational(1)));
    require(contradiction.isBottom(),
            "Box failed to detect contradictory bounds");
}

void testStableVocabularyExpandFoldAndTrees()
{
    const Variable x(1);
    const Variable y(2);
    const Variable copy(4097);
    BoxDomain state = BoxDomain::top();
    require(state.bound(copy).isTop(),
            "an unmaterialized stable variable was not Top");
    state.assume(atLeast(x, Rational(1)));
    state.assume(atMost(x, Rational(3)));
    state.expand(x, {copy});
    require(hasBounds(state.bound(copy), Rational(1), Rational(3)),
            "Box expand did not duplicate the source interval");
    state.assume(atLeast(copy, Rational(2)));
    state.fold(x, {copy});
    require(state.bound(copy).isTop() &&
                hasBounds(state.bound(x), Rational(1), Rational(3)),
            "Box fold did not merge and forget the folded variable");

    BoxDomain unknown = BoxDomain::top();
    require(state.join(unknown).isTop() &&
                state.meet(unknown).isEquivalentTo(state) == CheckResult::True,
            "Box did not interpret a missing stable variable as Top");

    const Variable realX(x.id(), NumericType::real());
    requireThrows([&] { (void)state.bound(realX); },
                  "Box accepted two numeric types for one stable variable ID");
    requireThrows([&] { state.forget(realX); },
                  "Box forgot a slot through a mismatched typed variable");

    TreeExpression xTree = TreeExpression::variable(x, NumericType::integer());
    TreeExpression two =
        TreeExpression::constant(Rational(2), NumericType::integer());
    state.assign(y, TreeExpression::binary(BinaryOperator::Multiply, xTree, two,
                                           NumericType::integer()));
    require(hasBounds(state.bound(y), Rational(2), Rational(6)),
            "Box nonlinear tree interval evaluation lost finite bounds");
}

void testPagedCopyOnWriteAndSerialization()
{
    std::vector<Variable> variables;
    for (std::uint32_t id = 0; id < 256; ++id)
        variables.emplace_back(id * 17 + 1);
    const Variable first = variables.front();
    const Variable distant(variables[200].id(), NumericType::real());

    BoxDomain original = BoxDomain::top();
    original.assume(atLeast(first, Rational(1)));
    original.assume(atMost(first, Rational(3)));
    original.assume(atLeast(distant, Rational(9)));
    original.assume(atMost(distant, Rational(11)));
    BoxDomain copy = original;
    copy.assign(first, LinearExpression(Rational(7)));
    require(hasBounds(original.bound(first), Rational(1), Rational(3)) &&
                hasBounds(copy.bound(first), Rational(7), Rational(7)) &&
                hasBounds(copy.bound(distant), Rational(9), Rational(11)),
            "paged Box COW mutated a source or detached unrelated data");

    BoxDomain sharedPages = BoxDomain::top();
    for (const Variable variable : variables)
        sharedPages.assign(variable, LinearExpression(Rational(5)));
    require(sharedPages.constrainedVariablesBefore(variables[10]).size() == 10,
            "bounded Box observation crossed its stable-ID partition");
    BoxDomain identicalJoin = sharedPages;
    identicalJoin.joinWith(sharedPages);
    require(identicalJoin.isEquivalentTo(sharedPages) == CheckResult::True,
            "page-identity Box join changed a shared state");

    BoxDomain oneChanged = sharedPages;
    oneChanged.assign(first, LinearExpression(Rational(7)));
    BoxDomain partialJoin = sharedPages;
    partialJoin.joinWith(oneChanged);
    require(hasBounds(partialJoin.bound(first), Rational(5), Rational(7)) &&
                hasBounds(partialJoin.bound(variables[200]), Rational(5),
                          Rational(5)),
            "page-wise Box join lost a changed or shared page");

    const Variable samePage(first.id() + 1);
    BoxDomain disjointSlot = BoxDomain::top();
    disjointSlot.assign(samePage, LinearExpression(Rational(5)));
    BoxDomain missingSlotJoin = original;
    missingSlotJoin.joinWith(disjointSlot);
    require(missingSlotJoin.bound(first).isTop() &&
                missingSlotJoin.bound(samePage).isTop() &&
                missingSlotJoin.bound(distant).isTop(),
            "page-wise Box join did not treat missing slots as Top");

    const NumericalDomain::RawBuffer raw = original.serializeRaw();
    std::unique_ptr<NumericalDomain> restored =
        NumericalDomain::deserializeRaw(raw);
    require(restored->isDomain<BoxDomain>() &&
                restored->isEquivalentTo(original) == CheckResult::True &&
                restored->hash() == original.hash(),
            "Box raw round-trip changed semantic state or hash");
    NumericalDomain::RawBuffer corrupt = raw;
    corrupt[corrupt.size() / 2] ^= 1U;
    requireThrows([&] { (void)NumericalDomain::deserializeRaw(corrupt); },
                  "Box raw deserialization accepted corrupt data");
}

void testProgramStateMemoryFacet()
{
    const Variable pointer(1);
    const Variable source(2);
    const Variable target(3);
    const Variable cell(4);
    const Variable lateCell(5);
    const Location object(10);
    const Location lateObject(20);
    MemoryLayout layout({{object, cell}});
    BoxProgramState state(BoxDomain::top(), layout);
    require(state.isTop(),
            "empty typed Box program state was not unconstrained");
    BoxProgramState unreachable(BoxDomain::bottom(),
                                MemoryLayout({{object, cell}}));
    BoxProgramState firstMerge = unreachable;
    firstMerge.joinWith(state);
    require(firstMerge.isEquivalentTo(state) == CheckResult::True &&
                unreachable.isSubsetOf(state) == CheckResult::True,
            "Box program-state Bottom did not act as the join identity");
    state.allocate(object);
    state.assignPointer(pointer, AddressSet::singleton(object));
    state.assignNumeric(source, LinearExpression(Rational(7)));
    state.store(pointer, source);
    state.load(target, pointer);
    require(
        hasBounds(state.numerical().bound(target), Rational(7), Rational(7)),
        "Box program state did not preserve a strong store/load");

    layout.extend(lateObject, lateCell);
    require(state.memoryLayout().contains(lateObject) &&
                state.memoryLayout().contentOf(lateObject) == lateCell,
            "an existing state did not observe a monotone layout extension");
    state.allocate(lateObject);
    state.assignPointer(pointer, AddressSet::singleton(lateObject));
    state.assignNumeric(source, LinearExpression(Rational(11)));
    state.store(pointer, source);
    state.load(target, pointer);
    require(hasBounds(state.numerical().bound(target), Rational(11),
                      Rational(11)),
            "a dynamically registered memory cell was not usable");

    state.release(pointer);
    require(state.lifetimes().mustBeFreed(lateObject),
            "Box program state did not preserve released-memory status");

    BoxProgramState other = state;
    other.assignNumeric(source, LinearExpression(Rational(9)));
    BoxProgramState joined = state;
    joined.joinWith(other);
    require(other.isSubsetOf(joined) == CheckResult::True,
            "Box program-state join omitted a component");
}

void testLifetimeDomain()
{
    const Location object(10);
    LifetimeDomain alive = LifetimeDomain::bottom();
    alive.allocate(object);
    LifetimeDomain freed = alive;
    freed.release(object);
    require(alive.statusOf(object) == Lifetime::Alive &&
                freed.mustBeFreed(object),
            "lifetime copy-on-write changed the source property");

    LifetimeDomain maybeFreed = alive;
    maybeFreed.joinWith(freed);
    require(maybeFreed.mayBeFreed(object) && !maybeFreed.mustBeFreed(object),
            "lifetime join lost a path-dependent release");
    maybeFreed.meetWith(alive);
    require(maybeFreed.statusOf(object) == Lifetime::Alive,
            "lifetime meet did not recover the live alternative");
}

void testAddressDomain()
{
    const Variable p(1);
    const Variable q(2);
    const Location first(10);
    const Location second(20);
    AddressDomain unreachable = AddressDomain::bottom();
    require(unreachable.kind() == DomainKind::Address &&
                unreachable.isBottom() && unreachable.addressSet(p).isBottom(),
            "Address bottom did not represent an unreachable property");
    AddressDomain unknown = AddressDomain::top();
    require(unknown.isTop() && unknown.addressSet(p).isTop() &&
                unknown.nonDefaultVariables().empty(),
            "Address top did not represent an unknown pointer sparsely");
    AddressDomain independentTop = AddressDomain::top();
    independentTop.assign(p, AddressSet::singleton(first));
    require(unknown.isTop() && unknown.addressSet(p).isTop(),
            "mutating one Address Top changed another Top instance");
    require(
        Location::null().isNull() &&
            AddressSet::singleton(Location::null()).contains(Location::null()),
        "Address domain did not preserve the explicit null location");

    AddressDomain addresses = AddressDomain::top();
    addresses.assign(p, AddressSet::singleton(first));
    AddressDomain copy = addresses;
    copy.assign(p, AddressSet::singleton(second));
    require(addresses.addressSet(p).contains(first) &&
                !addresses.addressSet(p).contains(second) &&
                copy.addressSet(p).contains(second),
            "Address copy-on-write changed the source property");
    AddressDomain ranged = addresses;
    ranged.assign(q, AddressSet::singleton(second));
    const std::vector<Variable> beforeQ =
        ranged.nonDefaultVariablesBefore(q);
    require(beforeQ.size() == 1 && beforeQ.front() == p,
            "bounded Address observation crossed its stable-ID partition");

    AddressDomain joined = addresses;
    joined.joinWith(copy);
    require(joined.addressSet(p).contains(first) &&
                joined.addressSet(p).contains(second) &&
                joined.addressSet(p).hasIntersection(addresses.addressSet(p)) &&
                addresses.isSubsetOf(joined) == CheckResult::True &&
                copy.isSubsetOf(joined) == CheckResult::True,
            "Address join or ordering lost a possible location");

    AddressDomain unknownJoin = addresses;
    unknownJoin.joinWith(unknown);
    require(unknownJoin.isTop() && unknownJoin.addressSet(p).isTop(),
            "joining a known address with an unknown pointer was not Top");

    AddressDomain met = joined;
    met.meetWith(addresses);
    require(met.isEquivalentTo(addresses) == CheckResult::True,
            "Address meet did not compute set intersection");

    AddressDomain disjoint = AddressDomain::top();
    disjoint.assign(p, AddressSet::singleton(second));
    disjoint.meetWith(addresses);
    require(!disjoint.isBottom() && disjoint.addressSet(p).isBottom(),
            "an empty pointer intersection was confused with carrier Bottom");

    AddressDomain bottomIdentity = AddressDomain::bottom();
    bottomIdentity.joinWith(addresses);
    require(bottomIdentity.isEquivalentTo(addresses) == CheckResult::True,
            "whole-property Address Bottom was not the join identity");

    joined.forget(p);
    require(joined.isTop() && joined.addressSet(p).isTop() &&
                joined.addressSet(q).isTop(),
            "Address forget did not restore the missing-is-Top invariant");

    AddressDomain emptyPointer = AddressDomain::top();
    emptyPointer.assign(p, AddressSet::bottom());
    require(!emptyPointer.isBottom() && emptyPointer.addressSet(p).isBottom() &&
                emptyPointer.addressSet(q).isTop(),
            "an empty pointer fact was confused with whole-property Bottom");

    AddressSet reordered = AddressSet::bottom();
    reordered.insert(Location(40));
    reordered.insert(Location(10));
    reordered.insert(Location(30));
    reordered.insert(Location(20));
    reordered.insert(Location(20));
    std::vector<std::uint32_t> locationIds;
    for (Location location : reordered.locations())
        locationIds.push_back(location.id());
    require(locationIds == std::vector<std::uint32_t>({10, 20, 30, 40}),
            "AddressSet did not preserve sorted duplicate-free iteration");
    AddressSet isolatedSet = reordered;
    isolatedSet.insert(Location(50));
    require(!reordered.contains(Location(50)) &&
                isolatedSet.contains(Location(50)),
            "AddressSet growth changed a copied finite set");

    AddressDomain large = AddressDomain::top();
    std::vector<Variable> sparseVariables;
    for (std::uint32_t index = 0; index < 40; ++index)
    {
        const Variable variable(1000 + index * 97);
        sparseVariables.push_back(variable);
        large.assign(variable,
                     AddressSet::singleton(Location(100 + index)));
    }
    AddressDomain isolatedLarge = large;
    isolatedLarge.assign(sparseVariables[17],
                         AddressSet::singleton(Location(9999)));
    require(large.addressSet(sparseVariables[17]).contains(Location(117)) &&
                !large.addressSet(sparseVariables[17]).contains(
                    Location(9999)) &&
                isolatedLarge.addressSet(sparseVariables[17])
                    .contains(Location(9999)),
            "large sparse Address copy-on-write changed the source property");
    for (Variable variable : sparseVariables)
        isolatedLarge.forget(variable);
    require(isolatedLarge.isTop() &&
                large.nonDefaultVariables().size() == sparseVariables.size(),
            "large sparse Address erasure lost Top normalization or source "
            "isolation");

    BoxDomain numerical = BoxDomain::top();
    requireThrows([&] { unknown.joinWith(numerical); },
                  "AbstractDomain accepted a cross-kind lattice operation");
}

void testAddressDomainDifferential()
{
    using ReferenceSet = std::set<std::uint32_t>;
    using ReferenceDomain = std::map<std::uint32_t, ReferenceSet>;
    std::uint32_t random = 0x4d595df4U;
    auto next = [&] {
        random = random * 1664525U + 1013904223U;
        return random;
    };
    auto makeAddresses = [&](ReferenceSet& reference) {
        AddressSet value = AddressSet::bottom();
        const std::uint32_t count = next() % 6;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const std::uint32_t location = next() % 257;
            reference.insert(location);
            value.insert(Location(location));
        }
        return value;
    };
    auto compare = [&](const AddressDomain& actual,
                       const ReferenceDomain& reference) {
        const std::vector<Variable> variables = actual.nonDefaultVariables();
        require(variables.size() == reference.size(),
                "Address differential support size mismatch");
        std::size_t index = 0;
        for (const auto& [variableId, locations] : reference)
        {
            require(variables[index++].id() == variableId,
                    "Address differential support ordering mismatch");
            const AddressSet value = actual.addressSet(Variable(variableId));
            require(!value.isTop() && value.size() == locations.size(),
                    "Address differential cardinality mismatch");
            for (std::uint32_t location : locations)
                require(value.contains(Location(location)),
                        "Address differential member mismatch");
        }
    };

    AddressDomain domains[] = {AddressDomain::top(), AddressDomain::top()};
    ReferenceDomain references[2];
    for (std::size_t step = 0; step < 5000; ++step)
    {
        const std::size_t target = next() % 2;
        const std::size_t other = 1 - target;
        const std::uint32_t variable = 1 + next() % 4096;
        switch (next() % 5)
        {
        case 0:
        {
            ReferenceSet locations;
            AddressSet value = makeAddresses(locations);
            domains[target].assign(Variable(variable), std::move(value));
            references[target][variable] = std::move(locations);
            break;
        }
        case 1:
            domains[target].forget(Variable(variable));
            references[target].erase(variable);
            break;
        case 2:
            domains[target] = domains[other];
            references[target] = references[other];
            break;
        case 3:
        {
            domains[target].joinWith(domains[other]);
            ReferenceDomain joined;
            for (const auto& [key, left] : references[target])
            {
                const auto found = references[other].find(key);
                if (found == references[other].end())
                    continue;
                ReferenceSet value = left;
                value.insert(found->second.begin(), found->second.end());
                joined.emplace(key, std::move(value));
            }
            references[target] = std::move(joined);
            break;
        }
        case 4:
        {
            domains[target].meetWith(domains[other]);
            ReferenceDomain met = references[target];
            for (const auto& [key, right] : references[other])
            {
                const auto found = met.find(key);
                if (found == met.end())
                {
                    met.emplace(key, right);
                    continue;
                }
                ReferenceSet intersection;
                std::set_intersection(
                    found->second.begin(), found->second.end(), right.begin(),
                    right.end(), std::inserter(intersection, intersection.end()));
                found->second = std::move(intersection);
            }
            references[target] = std::move(met);
            break;
        }
        }
        compare(domains[0], references[0]);
        compare(domains[1], references[1]);
    }
}
} // namespace

int main()
{
    try
    {
        testLatticeAndTransferSurface();
        testScalarTransferOperations();
        testStableVocabularyExpandFoldAndTrees();
        testPagedCopyOnWriteAndSerialization();
        testProgramStateMemoryFacet();
        testLifetimeDomain();
        testAddressDomain();
        testAddressDomainDifferential();
        std::cout << "SVF AE core domain test: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SVF AE core domain test: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
