//===- Variable.h -- Typed abstract-domain variable identity -*- C++ -*-===//
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

#ifndef SVF_AE_VARIABLE_H
#define SVF_AE_VARIABLE_H

#include <cstdint>
#include <tuple>

namespace SVF::AbstractDomain
{

struct FloatFormat
{
    unsigned exponentBits = 0;
    unsigned significandBits = 0;

    static FloatFormat binary32()
    {
        return {8, 24};
    }
    static FloatFormat binary64()
    {
        return {11, 53};
    }
};

enum class NumericKind
{
    Integer,
    Real,
    IEEEFloat
};

struct NumericType
{
    NumericKind kind = NumericKind::Integer;
    FloatFormat floatFormat{};

    static NumericType integer()
    {
        return {NumericKind::Integer, {}};
    }
    static NumericType real()
    {
        return {NumericKind::Real, {}};
    }
    static NumericType ieee(FloatFormat format)
    {
        return {NumericKind::IEEEFloat, format};
    }

    friend bool operator==(const NumericType& lhs, const NumericType& rhs)
    {
        return lhs.kind == rhs.kind &&
               lhs.floatFormat.exponentBits == rhs.floatFormat.exponentBits &&
               lhs.floatFormat.significandBits ==
                   rhs.floatFormat.significandBits;
    }
    friend bool operator!=(const NumericType& lhs, const NumericType& rhs)
    {
        return !(lhs == rhs);
    }
};

/// Stable identity of one scalar or object-content value within an analysis.
///
/// The ID is also the non-relational domains' global sparse-storage coordinate.
/// Adapters should allocate IDs densely for page locality, but correctness does
/// not depend on density because missing pages denote Top. Numeric type is part
/// of the identity so Box operations never need a payload-owned vocabulary just
/// to interpret integer, real, or IEEE bounds.
class Variable
{
public:
    explicit Variable(std::uint32_t id = 0,
                      NumericType type = NumericType::integer())
        : id_(id), type_(type)
    {
    }

    std::uint32_t id() const
    {
        return id_;
    }
    const NumericType& type() const
    {
        return type_;
    }

    friend bool operator==(Variable lhs, Variable rhs)
    {
        return lhs.id_ == rhs.id_ && lhs.type_ == rhs.type_;
    }
    friend bool operator!=(Variable lhs, Variable rhs)
    {
        return !(lhs == rhs);
    }
    friend bool operator<(Variable lhs, Variable rhs)
    {
        return std::tie(lhs.id_, lhs.type_.kind,
                        lhs.type_.floatFormat.exponentBits,
                        lhs.type_.floatFormat.significandBits) <
               std::tie(rhs.id_, rhs.type_.kind,
                        rhs.type_.floatFormat.exponentBits,
                        rhs.type_.floatFormat.significandBits);
    }

private:
    std::uint32_t id_;
    NumericType type_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_VARIABLE_H
