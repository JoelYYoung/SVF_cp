//===- RelExeState.cpp ---- Relation execution state ----------------------===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013-2022>  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
/*
 * RelExeState.cpp
 *
 *  Created on: Aug 15, 2022
 *      Author: Jiawei Ren, Xiao Cheng
 *
 */

#include "AE/Core/RelExeState.h"
#include "Util/SVFUtil.h"

#include <iomanip>
#include <sstream>

using namespace SVF;
using namespace SVFUtil;

void RelExeState::extractSubVars(const Z3Expr& expr, Set<u32_t>& res)
{
    if (expr.getExpr().num_args() == 0 && !expr.getExpr().is_true() &&
            !expr.getExpr().is_false() && !expr.is_numeral())
    {
        const std::string exprStr = expr.to_string();
        res.insert(std::stoi(exprStr.substr(1, exprStr.size() - 1)));
    }
    for (u32_t i = 0; i < expr.getExpr().num_args(); ++i)
    {
        const z3::expr& argument = expr.getExpr().arg(i);
        extractSubVars(argument, res);
    }
}

void RelExeState::extractCmpVars(const Z3Expr& expr, Set<u32_t>& res)
{
    Set<u32_t> directVars;
    extractSubVars(expr, directVars);
    res.insert(directVars.begin(), directVars.end());
    assert(!directVars.empty() && "symbol not init?");
    if (directVars.size() == 1 && eq(expr, toZ3Expr(*directVars.begin())))
        return;

    for (u32_t id : directVars)
        extractCmpVars((*this)[id], res);
}

Z3Expr RelExeState::buildRelZ3Expr(u32_t cmp, s32_t succ, Set<u32_t>& vars,
                                   Set<u32_t>& initVars)
{
    Z3Expr result = (getZ3Expr(cmp) == succ).simplify();
    extractSubVars(result, initVars);
    extractCmpVars(result, vars);
    for (u32_t id : vars)
        result = (result && toZ3Expr(id) == getZ3Expr(id)).simplify();
    result = (result && (toZ3Expr(cmp) == getZ3Expr(cmp))).simplify();
    vars.insert(cmp);
    return result;
}

RelExeState& RelExeState::operator=(const RelExeState& rhs)
{
    if (*this != rhs)
    {
        _varToVal = rhs.getVarToVal();
        _addrToVal = rhs.getLocToVal();
    }
    return *this;
}

RelExeState& RelExeState::operator=(RelExeState&& rhs) noexcept
{
    if (this != &rhs)
    {
        _varToVal = std::move(rhs._varToVal);
        _addrToVal = std::move(rhs._addrToVal);
    }
    return *this;
}

bool RelExeState::operator==(const RelExeState& rhs) const
{
    return eqVarToValMap(_varToVal, rhs.getVarToVal()) &&
           eqVarToValMap(_addrToVal, rhs.getLocToVal());
}

bool RelExeState::operator<(const RelExeState& rhs) const
{
    return lessThanVarToValMap(_varToVal, rhs.getVarToVal()) ||
           lessThanVarToValMap(_addrToVal, rhs.getLocToVal());
}

u32_t RelExeState::hash() const
{
    std::size_t variableHash = getVarToVal().size() * 2;
    Hash<u32_t> hashValue;
    for (const auto& [variable, expression] : getVarToVal())
    {
        variableHash ^= hashValue(variable) + 0x9e3779b9 +
                        (variableHash << 6) + (variableHash >> 2);
        variableHash ^= hashValue(expression.id()) + 0x9e3779b9 +
                        (variableHash << 6) + (variableHash >> 2);
    }

    std::size_t locationHash = getVarToVal().size() * 2;
    for (const auto& [location, expression] : getLocToVal())
    {
        locationHash ^= hashValue(location) + 0x9e3779b9 +
                        (locationHash << 6) + (locationHash >> 2);
        locationHash ^= hashValue(expression.id()) + 0x9e3779b9 +
                        (locationHash << 6) + (locationHash >> 2);
    }

    Hash<std::pair<u32_t, u32_t>> pairHash;
    return pairHash(std::make_pair(static_cast<u32_t>(variableHash),
                                   static_cast<u32_t>(locationHash)));
}

bool RelExeState::eqVarToValMap(const VarToValMap& lhs,
                                const VarToValMap& rhs) const
{
    if (lhs.size() != rhs.size())
        return false;
    for (const auto& [variable, expression] : lhs)
    {
        const auto found = rhs.find(variable);
        if (found == rhs.end() || !eq(expression, found->second))
            return false;
    }
    return true;
}

bool RelExeState::lessThanVarToValMap(const VarToValMap& lhs,
                                      const VarToValMap& rhs) const
{
    if (lhs.size() != rhs.size())
        return lhs.size() < rhs.size();
    for (const auto& [variable, expression] : lhs)
    {
        const auto found = rhs.find(variable);
        if (found == rhs.end())
            return false;
        if (!eq(expression, found->second))
            return expression.id() < found->second.id();
    }
    return false;
}

void RelExeState::store(const Z3Expr& loc, const Z3Expr& value)
{
    assert(loc.is_numeral() && "location must be numeral");
    const s32_t virtualAddress = z3Expr2NumValue(loc);
    assert(isVirtualMemAddress(virtualAddress) &&
           "pointer operand is not a virtual address");
    store(getInternalID(virtualAddress), value);
}

Z3Expr& RelExeState::load(const Z3Expr& loc)
{
    assert(loc.is_numeral() && "location must be numeral");
    const s32_t virtualAddress = z3Expr2NumValue(loc);
    assert(isVirtualMemAddress(virtualAddress) &&
           "pointer operand is not a virtual address");
    const u32_t objectId = getInternalID(virtualAddress);
    assert(getInternalID(objectId) == objectId &&
           "SVFVar index overflow > 0x7f000000");
    return load(objectId);
}

void RelExeState::printExprValues()
{
    std::cout.flags(std::ios::left);
    std::cout << "-----------Var and Value-----------\n";
    for (const auto& [variable, expression] : getVarToVal())
    {
        std::stringstream expressionName;
        expressionName << "Var" << variable;
        std::cout << std::setw(25) << expressionName.str();
        const Z3Expr simplified = expression.simplify();
        if (simplified.is_numeral() &&
                isVirtualMemAddress(z3Expr2NumValue(simplified)))
        {
            std::cout << "\t Value: " << std::hex
                      << "0x" << z3Expr2NumValue(simplified) << "\n";
        }
        else
            std::cout << "\t Value: " << std::dec << simplified << "\n";
    }
    std::cout << "-----------------------------------------\n";
}
