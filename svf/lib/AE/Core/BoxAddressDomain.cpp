//===- BoxAddressDomain.cpp -- Box/address reduced product -------------===//
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

#include "AE/Core/BoxAddressDomain.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

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

Lifetime joinLifetime(Lifetime lhs, Lifetime rhs)
{
    if (lhs == Lifetime::Bottom)
        return rhs;
    if (rhs == Lifetime::Bottom || lhs == rhs)
        return lhs;
    return Lifetime::MaybeFreed;
}

Lifetime meetLifetime(Lifetime lhs, Lifetime rhs)
{
    if (lhs == Lifetime::MaybeFreed)
        return rhs;
    if (rhs == Lifetime::MaybeFreed || lhs == rhs)
        return lhs;
    return Lifetime::Bottom;
}

bool lifetimeIsSubsetOf(Lifetime lhs, Lifetime rhs)
{
    return lhs == Lifetime::Bottom || rhs == Lifetime::MaybeFreed || lhs == rhs;
}

const char* lifetimeToString(Lifetime lifetime)
{
    switch (lifetime)
    {
    case Lifetime::Bottom:
        return "bottom";
    case Lifetime::Alive:
        return "alive";
    case Lifetime::Freed:
        return "freed";
    case Lifetime::MaybeFreed:
        return "maybe-freed";
    }
    return "invalid";
}

} // namespace

LifetimeDomain LifetimeDomain::top()
{
    return LifetimeDomain(Lifetime::MaybeFreed);
}

LifetimeDomain LifetimeDomain::bottom()
{
    return LifetimeDomain(Lifetime::Bottom);
}


std::unique_ptr<AbstractDomain> LifetimeDomain::clone() const
{
    return std::make_unique<LifetimeDomain>(*this);
}

Lifetime LifetimeDomain::statusOf(Location location) const
{
    const auto it = values_->find(location);
    return it == values_->end() ? defaultValue_ : it->second;
}

void LifetimeDomain::allocate(Location location)
{
    set(location, Lifetime::Alive);
}

void LifetimeDomain::release(Location location)
{
    const Lifetime current = statusOf(location);
    set(location, current == Lifetime::Alive || current == Lifetime::Freed
        ? Lifetime::Freed
        : Lifetime::MaybeFreed);
}

bool LifetimeDomain::mayBeFreed(Location location) const
{
    const Lifetime lifetime = statusOf(location);
    return lifetime == Lifetime::Freed || lifetime == Lifetime::MaybeFreed;
}

bool LifetimeDomain::mustBeFreed(Location location) const
{
    return statusOf(location) == Lifetime::Freed;
}

bool LifetimeDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    return other.isDomain<LifetimeDomain>();
}

void LifetimeDomain::joinDomain(const AbstractDomain& other)
{
    const auto& state = static_cast<const LifetimeDomain&>(other);
    if (state.isBottomDomain())
        return;
    if (isBottomDomain())
    {
        *this = state;
        return;
    }
    const std::set<Location> locations = combinedKeys(*values_, *state.values_);
    const Lifetime nextDefault =
        joinLifetime(defaultValue_, state.defaultValue_);
    std::map<Location, Lifetime> next;
    for (Location location : locations)
    {
        const Lifetime value =
            joinLifetime(statusOf(location), state.statusOf(location));
        if (value != nextDefault)
            next.emplace(location, value);
    }
    defaultValue_ = nextDefault;
    values_ = std::make_shared<Values>(std::move(next));
}

void LifetimeDomain::meetDomain(const AbstractDomain& other)
{
    const auto& state = static_cast<const LifetimeDomain&>(other);
    if (state.isTopDomain())
        return;
    if (isTopDomain())
    {
        *this = state;
        return;
    }
    const std::set<Location> locations = combinedKeys(*values_, *state.values_);
    const Lifetime nextDefault =
        meetLifetime(defaultValue_, state.defaultValue_);
    std::map<Location, Lifetime> next;
    for (Location location : locations)
    {
        const Lifetime value =
            meetLifetime(statusOf(location), state.statusOf(location));
        if (value != nextDefault)
            next.emplace(location, value);
    }
    defaultValue_ = nextDefault;
    values_ = std::make_shared<Values>(std::move(next));
}

void LifetimeDomain::widenDomain(const AbstractDomain& next)
{
    joinDomain(next);
}

void LifetimeDomain::narrowDomain(const AbstractDomain& next)
{
    meetDomain(next);
}

bool LifetimeDomain::isBottomDomain() const
{
    return defaultValue_ == Lifetime::Bottom && values_->empty();
}

bool LifetimeDomain::isTopDomain() const
{
    return defaultValue_ == Lifetime::MaybeFreed && values_->empty();
}

bool LifetimeDomain::leqDomain(const AbstractDomain& other) const
{
    const auto& state = static_cast<const LifetimeDomain&>(other);
    if (defaultValue_ == state.defaultValue_ &&
            (values_ == state.values_ || *values_ == *state.values_))
        return true;
    if (!lifetimeIsSubsetOf(defaultValue_, state.defaultValue_))
        return false;
    const std::set<Location> locations = combinedKeys(*values_, *state.values_);
    return std::all_of(locations.begin(), locations.end(),
                       [&](Location location)
    {
        return lifetimeIsSubsetOf(
                   statusOf(location), state.statusOf(location));
    });
}

std::string LifetimeDomain::domainToString() const
{
    std::ostringstream output;
    output << "default=" << lifetimeToString(defaultValue_) << " {";
    bool first = true;
    for (const auto& [location, value] : *values_)
    {
        if (!first)
            output << ", ";
        first = false;
        output << location.id() << "=" << lifetimeToString(value);
    }
    output << "}";
    return output.str();
}

void LifetimeDomain::set(Location location, Lifetime lifetime)
{
    if (lifetime == defaultValue_)
        writableValues().erase(location);
    else
        writableValues()[location] = lifetime;
}

LifetimeDomain::Values& LifetimeDomain::writableValues()
{
    if (values_.use_count() != 1)
        values_ = std::make_shared<Values>(*values_);
    return *values_;
}

Variable MemoryLayout::contentOf(Location location) const
{
    const auto it = cells_->find(location);
    if (it == cells_->end())
        throw std::out_of_range("location has no content symbol");
    return it->second;
}

void MemoryLayout::extend(Location location, Variable content)
{
    const auto [iterator, inserted] = cells_->emplace(location, content);
    if (!inserted && iterator->second != content)
        throw std::invalid_argument(
            "location already has a different content symbol");
}

} // namespace SVF::AbstractDomain
