//===- AddressDomain.cpp -- Address-set abstract property ----------------===//

#include "AE/Core/AddressDomain.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SVF::AbstractDomain
{
namespace
{
template <typename Key, typename Value>
std::set<Key> combinedKeys(const std::map<Key, Value>& lhs,
                           const std::map<Key, Value>& rhs)
{
    std::set<Key> keys;
    for (const auto& entry : lhs)
        keys.insert(entry.first);
    for (const auto& entry : rhs)
        keys.insert(entry.first);
    return keys;
}
} // namespace

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
    return top_ || std::binary_search(locations_.begin(), locations_.end(),
                                      location);
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
        return std::binary_search(larger->begin(), larger->end(), location);
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

const std::vector<Location>& AddressSet::locations() const
{
    if (top_)
        throw std::logic_error("top address set has no finite enumeration");
    return locations_;
}

void AddressSet::insert(Location location)
{
    if (top_)
        return;
    const auto position =
        std::lower_bound(locations_.begin(), locations_.end(), location);
    if (position == locations_.end() || *position != location)
        locations_.insert(position, location);
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
    std::vector<Location> joined;
    joined.reserve(locations_.size() + other.locations_.size());
    std::set_union(locations_.begin(), locations_.end(),
                   other.locations_.begin(), other.locations_.end(),
                   std::back_inserter(joined));
    locations_ = std::move(joined);
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
    std::vector<Location> intersection;
    intersection.reserve(std::min(locations_.size(), other.locations_.size()));
    std::set_intersection(locations_.begin(), locations_.end(),
                          other.locations_.begin(), other.locations_.end(),
                          std::back_inserter(intersection));
    locations_ = std::move(intersection);
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
    const auto iterator = values_->find(variable);
    return iterator == values_->end() ? AddressSet::top() : iterator->second;
}

std::vector<Variable> AddressDomain::nonDefaultVariables() const
{
    std::vector<Variable> variables;
    variables.reserve(values_->size());
    for (const auto& [variable, value] : *values_)
    {
        (void)value;
        variables.push_back(variable);
    }
    return variables;
}

void AddressDomain::assign(Variable variable, AddressSet addresses)
{
    if (bottom_)
        return;
    writableValues()[variable] = std::move(addresses);
    normalize(variable);
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
    Values next;
    for (const auto& [variable, value] : *values_)
    {
        const auto otherValue = address.values_->find(variable);
        if (otherValue == address.values_->end())
            continue;
        AddressSet joined = value;
        joined.joinWith(otherValue->second);
        if (!joined.isTop())
            next.emplace(variable, std::move(joined));
    }
    values_ = std::make_shared<Values>(std::move(next));
}

void AddressDomain::meetDomain(const AbstractDomain& other)
{
    const AddressDomain& address = requireAddress(other);
    if (bottom_ || address.bottom_)
    {
        makeBottom();
        return;
    }
    const std::set<Variable> variables =
        combinedKeys(*values_, *address.values_);
    Values next;
    for (Variable variable : variables)
    {
        AddressSet value = addressSet(variable);
        value.meetWith(address.addressSet(variable));
        if (!value.isTop())
            next.emplace(variable, std::move(value));
    }
    values_ = std::make_shared<Values>(std::move(next));
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
    return !bottom_ && values_->empty();
}

bool AddressDomain::leqDomain(const AbstractDomain& other) const
{
    const AddressDomain& address = requireAddress(other);
    if (bottom_)
        return true;
    if (address.bottom_)
        return false;
    if (values_ == address.values_ || *values_ == *address.values_)
        return true;
    return std::all_of(
        address.values_->begin(), address.values_->end(),
        [&](const auto& entry) {
            return addressSet(entry.first).isSubsetOf(entry.second);
        });
}

std::string AddressDomain::domainToString() const
{
    std::ostringstream output;
    if (bottom_)
        return "bottom";
    output << "{";
    bool first = true;
    for (const auto& [variable, value] : *values_)
    {
        if (!first)
            output << ", ";
        first = false;
        output << variable.id() << "=" << value.toString();
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

void AddressDomain::normalize(Variable variable)
{
    const auto iterator = values_->find(variable);
    if (iterator != values_->end() && iterator->second.isTop())
        writableValues().erase(variable);
}

AddressDomain::Values& AddressDomain::writableValues()
{
    if (values_.use_count() != 1)
        values_ = std::make_shared<Values>(*values_);
    return *values_;
}

void AddressDomain::makeBottom()
{
    bottom_ = true;
    values_ = std::make_shared<Values>();
}

} // namespace SVF::AbstractDomain
