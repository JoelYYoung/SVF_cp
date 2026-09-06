//===- AddressDomain.h -- Address-set abstract property --------*- C++ -*-===//

#ifndef SVF_AE_ADDRESS_DOMAIN_H
#define SVF_AE_ADDRESS_DOMAIN_H

#include "AE/Core/AbstractDomain.h"
#include "AE/Core/Variable.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SVF::AbstractDomain
{

class Location
{
public:
    explicit Location(std::uint32_t id = 0) : id_(id) {}

    static Location null()
    {
        return Location();
    }

    std::uint32_t id() const
    {
        return id_;
    }
    bool isNull() const
    {
        return id_ == 0;
    }

    friend bool operator==(Location lhs, Location rhs)
    {
        return lhs.id_ == rhs.id_;
    }
    friend bool operator!=(Location lhs, Location rhs)
    {
        return !(lhs == rhs);
    }
    friend bool operator<(Location lhs, Location rhs)
    {
        return lhs.id_ < rhs.id_;
    }

private:
    std::uint32_t id_;
};

/// Sorted finite location storage with two inline elements. Most AE
/// points-to facts are singletons, so they require no auxiliary allocation.
class FiniteLocationSet
{
public:
    using const_iterator = const Location*;

    FiniteLocationSet() = default;
    FiniteLocationSet(const FiniteLocationSet& other);
    FiniteLocationSet(FiniteLocationSet&& other) noexcept = default;
    FiniteLocationSet& operator=(const FiniteLocationSet& other);
    FiniteLocationSet& operator=(FiniteLocationSet&& other) noexcept = default;

    std::size_t size() const;
    bool empty() const;
    const_iterator begin() const;
    const_iterator end() const;
    void insert(Location location);
    void assign(std::vector<Location> locations);

    friend bool operator==(const FiniteLocationSet& lhs,
                           const FiniteLocationSet& rhs);

private:
    static constexpr std::size_t InlineCapacity = 2;
    std::size_t size_ = 0;
    std::array<Location, InlineCapacity> inline_{Location(), Location()};
    std::unique_ptr<std::vector<Location>> overflow_;
};

/// Finite points-to set for one pointer variable, with an explicit top value.
class AddressSet
{
public:
    using const_iterator = FiniteLocationSet::const_iterator;

    AddressSet() = default;

    static AddressSet bottom();
    static AddressSet top();
    static AddressSet singleton(Location location);

    bool isBottom() const;
    bool isTop() const;
    bool isSingleton() const;
    bool contains(Location location) const;
    bool hasIntersection(const AddressSet& other) const;
    std::size_t size() const;
    bool empty() const;
    const FiniteLocationSet& locations() const;
    const_iterator begin() const
    {
        return locations().begin();
    }
    const_iterator end() const
    {
        return locations().end();
    }

    void insert(Location location);
    void joinWith(const AddressSet& other);
    void meetWith(const AddressSet& other);
    bool isSubsetOf(const AddressSet& other) const;
    std::string toString() const;

    friend bool operator==(const AddressSet& lhs, const AddressSet& rhs)
    {
        return lhs.top_ == rhs.top_ && lhs.locations_ == rhs.locations_;
    }
    friend bool operator!=(const AddressSet& lhs, const AddressSet& rhs)
    {
        return !(lhs == rhs);
    }

private:
    explicit AddressSet(bool top) : top_(top) {}

    bool top_ = false;
    FiniteLocationSet locations_;
};

/// Flow-sensitive address property with finite non-Top support over stable
/// Variables. Missing entries in every non-Bottom property denote Address Top.
/// An explicit empty AddressSet is a per-variable fact; it is distinct from
/// whole-property Bottom, which denotes an unreachable address carrier.
class AddressDomain final : public AbstractDomain
{
public:
    static AddressDomain top();
    static AddressDomain bottom();

    DomainKind kind() const noexcept override
    {
        return DomainKind::Address;
    }
    std::unique_ptr<AbstractDomain> clone() const override;

    AddressSet addressSet(Variable variable) const;
    /// Variables with a value different from Address Top.
    std::vector<Variable> nonDefaultVariables() const;
    void assign(Variable variable, AddressSet addresses);
    void forget(Variable variable);

private:
#ifndef SVF_AE_ADDRESS_VALUES_PER_PAGE
#define SVF_AE_ADDRESS_VALUES_PER_PAGE 16
#endif
#ifndef SVF_AE_ADDRESS_SMALL_THRESHOLD
#define SVF_AE_ADDRESS_SMALL_THRESHOLD 16
#endif
    static constexpr std::size_t ValuesPerPage =
        SVF_AE_ADDRESS_VALUES_PER_PAGE;
    static constexpr std::size_t SmallThreshold =
        SVF_AE_ADDRESS_SMALL_THRESHOLD;

    using Value = std::pair<Variable, AddressSet>;
    using SmallValues = std::vector<Value>;

    struct ValuePage
    {
        std::array<std::optional<AddressSet>, ValuesPerPage> values;
    };

    struct ValuePageEntry
    {
        std::size_t index;
        std::shared_ptr<ValuePage> page;
    };

    using ValuePageDirectory = std::vector<ValuePageEntry>;

    explicit AddressDomain(bool bottom)
        : bottom_(bottom), smallValues_(emptySmallValues())
    {
    }

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<AddressDomain>();
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

    const AddressDomain& requireAddress(const AbstractDomain& other) const;
    static std::shared_ptr<SmallValues> emptySmallValues();
    const AddressSet* findValue(Variable variable) const;
    void storeValue(Variable variable, AddressSet addresses);
    void eraseValue(Variable variable);
    void promoteToPages();
    SmallValues& writableSmallValues();
    ValuePage& writablePage(std::size_t pageIndex);
    static bool pageIsEmpty(const ValuePage& page);
    void makeBottom();

    bool bottom_ = false;
    bool paged_ = false;
    std::size_t size_ = 0;
    std::shared_ptr<SmallValues> smallValues_;
    ValuePageDirectory pages_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_ADDRESS_DOMAIN_H
