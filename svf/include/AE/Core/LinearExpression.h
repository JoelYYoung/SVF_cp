//===- LinearExpression.h -- Domain-neutral linear syntax -------*- C++ -*-===//

#ifndef SVF_AE_LINEAR_EXPRESSION_H
#define SVF_AE_LINEAR_EXPRESSION_H

#include "AE/Core/NumericalDomain.h"

#include <map>
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

} // namespace SVF::AbstractDomain

#endif // SVF_AE_LINEAR_EXPRESSION_H
