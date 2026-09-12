//===- RelExeState.h ---- Relation execution state ------------------------===//
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
 * RelExeState.h
 *
 *  Created on: Aug 15, 2022
 *      Author: Jiawei Ren, Xiao Cheng
 *
 */

#ifndef SVF_AE_RELEXESTATE_H
#define SVF_AE_RELEXESTATE_H

#include "Util/GeneralType.h"
#include "Util/Z3Expr.h"

#include <functional>
#include <utility>

namespace SVF
{

class RelExeState
{
    friend class SVFIR2AbsState;

public:
    using VarToValMap = Map<u32_t, Z3Expr>;
    using AddrToValMap = VarToValMap;

protected:
    VarToValMap _varToVal;
    AddrToValMap _addrToVal;

public:
    RelExeState() = default;
    RelExeState(VarToValMap& varToVal, AddrToValMap& locToVal)
        : _varToVal(varToVal), _addrToVal(locToVal)
    {
    }
    RelExeState(const RelExeState& rhs)
        : _varToVal(rhs.getVarToVal()), _addrToVal(rhs.getLocToVal())
    {
    }
    RelExeState(RelExeState&& rhs) noexcept
        : _varToVal(std::move(rhs._varToVal)),
          _addrToVal(std::move(rhs._addrToVal))
    {
    }
    virtual ~RelExeState() = default;

    RelExeState& operator=(const RelExeState& rhs);
    RelExeState& operator=(RelExeState&& rhs) noexcept;

    bool operator==(const RelExeState& rhs) const;
    bool operator!=(const RelExeState& rhs) const
    {
        return !(*this == rhs);
    }
    bool operator<(const RelExeState& rhs) const;

    static z3::context& getContext()
    {
        return Z3Expr::getContext();
    }

    const VarToValMap& getVarToVal() const
    {
        return _varToVal;
    }
    const AddrToValMap& getLocToVal() const
    {
        return _addrToVal;
    }

    Z3Expr& operator[](u32_t varId)
    {
        return getZ3Expr(varId);
    }

    u32_t hash() const;

    bool existsVar(u32_t varId) const
    {
        return _varToVal.count(varId);
    }

    virtual Z3Expr& getZ3Expr(u32_t varId)
    {
        return _varToVal[varId];
    }

    virtual Z3Expr toZ3Expr(u32_t varId) const
    {
        return getContext().int_const(std::to_string(varId).c_str());
    }

    void extractSubVars(const Z3Expr& expr, Set<u32_t>& res);
    void extractCmpVars(const Z3Expr& expr, Set<u32_t>& res);
    Z3Expr buildRelZ3Expr(u32_t cmp, s32_t succ, Set<u32_t>& vars,
                          Set<u32_t>& initVars);

    void store(const Z3Expr& loc, const Z3Expr& value);
    Z3Expr& load(const Z3Expr& loc);

    static u32_t getVirtualMemAddress(u32_t idx)
    {
        assert(idx != 0 && "idx cannot be 0 because it represents nullptr");
        return AddressMask + idx;
    }

    static bool isVirtualMemAddress(u32_t val)
    {
        if (val == 0)
            assert(false && "val cannot be 0");
        return (val & 0xff000000U) == AddressMask;
    }

    static u32_t getInternalID(u32_t idx)
    {
        return idx & FlippedAddressMask;
    }

    static s32_t z3Expr2NumValue(const Z3Expr& expression)
    {
        assert(expression.is_numeral() && "not numeral?");
        return expression.get_numeral_int64();
    }

    void printExprValues();

private:
    static constexpr u32_t AddressMask = 0x7f000000U;
    static constexpr u32_t FlippedAddressMask = AddressMask ^ 0xffffffffU;

    bool eqVarToValMap(const VarToValMap& lhs,
                       const VarToValMap& rhs) const;
    bool lessThanVarToValMap(const VarToValMap& lhs,
                             const VarToValMap& rhs) const;

protected:
    void store(u32_t objId, const Z3Expr& expression)
    {
        _addrToVal[objId] = expression.simplify();
    }
    Z3Expr& load(u32_t objId)
    {
        return _addrToVal[objId];
    }
};

} // namespace SVF

template <>
struct std::hash<SVF::RelExeState>
{
    std::size_t operator()(const SVF::RelExeState& exeState) const
    {
        return exeState.hash();
    }
};

#endif // SVF_AE_RELEXESTATE_H
