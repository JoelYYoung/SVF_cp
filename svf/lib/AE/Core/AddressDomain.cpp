//===- AddressDomain.cpp -- Address-set abstract property ----------------===//

#include "AE/Core/AddressDomain.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SVF::AbstractDomain
{
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
        for (const std::optional<Value>& value : entry.page->values)
        {
            if (value)
                variables.push_back(value->first);
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
    const std::optional<Value>& value =
        iterator->page->values[variable.id() % ValuesPerPage];
    return value && value->first == variable ? &value->second : nullptr;
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
    std::optional<Value>& slot =
        writablePage(variable.id() / ValuesPerPage)
            .values[variable.id() % ValuesPerPage];
    if (!slot)
        ++size_;
    slot = Value(variable, std::move(addresses));
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
    std::optional<Value>& slot =
        iterator->page->values[variable.id() % ValuesPerPage];
    if (!slot || slot->first != variable)
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
    smallValues_ = std::make_shared<SmallValues>();
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
                        [](const std::optional<Value>& value) {
                            return value.has_value();
                        });
}

void AddressDomain::makeBottom()
{
    bottom_ = true;
    paged_ = false;
    size_ = 0;
    smallValues_ = std::make_shared<SmallValues>();
    pages_.clear();
}

} // namespace SVF::AbstractDomain
