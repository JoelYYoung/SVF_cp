//===- RelExeStateTest.cpp -- Relation execution-state tests --------------===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

#include "AE/Core/RelExeState.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace SVF;

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testVariableExpressions()
{
    RelExeState state;
    state[0] = state.toZ3Expr(0);
    state[1] = state.toZ3Expr(0) + 2;

    Set<u32_t> variables;
    state.extractSubVars(state[1], variables);
    require(variables == Set<u32_t>({0}),
            "RelExeState did not recover a symbolic dependency");
    require(state.existsVar(1) && !state.existsVar(2),
            "RelExeState reported the wrong stored variables");

    Set<u32_t> relatedVariables;
    Set<u32_t> initialVariables;
    const Z3Expr relation =
        state.buildRelZ3Expr(1, 2, relatedVariables, initialVariables);
    require(relatedVariables == Set<u32_t>({0, 1}) &&
            initialVariables == Set<u32_t>({0}) &&
            !relation.getExpr().is_false(),
            "RelExeState did not build the expected relation closure");

    RelExeState copied(state);
    require(copied == state && copied.hash() == state.hash(),
            "RelExeState copy changed equality or hashing");
    copied[1] = copied.toZ3Expr(0) + 3;
    require(copied != state,
            "RelExeState equality ignored a changed expression");
}

void testEncodedMemory()
{
    RelExeState state;
    constexpr u32_t object = 42;
    const u32_t encoded = RelExeState::getVirtualMemAddress(object);
    require(RelExeState::isVirtualMemAddress(encoded) &&
            RelExeState::getInternalID(encoded) == object,
            "RelExeState address encoding did not round-trip");

    const Z3Expr address = RelExeState::getContext().int_val(encoded);
    const Z3Expr value = RelExeState::getContext().int_val(17);
    state.store(address, value);
    require(eq(state.load(address), value),
            "RelExeState did not preserve a stored expression");
}
} // namespace

int main()
{
    try
    {
        testVariableExpressions();
        testEncodedMemory();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
