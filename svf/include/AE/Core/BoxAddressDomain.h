//===- BoxAddressDomain.h -- Box/address reduced product ------*- C++ -*-===//
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

#ifndef SVF_AE_BOX_ADDRESS_DOMAIN_H
#define SVF_AE_BOX_ADDRESS_DOMAIN_H

#include "AE/Core/AbstractDomain.h"
#include "AE/Core/AddressDomain.h"
#include "AE/Core/Expression.h"
#include "AE/Core/NumericalDomain.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace SVF::AbstractDomain
{

enum class Lifetime
{
    Bottom,
    Alive,
    Freed,
    MaybeFreed
};

class LifetimeDomain final : public AbstractDomain
{
public:
    static LifetimeDomain top();
    static LifetimeDomain bottom();

    DomainKind kind() const noexcept override
    {
        return DomainKind::Lifetime;
    }
    std::unique_ptr<AbstractDomain> clone() const override;

    Lifetime statusOf(Location location) const;
    void allocate(Location location);
    void release(Location location);
    bool mayBeFreed(Location location) const;
    bool mustBeFreed(Location location) const;

private:
    using Values = std::map<Location, Lifetime>;
    explicit LifetimeDomain(Lifetime defaultValue)
        : defaultValue_(defaultValue), values_(std::make_shared<Values>())
    {
    }

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<LifetimeDomain>();
    }
    bool hasCompatibleDomain(const AbstractDomain& other) const override;
    void joinDomain(const AbstractDomain& other) override;
    void meetDomain(const AbstractDomain& other) override;
    void widenDomain(const AbstractDomain& next) override;
    void narrowDomain(const AbstractDomain& next) override;
    bool isBottomDomain() const override;
    bool isTopDomain() const override;
    bool leqDomain(const AbstractDomain& other) const override;
    std::string domainToString() const override;

    void set(Location location, Lifetime lifetime);
    Values& writableValues();

    Lifetime defaultValue_ = Lifetime::Bottom;
    std::shared_ptr<Values> values_;
};

/// Monotone analysis-wide schema mapping abstract locations to the scalar
/// symbols denoting their stored contents. Copies share the schema so objects
/// discovered lazily by the frontend become visible to existing states. Such
/// an extension does not mutate abstract values: every new coordinate has the
/// domains' implicit Top value until a transfer assigns it.
class MemoryLayout
{
public:
    MemoryLayout() : cells_(std::make_shared<Cells>()) {}
    explicit MemoryLayout(std::map<Location, Variable> cells)
        : cells_(std::make_shared<Cells>(std::move(cells)))
    {
    }

    bool contains(Location location) const
    {
        return cells_->count(location) != 0;
    }
    void extend(Location location, Variable content);
    Variable contentOf(Location location) const;
    const std::map<Location, Variable>& cells() const
    {
        return *cells_;
    }

    friend bool operator==(const MemoryLayout& lhs, const MemoryLayout& rhs)
    {
        return lhs.cells_ == rhs.cells_ || *lhs.cells_ == *rhs.cells_;
    }

private:
    using Cells = std::map<Location, Variable>;
    std::shared_ptr<Cells> cells_;
};

/// Reduced product used by AE: Box tracks numerical facts, AddressDomain tracks
/// pointer facts, and LifetimeDomain tracks allocation status. Memory contents
/// are ordinary abstract variables in Box or AddressDomain, connected to
/// locations by the shared MemoryLayout.
class BoxAddressDomain final : public AbstractDomain
{
public:
    BoxAddressDomain(BoxDomain numerical, MemoryLayout memoryLayout)
        : numerical_(std::move(numerical)),
          memoryLayout_(std::move(memoryLayout)),
          addresses_(AddressDomain::top()), lifetimes_(LifetimeDomain::bottom())
    {
    }

    BoxAddressDomain(BoxDomain numerical, MemoryLayout memoryLayout,
                    AddressDomain addresses, LifetimeDomain lifetimes)
        : numerical_(std::move(numerical)),
          memoryLayout_(std::move(memoryLayout)),
          addresses_(std::move(addresses)), lifetimes_(std::move(lifetimes))
    {
    }

    DomainKind kind() const noexcept override
    {
        return DomainKind::Product;
    }
    std::unique_ptr<AbstractDomain> clone() const override
    {
        return std::make_unique<BoxAddressDomain>(*this);
    }

    BoxDomain& numerical()
    {
        return numerical_;
    }
    const BoxDomain& numerical() const
    {
        return numerical_;
    }
    AddressDomain& addresses()
    {
        return addresses_;
    }
    const AddressDomain& addresses() const
    {
        return addresses_;
    }
    LifetimeDomain& lifetimes()
    {
        return lifetimes_;
    }
    const LifetimeDomain& lifetimes() const
    {
        return lifetimes_;
    }
    const MemoryLayout& memoryLayout() const
    {
        return memoryLayout_;
    }

    void assignPointer(Variable target, const AddressSet& value)
    {
        addresses_.assign(target, value);
        numerical_.forget(target);
    }

    void assignNumeric(Variable target, const LinearExpression& expression)
    {
        numerical_.assign(target, expression);
        addresses_.forget(target);
    }

    void assignNumericParallel(const LinearAssignmentList& assignments)
    {
        numerical_.assignParallel(assignments);
        for (const LinearAssignment& assignment : assignments)
        {
            addresses_.forget(assignment.target);
        }
    }

    void assignNumericParallel(const TreeAssignmentList& assignments)
    {
        numerical_.assignParallel(assignments);
        for (const TreeAssignment& assignment : assignments)
        {
            addresses_.forget(assignment.target);
        }
    }

    void assume(const LinearConstraint& constraint)
    {
        numerical_.assume(constraint);
    }

    void load(Variable target, Variable pointer)
    {
        const AddressSet pointees = addresses_.addressSet(pointer);
        if (pointees.isTop() || pointees.isBottom())
        {
            numerical_.forget(target);
            if (pointees.isTop())
                addresses_.forget(target);
            else
                addresses_.assign(target, AddressSet::bottom());
            return;
        }

        bool first = true;
        BoxAddressDomain result(*this);
        for (Location location : pointees.locations())
        {
            if (!memoryLayout_.contains(location))
                continue;
            BoxAddressDomain alternative(*this);
            const Variable content = memoryLayout_.contentOf(location);
            alternative.numerical_.assign(target, LinearExpression(content));
            alternative.addresses_.assign(
                target, alternative.addresses_.addressSet(content));
            if (first)
            {
                result = std::move(alternative);
                first = false;
            }
            else
            {
                result.joinDomain(alternative);
            }
        }
        if (first)
        {
            numerical_.forget(target);
            addresses_.forget(target);
        }
        else
        {
            *this = std::move(result);
        }
    }

    void store(Variable pointer, Variable source)
    {
        const AddressSet pointees = addresses_.addressSet(pointer);
        if (pointees.isTop())
        {
            for (const auto& [location, content] : memoryLayout_.cells())
            {
                (void)location;
                weakStore(content, source);
            }
            return;
        }
        if (pointees.isBottom())
            return;
        if (pointees.isSingleton())
        {
            const Location location = *pointees.locations().begin();
            if (memoryLayout_.contains(location))
                strongStore(memoryLayout_.contentOf(location), source);
            return;
        }
        for (Location location : pointees.locations())
        {
            if (memoryLayout_.contains(location))
                weakStore(memoryLayout_.contentOf(location), source);
        }
    }

    void allocate(Location location)
    {
        lifetimes_.allocate(location);
    }

    void release(Variable pointer)
    {
        const AddressSet pointees = addresses_.addressSet(pointer);
        if (pointees.isTop())
        {
            for (const auto& [location, content] : memoryLayout_.cells())
            {
                (void)content;
                lifetimes_.release(location);
            }
            return;
        }
        for (Location location : pointees.locations())
            lifetimes_.release(location);
    }

private:
    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<BoxAddressDomain>();
    }
    bool hasCompatibleDomain(const AbstractDomain& other) const override
    {
        const auto* product = other.isDomain<BoxAddressDomain>()
                                  ? &static_cast<const BoxAddressDomain&>(other)
                                  : nullptr;
        return product && memoryLayout_ == product->memoryLayout_ &&
               numerical_.config().operationCompatible(
                   product->numerical_.config());
    }

    void joinDomain(const AbstractDomain& other) override
    {
        const BoxAddressDomain& product = requireProduct(other);
        if (product.isBottomDomain())
            return;
        if (isBottomDomain())
        {
            *this = product;
            return;
        }
        numerical_.joinWith(product.numerical_);
        addresses_.joinWith(product.addresses_);
        lifetimes_.joinWith(product.lifetimes_);
    }

    void meetDomain(const AbstractDomain& other) override
    {
        const BoxAddressDomain& product = requireProduct(other);
        if (product.isTopDomain())
            return;
        if (isTopDomain())
        {
            *this = product;
            return;
        }
        numerical_.meetWith(product.numerical_);
        addresses_.meetWith(product.addresses_);
        lifetimes_.meetWith(product.lifetimes_);
    }

    void widenDomain(const AbstractDomain& next) override
    {
        const BoxAddressDomain& product = requireProduct(next);
        if (isBottomDomain())
        {
            *this = product;
            return;
        }
        numerical_.widenWith(product.numerical_);
        addresses_.widenWith(product.addresses_);
        lifetimes_.widenWith(product.lifetimes_);
    }

    void narrowDomain(const AbstractDomain& next) override
    {
        const BoxAddressDomain& product = requireProduct(next);
        if (isTopDomain())
        {
            *this = product;
            return;
        }
        numerical_.narrowWith(product.numerical_);
        addresses_.narrowWith(product.addresses_);
        lifetimes_.narrowWith(product.lifetimes_);
    }

    bool isBottomDomain() const override
    {
        return numerical_.isBottom() || addresses_.isBottom();
    }

    bool isTopDomain() const override
    {
        // At the typed program-state layer an absent address facet means that
        // no pointer-specific constraint has been materialized; pointer reads
        // conservatively project it as Address Top. Likewise, absent lifetime
        // facts impose no release constraint. This is the canonical
        // unconstrained flow state used by dense and sparse AE.
        return numerical_.isTop() && addresses_.isTop() &&
               lifetimes_.isBottom();
    }

    bool leqDomain(const AbstractDomain& other) const override
    {
        const BoxAddressDomain& product = requireProduct(other);
        if (isBottomDomain() || product.isTopDomain())
            return true;
        if (product.isBottomDomain())
            return false;
        return numerical_.isSubsetOf(product.numerical_) == CheckResult::True &&
               addresses_.isSubsetOf(product.addresses_) == CheckResult::True &&
               lifetimes_.isSubsetOf(product.lifetimes_) == CheckResult::True;
    }

    std::string domainToString() const override
    {
        return "numeric=" + numerical_.toString() +
               ", addresses=" + addresses_.toString() +
               ", lifetimes=" + lifetimes_.toString();
    }

    const BoxAddressDomain& requireProduct(const AbstractDomain& other) const
    {
        requireCompatible(other);
        return static_cast<const BoxAddressDomain&>(other);
    }

    void strongStore(Variable content, Variable source)
    {
        numerical_.assign(content, LinearExpression(source));
        addresses_.assign(content, addresses_.addressSet(source));
    }

    void weakStore(Variable content, Variable source)
    {
        BoxAddressDomain alternative(*this);
        alternative.strongStore(content, source);
        joinDomain(alternative);
    }

    BoxDomain numerical_;
    MemoryLayout memoryLayout_;
    AddressDomain addresses_;
    LifetimeDomain lifetimes_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_BOX_ADDRESS_DOMAIN_H
