//===- Expression.h -- Domain-neutral numerical expressions ----*- C++ -*-===//
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
// Contributors: Jiawei Wang, Xiao Cheng, Jiawei Yang
//
//===----------------------------------------------------------------------===//

#ifndef SVF_AE_EXPRESSION_H
#define SVF_AE_EXPRESSION_H

#include "AE/Core/NumericalDomain.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SVF::AbstractDomain
{

class LinearExpression
{
public:
    using Terms = std::map<Variable, Rational>;

    LinearExpression();
    explicit LinearExpression(Rational constant);
    explicit LinearExpression(Variable variable);

    const Terms& terms() const
    {
        return terms_;
    }
    const Rational& constant() const
    {
        return constant_;
    }
    Rational coefficient(Variable variable) const;

    LinearExpression& setCoefficient(Variable variable, Rational coefficient);
    LinearExpression& setConstant(Rational constant);
    LinearExpression& operator+=(const LinearExpression& rhs);
    LinearExpression& operator-=(const LinearExpression& rhs);
    LinearExpression& operator*=(const Rational& scalar);

    /// Simultaneously replace variables in this expression. Replacement
    /// expressions are inserted verbatim: variables occurring inside a
    /// replacement are pre-state variables and are not recursively replaced.
    LinearExpression substituted(
        const std::map<Variable, LinearExpression>& replacements) const;

    std::string toString() const;

    friend LinearExpression operator+(LinearExpression lhs,
                                      const LinearExpression& rhs)
    {
        return lhs += rhs;
    }
    friend LinearExpression operator-(LinearExpression lhs,
                                      const LinearExpression& rhs)
    {
        return lhs -= rhs;
    }
    friend LinearExpression operator*(LinearExpression lhs,
                                      const Rational& scalar)
    {
        return lhs *= scalar;
    }
    friend LinearExpression operator*(const Rational& scalar,
                                      LinearExpression rhs)
    {
        return rhs *= scalar;
    }
    friend LinearExpression operator-(LinearExpression expression)
    {
        return expression *= Rational(-1);
    }

private:
    void removeZeroTerms();

    Terms terms_;
    Rational constant_;
};
enum class ConstraintKind
{
    Equal,
    NotEqual,
    LessThan,
    LessEqual,
    GreaterThan,
    GreaterEqual
};

/// A normalized constraint of the form expression (relation) 0.
class LinearConstraint
{
public:
    LinearConstraint(LinearExpression expression, ConstraintKind kind);

    const LinearExpression& expression() const
    {
        return expression_;
    }
    ConstraintKind kind() const
    {
        return kind_;
    }
    std::string toString() const;

private:
    LinearExpression expression_;
    ConstraintKind kind_;
};

using LinearConstraintSet = std::vector<LinearConstraint>;

struct WideningPolicy
{
    WideningPolicy() = default;

    explicit WideningPolicy(std::vector<Rational> thresholdValues)
        : thresholds(std::move(thresholdValues))
    {
    }

    WideningPolicy(std::vector<Rational> thresholdValues,
                   LinearConstraintSet linearThresholdValues)
        : thresholds(std::move(thresholdValues)),
          linearThresholds(std::move(linearThresholdValues))
    {
    }

    std::vector<Rational> thresholds;
    LinearConstraintSet linearThresholds;
};

struct LinearAssignment
{
    Variable target;
    LinearExpression expression;
};

using LinearAssignmentList = std::vector<LinearAssignment>;
LinearConstraint equal(LinearExpression lhs, LinearExpression rhs);
LinearConstraint notEqual(LinearExpression lhs, LinearExpression rhs);
LinearConstraint lessEqual(LinearExpression lhs, LinearExpression rhs);
LinearConstraint lessThan(LinearExpression lhs, LinearExpression rhs);
LinearConstraint greaterEqual(LinearExpression lhs, LinearExpression rhs);
LinearConstraint greaterThan(LinearExpression lhs, LinearExpression rhs);

enum class UnaryOperator
{
    Negate,
    Cast,
    SquareRoot
};

enum class BinaryOperator
{
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder
};

class TreeExpression
{
public:
    enum class Kind
    {
        Constant,
        Variable,
        Unary,
        Binary
    };

    static TreeExpression constant(Rational value,
                                   NumericType type = NumericType::real());
    static TreeExpression variable(Variable value, NumericType type);
    static TreeExpression unary(
        UnaryOperator operation, TreeExpression operand, NumericType type,
        RoundingMode rounding = RoundingMode::NearestTiesToEven);
    static TreeExpression binary(
        BinaryOperator operation, TreeExpression lhs, TreeExpression rhs,
        NumericType type,
        RoundingMode rounding = RoundingMode::NearestTiesToEven);

    Kind kind() const
    {
        return kind_;
    }
    const NumericType& type() const
    {
        return type_;
    }
    const Rational& constant() const
    {
        return constant_;
    }
    Variable variable() const
    {
        return variable_;
    }
    UnaryOperator unaryOperator() const
    {
        return unaryOperator_;
    }
    BinaryOperator binaryOperator() const
    {
        return binaryOperator_;
    }
    RoundingMode roundingMode() const
    {
        return roundingMode_;
    }
    const TreeExpression& lhs() const;
    const TreeExpression& rhs() const;

    /// Return an exact affine expression when the tree is affine under
    /// mathematical integer/real semantics. Floating and nonlinear trees
    /// deliberately return nullopt and must use a sound backend fallback.
    std::optional<LinearExpression> asLinear() const;

private:
    Kind kind_ = Kind::Constant;
    NumericType type_ = NumericType::real();
    Rational constant_;
    Variable variable_;
    UnaryOperator unaryOperator_ = UnaryOperator::Negate;
    BinaryOperator binaryOperator_ = BinaryOperator::Add;
    RoundingMode roundingMode_ = RoundingMode::NearestTiesToEven;
    std::shared_ptr<const TreeExpression> lhs_;
    std::shared_ptr<const TreeExpression> rhs_;
};

class TreeConstraint
{
public:
    TreeConstraint(TreeExpression expression, ConstraintKind kind);

    const TreeExpression& expression() const
    {
        return expression_;
    }
    ConstraintKind kind() const
    {
        return kind_;
    }

private:
    TreeExpression expression_;
    ConstraintKind kind_;
};

struct TreeAssignment
{
    Variable target;
    TreeExpression expression;
};

using TreeAssignmentList = std::vector<TreeAssignment>;

} // namespace SVF::AbstractDomain

#endif // SVF_AE_EXPRESSION_H
