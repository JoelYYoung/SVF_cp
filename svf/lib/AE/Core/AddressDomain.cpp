//===- AddressDomain.cpp -- Address-set abstract property ----------------===//
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
// Contributors: Xiao Cheng, Jiawei Yang
//
//===----------------------------------------------------------------------===//

#include "AE/Core/AddressDomain.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SVF::AbstractDomain
{
FiniteLocationSet::FiniteLocationSet(const FiniteLocationSet& other)
    : size_(other.size_), inline_(other.inline_)
{
    if (other.overflow_)
        overflow_ =
            std::make_unique<std::vector<Location>>(*other.overflow_);
}

FiniteLocationSet& FiniteLocationSet::operator=(
    const FiniteLocationSet& other)
{
    if (this == &other)
        return *this;
    size_ = other.size_;
    inline_ = other.inline_;
    overflow_ = other.overflow_
                    ? std::make_unique<std::vector<Location>>(*other.overflow_)
                    : nullptr;
    return *this;
}

std::size_t FiniteLocationSet::size() const
{
    return size_;
}

bool FiniteLocationSet::empty() const
{
    return size_ == 0;
}

bool FiniteLocationSet::contains(Location location) const
{
    if (size_ <= InlineCapacity)
        return std::find(begin(), end(), location) != end();
    return std::binary_search(begin(), end(), location);
}

FiniteLocationSet::const_iterator FiniteLocationSet::begin() const
{
    return overflow_ ? overflow_->data() : inline_.data();
}

FiniteLocationSet::const_iterator FiniteLocationSet::end() const
{
    return begin() + size_;
}

void FiniteLocationSet::insert(Location location)
{
    const auto position = std::lower_bound(begin(), end(), location);
    if (position != end() && *position == location)
        return;
    const std::size_t index = static_cast<std::size_t>(position - begin());
    if (!overflow_ && size_ < InlineCapacity)
    {
        std::move_backward(inline_.begin() + index, inline_.begin() + size_,
                           inline_.begin() + size_ + 1);
        inline_[index] = location;
        ++size_;
        return;
    }
    if (!overflow_)
    {
        overflow_ = std::make_unique<std::vector<Location>>(inline_.begin(),
                                                             inline_.end());
        overflow_->reserve(InlineCapacity * 2);
    }
    overflow_->insert(overflow_->begin() + index, location);
    ++size_;
}

void FiniteLocationSet::assign(std::vector<Location> locations)
{
    size_ = locations.size();
    if (locations.size() <= InlineCapacity)
    {
        overflow_.reset();
        std::copy(locations.begin(), locations.end(), inline_.begin());
        return;
    }
    overflow_ =
        std::make_unique<std::vector<Location>>(std::move(locations));
}

bool operator==(const FiniteLocationSet& lhs, const FiniteLocationSet& rhs)
{
    return lhs.size_ == rhs.size_ &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

AddressSet AddressSet::bottom()
{
    return AddressSet(false);
}

AddressSet AddressSet::top()
{
    return AddressSet(true);
}

AddressSet AddressSet::singleton(Location location)
{
    AddressSet result = bottom();
    result.insert(location);
    return result;
}

bool AddressSet::isBottom() const
{
    return !top_ && locations_.empty();
}

bool AddressSet::isTop() const
{
    return top_;
}

bool AddressSet::isSingleton() const
{
    return !top_ && locations_.size() == 1;
}

bool AddressSet::contains(Location location) const
{
    return top_ || locations_.contains(location);
}

bool AddressSet::hasIntersection(const AddressSet& other) const
{
    if (isBottom() || other.isBottom())
        return false;
    if (isTop() || other.isTop())
        return true;
    const auto* smaller = &locations_;
    const auto* larger = &other.locations_;
    if (larger->size() < smaller->size())
        std::swap(smaller, larger);
    return std::any_of(smaller->begin(), smaller->end(), [&](Location location) {
        return larger->contains(location);
    });
}

std::size_t AddressSet::size() const
{
    if (top_)
        throw std::logic_error("top address set has no finite size");
    return locations_.size();
}

bool AddressSet::empty() const
{
    return isBottom();
}

const FiniteLocationSet& AddressSet::locations() const
{
    if (top_)
        throw std::logic_error("top address set has no finite enumeration");
    return locations_;
}

void AddressSet::insert(Location location)
{
    if (top_)
        return;
    locations_.insert(location);
}

void AddressSet::joinWith(const AddressSet& other)
{
    if (top_ || other.isBottom())
        return;
    if (other.top_)
    {
        *this = top();
        return;
    }
    if (locations_.size() <= 2 && other.locations_.size() <= 2)
    {
        for (Location location : other.locations_)
            locations_.insert(location);
        return;
    }
    std::vector<Location> joined;
    joined.reserve(locations_.size() + other.locations_.size());
    std::set_union(locations_.begin(), locations_.end(),
                   other.locations_.begin(), other.locations_.end(),
                   std::back_inserter(joined));
    locations_.assign(std::move(joined));
}

void AddressSet::meetWith(const AddressSet& other)
{
    if (other.top_ || isBottom())
        return;
    if (top_)
    {
        *this = other;
        return;
    }
    if (locations_.size() <= 2 && other.locations_.size() <= 2)
    {
        FiniteLocationSet intersection;
        for (Location location : locations_)
        {
            if (other.locations_.contains(location))
                intersection.insert(location);
        }
        locations_ = std::move(intersection);
        return;
    }
    std::vector<Location> intersection;
    intersection.reserve(std::min(locations_.size(), other.locations_.size()));
    std::set_intersection(locations_.begin(), locations_.end(),
                          other.locations_.begin(), other.locations_.end(),
                          std::back_inserter(intersection));
    locations_.assign(std::move(intersection));
}

bool AddressSet::isSubsetOf(const AddressSet& other) const
{
    if (other.top_ || isBottom())
        return true;
    if (top_)
        return false;
    return std::includes(other.locations_.begin(), other.locations_.end(),
                         locations_.begin(), locations_.end());
}

std::string AddressSet::toString() const
{
    if (top_)
        return "top";
    if (locations_.empty())
        return "bottom";
    std::ostringstream output;
    output << "{";
    bool first = true;
    for (Location location : locations_)
    {
        if (!first)
            output << ",";
        first = false;
        output << location.id();
    }
    output << "}";
    return output.str();
}

AddressDomain AddressDomain::top()
{
    return AddressDomain(false);
}

AddressDomain AddressDomain::bottom()
{
    return AddressDomain(true);
}

std::unique_ptr<AbstractDomain> AddressDomain::clone() const
{
    return std::make_unique<AddressDomain>(*this);
}

AddressSet AddressDomain::addressSet(Variable variable) const
{
    if (bottom_)
        return AddressSet::bottom();
    const AddressSet* value = findValue(variable);
    return value ? *value : AddressSet::top();
}

std::vector<Variable> AddressDomain::nonDefaultVariables() const
{
    std::vector<Variable> variables;
    variables.reserve(size_);
    if (!paged_)
    {
        for (const auto& [variable, value] : *smallValues_)
        {
            (void)value;
            variables.push_back(variable);
        }
        return variables;
    }
    for (const ValuePageEntry& entry : pages_)
    {
        for (std::size_t slot = 0; slot < ValuesPerPage; ++slot)
        {
            if (entry.page->values[slot])
                variables.emplace_back(static_cast<std::uint32_t>(
                    entry.index * ValuesPerPage + slot));
        }
    }
    return variables;
}

std::vector<Variable> AddressDomain::nonDefaultVariablesBefore(
    Variable upperBound) const
{
    std::vector<Variable> variables;
    if (!paged_)
    {
        for (const auto& [variable, value] : *smallValues_)
        {
            (void)value;
            if (variable.id() >= upperBound.id())
                break;
            variables.push_back(variable);
        }
        return variables;
    }
    for (const ValuePageEntry& entry : pages_)
    {
        if (entry.index * ValuesPerPage >= upperBound.id())
            break;
        for (std::size_t slot = 0; slot < ValuesPerPage; ++slot)
        {
            if (!entry.page->values[slot])
                continue;
            const Variable variable(static_cast<std::uint32_t>(
                entry.index * ValuesPerPage + slot));
            if (variable.id() < upperBound.id())
                variables.push_back(variable);
        }
    }
    return variables;
}

void AddressDomain::assign(Variable variable, AddressSet addresses)
{
    if (bottom_)
        return;
    if (addresses.isTop())
        eraseValue(variable);
    else
        storeValue(variable, std::move(addresses));
}

void AddressDomain::forget(Variable variable)
{
    assign(variable, AddressSet::top());
}

bool AddressDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    return other.isDomain<AddressDomain>();
}

void AddressDomain::joinDomain(const AbstractDomain& other)
{
    const AddressDomain& address = requireAddress(other);
    if (address.bottom_)
        return;
    if (bottom_)
    {
        *this = address;
        return;
    }
    AddressDomain result = top();
    for (Variable variable : nonDefaultVariables())
    {
        AddressSet joined = addressSet(variable);
        joined.joinWith(address.addressSet(variable));
        if (!joined.isTop())
            result.assign(variable, std::move(joined));
    }
    *this = std::move(result);
}

void AddressDomain::meetDomain(const AbstractDomain& other)
{
    const AddressDomain& address = requireAddress(other);
    if (bottom_ || address.bottom_)
    {
        makeBottom();
        return;
    }
    AddressDomain result = top();
    std::vector<Variable> variables = nonDefaultVariables();
    const std::vector<Variable> otherVariables = address.nonDefaultVariables();
    variables.insert(variables.end(), otherVariables.begin(),
                     otherVariables.end());
    std::sort(variables.begin(), variables.end());
    variables.erase(std::unique(variables.begin(), variables.end()),
                    variables.end());
    for (Variable variable : variables)
    {
        AddressSet value = addressSet(variable);
        value.meetWith(address.addressSet(variable));
        if (!value.isTop())
            result.assign(variable, std::move(value));
    }
    *this = std::move(result);
}

void AddressDomain::widenDomain(const AbstractDomain& next)
{
    joinDomain(next);
}

void AddressDomain::narrowDomain(const AbstractDomain& next)
{
    meetDomain(next);
}

bool AddressDomain::isBottomDomain() const
{
    return bottom_;
}

bool AddressDomain::isTopDomain() const
{
    return !bottom_ && size_ == 0;
}

bool AddressDomain::leqDomain(const AbstractDomain& other) const
{
    const AddressDomain& address = requireAddress(other);
    if (bottom_)
        return true;
    if (address.bottom_)
        return false;
    for (Variable variable : address.nonDefaultVariables())
    {
        if (!addressSet(variable).isSubsetOf(address.addressSet(variable)))
            return false;
    }
    return true;
}

std::string AddressDomain::domainToString() const
{
    std::ostringstream output;
    if (bottom_)
        return "bottom";
    output << "{";
    bool first = true;
    for (Variable variable : nonDefaultVariables())
    {
        if (!first)
            output << ", ";
        first = false;
        output << variable.id() << "=" << addressSet(variable).toString();
    }
    output << "}";
    return output.str();
}

const AddressDomain& AddressDomain::requireAddress(
    const AbstractDomain& other) const
{
    requireCompatible(other);
    return static_cast<const AddressDomain&>(other);
}

std::shared_ptr<AddressDomain::SmallValues> AddressDomain::emptySmallValues()
{
    static const std::shared_ptr<SmallValues> empty =
        std::make_shared<SmallValues>();
    return empty;
}

const AddressSet* AddressDomain::findValue(Variable variable) const
{
    if (!paged_)
    {
        const auto iterator = std::lower_bound(
            smallValues_->begin(), smallValues_->end(), variable,
            [](const Value& value, Variable key) { return value.first < key; });
        return iterator != smallValues_->end() && iterator->first == variable
                   ? &iterator->second
                   : nullptr;
    }
    const std::size_t pageIndex = variable.id() / ValuesPerPage;
    const auto iterator = std::lower_bound(
        pages_.begin(), pages_.end(), pageIndex,
        [](const ValuePageEntry& entry, std::size_t index) {
            return entry.index < index;
        });
    if (iterator == pages_.end() || iterator->index != pageIndex)
        return nullptr;
    const std::optional<AddressSet>& value =
        iterator->page->values[variable.id() % ValuesPerPage];
    return value ? &*value : nullptr;
}

void AddressDomain::storeValue(Variable variable, AddressSet addresses)
{
    if (!paged_)
    {
        SmallValues& values = writableSmallValues();
        auto iterator = std::lower_bound(
            values.begin(), values.end(), variable,
            [](const Value& value, Variable key) { return value.first < key; });
        if (iterator != values.end() && iterator->first == variable)
        {
            iterator->second = std::move(addresses);
            return;
        }
        values.insert(iterator, Value(variable, std::move(addresses)));
        ++size_;
        if (size_ > SmallThreshold)
            promoteToPages();
        return;
    }
    std::optional<AddressSet>& slot =
        writablePage(variable.id() / ValuesPerPage)
            .values[variable.id() % ValuesPerPage];
    if (!slot)
        ++size_;
    slot = std::move(addresses);
}

void AddressDomain::eraseValue(Variable variable)
{
    if (!paged_)
    {
        SmallValues& values = writableSmallValues();
        const auto iterator = std::lower_bound(
            values.begin(), values.end(), variable,
            [](const Value& value, Variable key) { return value.first < key; });
        if (iterator != values.end() && iterator->first == variable)
        {
            values.erase(iterator);
            --size_;
        }
        return;
    }
    const std::size_t pageIndex = variable.id() / ValuesPerPage;
    auto iterator = std::lower_bound(
        pages_.begin(), pages_.end(), pageIndex,
        [](const ValuePageEntry& entry, std::size_t index) {
            return entry.index < index;
        });
    if (iterator == pages_.end() || iterator->index != pageIndex)
        return;
    if (iterator->page.use_count() != 1)
        iterator->page = std::make_shared<ValuePage>(*iterator->page);
    std::optional<AddressSet>& slot =
        iterator->page->values[variable.id() % ValuesPerPage];
    if (!slot)
        return;
    slot.reset();
    --size_;
    if (pageIsEmpty(*iterator->page))
        pages_.erase(iterator);
}

void AddressDomain::promoteToPages()
{
    if (paged_)
        return;
    const SmallValues values = *smallValues_;
    paged_ = true;
    size_ = 0;
    smallValues_ = emptySmallValues();
    for (const auto& [variable, addresses] : values)
        storeValue(variable, addresses);
}

AddressDomain::SmallValues& AddressDomain::writableSmallValues()
{
    if (smallValues_.use_count() != 1)
        smallValues_ = std::make_shared<SmallValues>(*smallValues_);
    return *smallValues_;
}

AddressDomain::ValuePage& AddressDomain::writablePage(std::size_t pageIndex)
{
    auto iterator = std::lower_bound(
        pages_.begin(), pages_.end(), pageIndex,
        [](const ValuePageEntry& entry, std::size_t index) {
            return entry.index < index;
        });
    if (iterator == pages_.end() || iterator->index != pageIndex)
        iterator = pages_.insert(
            iterator, {pageIndex, std::make_shared<ValuePage>()});
    else if (iterator->page.use_count() != 1)
        iterator->page = std::make_shared<ValuePage>(*iterator->page);
    return *iterator->page;
}

bool AddressDomain::pageIsEmpty(const ValuePage& page)
{
    return std::none_of(page.values.begin(), page.values.end(),
                        [](const std::optional<AddressSet>& value) {
                            return value.has_value();
                        });
}

void AddressDomain::makeBottom()
{
    bottom_ = true;
    paged_ = false;
    size_ = 0;
    smallValues_ = emptySmallValues();
    pages_.clear();
}

} // namespace SVF::AbstractDomain
