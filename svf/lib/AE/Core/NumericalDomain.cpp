//===- NumericalDomain.cpp -- Numerical properties and Box domain -------===//

#include "AE/Core/NumericalDomain.h"
#include "AE/Core/LinearExpression.h"
#include "AE/Core/TreeExpression.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace SVF::AbstractDomain
{

Integer::Integer() : value_(0) {}

Integer::Integer(std::int64_t value)
{
    if (mpz_set_str(value_.get_mpz_t(), std::to_string(value).c_str(), 10) != 0)
        throw std::invalid_argument("invalid 64-bit integer");
}

Integer::Integer(const std::string& value) : value_(value) {}

std::string Integer::toString() const
{
    return value_.get_str();
}

Rational::Rational() : value_(0) {}

Rational::Rational(std::int64_t value)
{
    mpz_class integer;
    if (mpz_set_str(integer.get_mpz_t(), std::to_string(value).c_str(), 10) !=
        0)
        throw std::invalid_argument("invalid 64-bit rational integer");
    mpq_set_z(value_.get_mpq_t(), integer.get_mpz_t());
}

Rational::Rational(const Integer& value) : value_(value.value()) {}

Rational::Rational(const std::string& value) : value_(value)
{
    value_.canonicalize();
}

Rational::Rational(const Integer& numerator, const Integer& denominator)
{
    if (denominator.value() == 0)
        throw std::invalid_argument("a rational denominator cannot be zero");
    mpq_set_num(value_.get_mpq_t(), numerator.value().get_mpz_t());
    mpq_set_den(value_.get_mpq_t(), denominator.value().get_mpz_t());
    value_.canonicalize();
}

Rational::Rational(mpq_class value, int) : value_(std::move(value))
{
    value_.canonicalize();
}

Rational Rational::fromRaw(const mpq_class& value)
{
    return Rational(value, 0);
}

Rational Rational::fromDouble(double value)
{
    if (!std::isfinite(value))
        throw std::invalid_argument(
            "a non-finite floating value is not rational");
    mpq_class rational;
    mpq_set_d(rational.get_mpq_t(), value);
    rational.canonicalize();
    return fromRaw(rational);
}

std::string Rational::toString() const
{
    return value_.get_str();
}

bool Rational::isInteger() const
{
    return mpz_cmp_ui(mpq_denref(value_.get_mpq_t()), 1U) == 0;
}

std::int64_t Rational::toInt64() const
{
    if (!isInteger())
        throw std::domain_error("rational is not an integer");
    const std::string encoded =
        mpz_class(mpq_numref(value_.get_mpq_t())).get_str();
    std::size_t consumed = 0;
    try
    {
        const long long result = std::stoll(encoded, &consumed, 10);
        if (consumed != encoded.size())
            throw std::out_of_range("invalid integer encoding");
        return static_cast<std::int64_t>(result);
    }
    catch (const std::exception&)
    {
        throw std::overflow_error("rational integer does not fit int64");
    }
}

double Rational::toDouble() const
{
    return mpq_get_d(value_.get_mpq_t());
}

Rational Rational::floor() const
{
    mpz_class result;
    mpz_fdiv_q(result.get_mpz_t(), mpq_numref(value_.get_mpq_t()),
               mpq_denref(value_.get_mpq_t()));
    return Rational::fromRaw(mpq_class(result));
}

Rational Rational::ceil() const
{
    mpz_class result;
    mpz_cdiv_q(result.get_mpz_t(), mpq_numref(value_.get_mpq_t()),
               mpq_denref(value_.get_mpq_t()));
    return Rational::fromRaw(mpq_class(result));
}

Rational Rational::dividedByPowerOfTwo(unsigned exponent) const
{
    Rational result;
    mpq_div_2exp(result.value_.get_mpq_t(), value_.get_mpq_t(), exponent);
    return result;
}

Rational& Rational::assignSum(const Rational& lhs, const Rational& rhs)
{
    mpq_add(value_.get_mpq_t(), lhs.value_.get_mpq_t(), rhs.value_.get_mpq_t());
    return *this;
}

Rational& Rational::divideByPowerOfTwoInPlace(unsigned exponent)
{
    mpq_div_2exp(value_.get_mpq_t(), value_.get_mpq_t(), exponent);
    return *this;
}

Rational& Rational::operator+=(const Rational& rhs)
{
    value_ += rhs.value_;
    return *this;
}

Rational& Rational::operator-=(const Rational& rhs)
{
    value_ -= rhs.value_;
    return *this;
}

Rational& Rational::operator*=(const Rational& rhs)
{
    value_ *= rhs.value_;
    return *this;
}

Rational& Rational::operator/=(const Rational& rhs)
{
    if (rhs.isZero())
        throw std::domain_error("division by zero rational");
    value_ /= rhs.value_;
    return *this;
}

Bound::Bound() = default;

Bound::Bound(Kind kind, Rational value, bool strict)
    : kind_(kind), value_(std::move(value)),
      strict_(kind == Kind::Finite && strict)
{
}

Bound Bound::minusInfinity()
{
    return Bound(Kind::MinusInfinity, Rational(), false);
}

Bound Bound::finite(Rational value, bool strict)
{
    return Bound(Kind::Finite, std::move(value), strict);
}

Bound Bound::plusInfinity()
{
    return Bound(Kind::PlusInfinity, Rational(), false);
}

const Rational& Bound::value() const
{
    if (!isFinite())
        throw std::logic_error("an infinite bound has no finite value");
    return value_;
}

int Bound::compare(const Bound& lhs, const Bound& rhs)
{
    if (lhs.kind_ != rhs.kind_)
        return static_cast<int>(lhs.kind_) < static_cast<int>(rhs.kind_) ? -1
                                                                         : 1;
    if (!lhs.isFinite())
        return 0;
    if (lhs.value_ < rhs.value_)
        return -1;
    if (rhs.value_ < lhs.value_)
        return 1;
    if (lhs.strict_ == rhs.strict_)
        return 0;
    return lhs.strict_ ? -1 : 1;
}

Bound Bound::min(const Bound& lhs, const Bound& rhs)
{
    return lhs <= rhs ? lhs : rhs;
}

Bound Bound::max(const Bound& lhs, const Bound& rhs)
{
    return lhs <= rhs ? rhs : lhs;
}

Bound Bound::add(const Bound& lhs, const Bound& rhs)
{
    if ((lhs.isMinusInfinity() && rhs.isPlusInfinity()) ||
        (lhs.isPlusInfinity() && rhs.isMinusInfinity()))
        throw std::domain_error("indeterminate sum of opposite infinities");
    if (lhs.isMinusInfinity() || rhs.isMinusInfinity())
        return minusInfinity();
    if (lhs.isPlusInfinity() || rhs.isPlusInfinity())
        return plusInfinity();
    return finite(lhs.value_ + rhs.value_, lhs.strict_ || rhs.strict_);
}

Bound& Bound::assignSum(const Bound& lhs, const Bound& rhs)
{
    if ((lhs.isMinusInfinity() && rhs.isPlusInfinity()) ||
        (lhs.isPlusInfinity() && rhs.isMinusInfinity()))
        throw std::domain_error("indeterminate sum of opposite infinities");
    if (lhs.isMinusInfinity() || rhs.isMinusInfinity())
    {
        kind_ = Kind::MinusInfinity;
        strict_ = false;
        return *this;
    }
    if (lhs.isPlusInfinity() || rhs.isPlusInfinity())
    {
        kind_ = Kind::PlusInfinity;
        strict_ = false;
        return *this;
    }
    kind_ = Kind::Finite;
    value_.assignSum(lhs.value_, rhs.value_);
    strict_ = lhs.strict_ || rhs.strict_;
    return *this;
}

Bound& Bound::divideByTwoInPlace()
{
    if (isFinite())
        value_.divideByPowerOfTwoInPlace(1);
    return *this;
}

Bound Bound::divideByTwo(const Bound& bound)
{
    if (!bound.isFinite())
        return bound;
    return finite(bound.value_.dividedByPowerOfTwo(1), bound.strict_);
}

Bound Bound::divideByPositive(const Bound& bound, const Rational& divisor)
{
    if (divisor.sign() <= 0)
        throw std::invalid_argument("bound divisor must be positive");
    if (!bound.isFinite())
        return bound;
    return finite(bound.value_ / divisor, bound.strict_);
}

std::string Bound::toString() const
{
    if (isMinusInfinity())
        return "-inf";
    if (isPlusInfinity())
        return "+inf";
    return std::string(strict_ ? "<" : "<=") + value_.toString();
}

namespace
{
int compareIntervalLower(const Bound& lhs, const Bound& rhs)
{
    if (lhs.kind() != rhs.kind())
        return static_cast<int>(lhs.kind()) < static_cast<int>(rhs.kind()) ? -1
                                                                           : 1;
    if (!lhs.isFinite())
        return 0;
    if (lhs.value() < rhs.value())
        return -1;
    if (rhs.value() < lhs.value())
        return 1;
    if (lhs.isStrict() == rhs.isStrict())
        return 0;
    return lhs.isStrict() ? 1 : -1;
}

Bound minimumLower(const Bound& lhs, const Bound& rhs)
{
    return compareIntervalLower(lhs, rhs) <= 0 ? lhs : rhs;
}

Bound maximumLower(const Bound& lhs, const Bound& rhs)
{
    return compareIntervalLower(lhs, rhs) >= 0 ? lhs : rhs;
}
} // namespace

Interval::Interval()
    : lower_(Bound::minusInfinity()), upper_(Bound::plusInfinity())
{
}

Interval::Interval(Bound lower, Bound upper)
    : lower_(std::move(lower)), upper_(std::move(upper))
{
}

Interval Interval::top()
{
    return Interval();
}

Interval Interval::bottom()
{
    return Interval(Bound::plusInfinity(), Bound::minusInfinity());
}

Interval Interval::singleton(const Rational& value)
{
    return Interval(Bound::finite(value), Bound::finite(value));
}

Interval Interval::closed(const Rational& lower, const Rational& upper)
{
    return upper < lower ? bottom()
                         : Interval(Bound::finite(lower), Bound::finite(upper));
}

bool Interval::isTop() const
{
    return lower_.isMinusInfinity() && upper_.isPlusInfinity();
}

bool Interval::isBottom() const
{
    if (lower_.isPlusInfinity() || upper_.isMinusInfinity())
        return true;
    if (!lower_.isFinite() || !upper_.isFinite())
        return false;
    if (upper_.value() < lower_.value())
        return true;
    return upper_.value() == lower_.value() &&
           (lower_.isStrict() || upper_.isStrict());
}

bool Interval::isSingleton() const
{
    return !isBottom() && lower_.isFinite() && upper_.isFinite() &&
           !lower_.isStrict() && !upper_.isStrict() &&
           lower_.value() == upper_.value();
}

bool Interval::isZero() const
{
    return isSingleton() && lower_.value().isZero();
}

bool Interval::contains(const Rational& value) const
{
    if (isBottom())
        return false;
    const bool aboveLower = lower_.isMinusInfinity() ||
                            (lower_.isFinite() &&
                             (lower_.value() < value ||
                              (lower_.value() == value && !lower_.isStrict())));
    const bool belowUpper = upper_.isPlusInfinity() ||
                            (upper_.isFinite() &&
                             (value < upper_.value() ||
                              (value == upper_.value() && !upper_.isStrict())));
    return aboveLower && belowUpper;
}

bool Interval::isSubsetOf(const Interval& other) const
{
    if (isBottom())
        return true;
    if (other.isBottom())
        return false;
    return compareIntervalLower(lower_, other.lower_) >= 0 &&
           Bound::compare(upper_, other.upper_) <= 0;
}

const Rational& Interval::singletonValue() const
{
    if (!isSingleton())
        throw std::domain_error("interval is not a singleton");
    return lower_.value();
}

void Interval::joinWith(const Interval& other)
{
    if (other.isBottom())
        return;
    if (isBottom())
    {
        *this = other;
        return;
    }
    lower_ = minimumLower(lower_, other.lower_);
    upper_ = Bound::max(upper_, other.upper_);
}

void Interval::meetWith(const Interval& other)
{
    if (isBottom() || other.isBottom())
    {
        *this = bottom();
        return;
    }
    lower_ = maximumLower(lower_, other.lower_);
    upper_ = Bound::min(upper_, other.upper_);
    if (isBottom())
        *this = bottom();
}

void Interval::widenWith(const Interval& next)
{
    if (isBottom())
    {
        *this = next;
        return;
    }
    if (next.isBottom())
        return;
    if (compareIntervalLower(next.lower_, lower_) < 0)
        lower_ = Bound::minusInfinity();
    if (upper_ < next.upper_)
        upper_ = Bound::plusInfinity();
}

void Interval::narrowWith(const Interval& next)
{
    if (isBottom() || next.isBottom())
    {
        *this = bottom();
        return;
    }
    if (lower_.isMinusInfinity())
        lower_ = next.lower_;
    if (upper_.isPlusInfinity())
        upper_ = next.upper_;
    meetWith(next);
}

std::string Interval::toString() const
{
    const char left = lower_.isStrict() ? '(' : '[';
    const char right = upper_.isStrict() ? ')' : ']';
    const std::string lower = lower_.isMinusInfinity() ? "-inf"
                              : lower_.isPlusInfinity()
                                  ? "+inf"
                                  : lower_.value().toString();
    const std::string upper = upper_.isPlusInfinity() ? "+inf"
                              : upper_.isMinusInfinity()
                                  ? "-inf"
                                  : upper_.value().toString();
    return std::string(1, left) + lower + ", " + upper + std::string(1, right);
}

MpfrValue::MpfrValue(mpfr_prec_t precision)
{
    if (precision < MPFR_PREC_MIN || precision > MPFR_PREC_MAX)
        throw std::invalid_argument("invalid MPFR precision");
    mpfr_init2(value_, precision);
    mpfr_set_zero(value_, 1);
}

MpfrValue::MpfrValue(const MpfrValue& rhs)
{
    mpfr_init2(value_, rhs.precision());
    mpfr_set(value_, rhs.value_, MPFR_RNDN);
}

MpfrValue::MpfrValue(MpfrValue&& rhs) noexcept
{
    mpfr_init2(value_, rhs.precision());
    mpfr_swap(value_, rhs.value_);
}

MpfrValue& MpfrValue::operator=(const MpfrValue& rhs)
{
    if (this == &rhs)
        return *this;
    mpfr_set_prec(value_, rhs.precision());
    mpfr_set(value_, rhs.value_, MPFR_RNDN);
    return *this;
}

MpfrValue& MpfrValue::operator=(MpfrValue&& rhs) noexcept
{
    if (this != &rhs)
        mpfr_swap(value_, rhs.value_);
    return *this;
}

MpfrValue::~MpfrValue()
{
    mpfr_clear(value_);
}

void MpfrValue::set(const Rational& value, mpfr_rnd_t rounding)
{
    mpfr_set_q(value_, value.value().get_mpq_t(), rounding);
}

Rational MpfrValue::toRational() const
{
    if (!mpfr_number_p(value_))
        throw std::domain_error("a non-finite MPFR value is not rational");
    mpq_class result;
    mpfr_get_q(result.get_mpq_t(), value_);
    result.canonicalize();
    return Rational::fromRaw(result);
}

namespace
{
mpfr_rnd_t toMpfrRounding(RoundingMode mode)
{
    switch (mode)
    {
    case RoundingMode::NearestTiesToEven:
        return MPFR_RNDN;
    case RoundingMode::TowardZero:
        return MPFR_RNDZ;
    case RoundingMode::TowardPositive:
        return MPFR_RNDU;
    case RoundingMode::TowardNegative:
        return MPFR_RNDD;
    }
    return MPFR_RNDN;
}
} // namespace

Rational FloatSemantics::evaluate(BinaryOperation operation,
                                  const Rational& lhs, const Rational& rhs,
                                  unsigned significandBits,
                                  RoundingMode rounding)
{
    if (significandBits < static_cast<unsigned>(MPFR_PREC_MIN))
        throw std::invalid_argument("invalid floating significand precision");

    const mpfr_rnd_t mode = toMpfrRounding(rounding);
    MpfrValue left(significandBits);
    MpfrValue right(significandBits);
    MpfrValue result(significandBits);
    left.set(lhs, mode);
    right.set(rhs, mode);

    switch (operation)
    {
    case BinaryOperation::Add:
        mpfr_add(result.raw(), left.raw(), right.raw(), mode);
        break;
    case BinaryOperation::Subtract:
        mpfr_sub(result.raw(), left.raw(), right.raw(), mode);
        break;
    case BinaryOperation::Multiply:
        mpfr_mul(result.raw(), left.raw(), right.raw(), mode);
        break;
    case BinaryOperation::Divide:
        mpfr_div(result.raw(), left.raw(), right.raw(), mode);
        break;
    }
    return result.toRational();
}

Rational FloatSemantics::add(const Rational& lhs, const Rational& rhs,
                             unsigned significandBits, RoundingMode rounding)
{
    return evaluate(BinaryOperation::Add, lhs, rhs, significandBits, rounding);
}

Rational FloatSemantics::subtract(const Rational& lhs, const Rational& rhs,
                                  unsigned significandBits,
                                  RoundingMode rounding)
{
    return evaluate(BinaryOperation::Subtract, lhs, rhs, significandBits,
                    rounding);
}

Rational FloatSemantics::multiply(const Rational& lhs, const Rational& rhs,
                                  unsigned significandBits,
                                  RoundingMode rounding)
{
    return evaluate(BinaryOperation::Multiply, lhs, rhs, significandBits,
                    rounding);
}

Rational FloatSemantics::divide(const Rational& lhs, const Rational& rhs,
                                unsigned significandBits, RoundingMode rounding)
{
    return evaluate(BinaryOperation::Divide, lhs, rhs, significandBits,
                    rounding);
}

namespace
{

constexpr std::array<std::uint8_t, 8> RawMagic{'S', 'V', 'F', 'A',
                                               'D', 'R', 'A', 'W'};
constexpr std::uint16_t RawVersion = 2;
constexpr std::uint32_t MaxCollectionEntries = 1U << 20;
constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

enum class DomainTag : std::uint8_t
{
    Box = 1
};

std::uint64_t fnv1a(const std::uint8_t* data, std::size_t size)
{
    std::uint64_t result = FnvOffset;
    for (std::size_t index = 0; index < size; ++index)
    {
        result ^= data[index];
        result *= FnvPrime;
    }
    return result;
}

class Writer
{
public:
    void writeByte(std::uint8_t value)
    {
        bytes_.push_back(value);
    }

    void writeU16(std::uint16_t value)
    {
        for (unsigned shift = 0; shift < 16; shift += 8)
            writeByte(static_cast<std::uint8_t>(value >> shift));
    }

    void writeU32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
            writeByte(static_cast<std::uint8_t>(value >> shift));
    }

    void writeU64(std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
            writeByte(static_cast<std::uint8_t>(value >> shift));
    }

    void writeString(const std::string& value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("raw state string is too large");
        writeU32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void writeMagic()
    {
        bytes_.insert(bytes_.end(), RawMagic.begin(), RawMagic.end());
    }

    NumericalDomain::RawBuffer finish()
    {
        const std::uint64_t checksum = fnv1a(bytes_.data(), bytes_.size());
        writeU64(checksum);
        return std::move(bytes_);
    }

private:
    NumericalDomain::RawBuffer bytes_;
};

std::uint64_t readTrailingU64(const NumericalDomain::RawBuffer& buffer)
{
    if (buffer.size() < sizeof(std::uint64_t))
        throw std::invalid_argument("raw state buffer is truncated");
    std::uint64_t value = 0;
    const std::size_t offset = buffer.size() - sizeof(std::uint64_t);
    for (unsigned index = 0; index < sizeof(std::uint64_t); ++index)
        value |= static_cast<std::uint64_t>(buffer[offset + index])
                 << (8 * index);
    return value;
}

class Reader
{
public:
    explicit Reader(const NumericalDomain::RawBuffer& bytes)
        : bytes_(bytes), limit_(checkedLimit(bytes))
    {
        const std::uint64_t expected = readTrailingU64(bytes_);
        const std::uint64_t actual = fnv1a(bytes_.data(), limit_);
        if (actual != expected)
            throw std::invalid_argument("raw state checksum mismatch");
    }

    void readMagic()
    {
        for (std::uint8_t expected : RawMagic)
        {
            if (readByte() != expected)
                throw std::invalid_argument("raw state has invalid magic");
        }
    }

    std::uint8_t readByte()
    {
        require(1);
        return bytes_[position_++];
    }

    std::uint16_t readU16()
    {
        std::uint16_t value = 0;
        for (unsigned index = 0; index < sizeof(value); ++index)
            value |= static_cast<std::uint16_t>(readByte()) << (8 * index);
        return value;
    }

    std::uint32_t readU32()
    {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < sizeof(value); ++index)
            value |= static_cast<std::uint32_t>(readByte()) << (8 * index);
        return value;
    }

    std::string readString()
    {
        const std::uint32_t size = readU32();
        require(size);
        const auto begin =
            bytes_.begin() + static_cast<std::ptrdiff_t>(position_);
        position_ += size;
        return std::string(begin, begin + size);
    }

    bool empty() const
    {
        return position_ == limit_;
    }

private:
    static std::size_t checkedLimit(const NumericalDomain::RawBuffer& bytes)
    {
        if (bytes.size() <
            RawMagic.size() + sizeof(std::uint16_t) + 2 + sizeof(std::uint64_t))
            throw std::invalid_argument("raw state buffer is truncated");
        return bytes.size() - sizeof(std::uint64_t);
    }

    void require(std::size_t size) const
    {
        if (size > limit_ - position_)
            throw std::invalid_argument("raw state buffer is truncated");
    }

    const NumericalDomain::RawBuffer& bytes_;
    std::size_t limit_;
    std::size_t position_ = 0;
};

std::uint8_t encodeKind(NumericKind kind)
{
    switch (kind)
    {
    case NumericKind::Integer:
        return 0;
    case NumericKind::Real:
        return 1;
    case NumericKind::IEEEFloat:
        return 2;
    }
    throw std::logic_error("unknown numerical kind");
}

NumericKind decodeKind(std::uint8_t value)
{
    switch (value)
    {
    case 0:
        return NumericKind::Integer;
    case 1:
        return NumericKind::Real;
    case 2:
        return NumericKind::IEEEFloat;
    default:
        throw std::invalid_argument("raw state has invalid numerical kind");
    }
}

std::uint8_t encodeConstraintKind(ConstraintKind kind)
{
    switch (kind)
    {
    case ConstraintKind::Equal:
        return 0;
    case ConstraintKind::NotEqual:
        return 1;
    case ConstraintKind::LessThan:
        return 2;
    case ConstraintKind::LessEqual:
        return 3;
    case ConstraintKind::GreaterThan:
        return 4;
    case ConstraintKind::GreaterEqual:
        return 5;
    }
    throw std::logic_error("unknown linear constraint kind");
}

ConstraintKind decodeConstraintKind(std::uint8_t value)
{
    switch (value)
    {
    case 0:
        return ConstraintKind::Equal;
    case 1:
        return ConstraintKind::NotEqual;
    case 2:
        return ConstraintKind::LessThan;
    case 3:
        return ConstraintKind::LessEqual;
    case 4:
        return ConstraintKind::GreaterThan;
    case 5:
        return ConstraintKind::GreaterEqual;
    default:
        throw std::invalid_argument("raw state has invalid constraint kind");
    }
}

DomainTag domainTag(const NumericalDomain& state)
{
    if (state.isDomain<BoxDomain>())
        return DomainTag::Box;
    throw std::invalid_argument(
        "raw serialization does not support this domain");
}

std::uint8_t configurationFlags(const NumericalDomain& state, DomainTag tag)
{
    switch (tag)
    {
    case DomainTag::Box: {
        const auto& box = static_cast<const BoxDomain&>(state);
        return box.config().integerTightening ? 1U : 0U;
    }
    }
    throw std::logic_error("unknown raw state domain tag");
}

void writeVariable(Writer& writer, Variable variable)
{
    writer.writeU32(variable.id());
    writer.writeByte(encodeKind(variable.type().kind));
    writer.writeU32(variable.type().floatFormat.exponentBits);
    writer.writeU32(variable.type().floatFormat.significandBits);
}

Variable readVariable(Reader& reader)
{
    const std::uint32_t id = reader.readU32();
    NumericType type;
    type.kind = decodeKind(reader.readByte());
    type.floatFormat.exponentBits = reader.readU32();
    type.floatFormat.significandBits = reader.readU32();
    return Variable(id, type);
}

void writeConstraints(Writer& writer, const LinearConstraintSet& constraints)
{
    if (constraints.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("raw state has too many constraints");
    writer.writeU32(static_cast<std::uint32_t>(constraints.size()));
    for (const LinearConstraint& constraint : constraints)
    {
        writer.writeByte(encodeConstraintKind(constraint.kind()));
        writer.writeString(constraint.expression().constant().toString());
        const auto& terms = constraint.expression().terms();
        if (terms.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("raw state constraint has too many terms");
        writer.writeU32(static_cast<std::uint32_t>(terms.size()));
        for (const auto& [variable, coefficient] : terms)
        {
            writeVariable(writer, variable);
            writer.writeString(coefficient.toString());
        }
    }
}

LinearConstraintSet canonicalConstraints(const NumericalDomain& state,
                                         DomainTag)
{
    return state.isBottom() ? LinearConstraintSet{} : state.toConstraints();
}

Rational readRational(Reader& reader)
{
    const std::string encoded = reader.readString();
    if (encoded.empty())
        throw std::invalid_argument("raw state contains an empty rational");
    try
    {
        return Rational(encoded);
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("raw state contains an invalid rational");
    }
}

LinearConstraintSet readConstraints(Reader& reader)
{
    const std::uint32_t count = reader.readU32();
    if (count > MaxCollectionEntries)
        throw std::invalid_argument("raw state has too many constraints");
    LinearConstraintSet constraints;
    constraints.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const ConstraintKind kind = decodeConstraintKind(reader.readByte());
        LinearExpression expression(readRational(reader));
        const std::uint32_t termCount = reader.readU32();
        if (termCount > MaxCollectionEntries)
            throw std::invalid_argument("raw state has too many terms");
        std::set<Variable> seen;
        for (std::uint32_t term = 0; term < termCount; ++term)
        {
            const Variable variable = readVariable(reader);
            if (!seen.insert(variable).second)
                throw std::invalid_argument(
                    "raw state constraint repeats a variable");
            expression.setCoefficient(variable, readRational(reader));
        }
        constraints.emplace_back(std::move(expression), kind);
    }
    return constraints;
}

DomainTag decodeDomainTag(std::uint8_t value)
{
    switch (value)
    {
    case static_cast<std::uint8_t>(DomainTag::Box):
        return DomainTag::Box;
    default:
        throw std::invalid_argument("raw state has an unknown domain tag");
    }
}

std::unique_ptr<NumericalDomain> restore(DomainTag tag, std::uint8_t flags,
                                         bool bottom,
                                         const LinearConstraintSet& constraints)
{
    switch (tag)
    {
    case DomainTag::Box: {
        if ((flags & ~1U) != 0)
            throw std::invalid_argument("raw Box state has invalid flags");
        BoxSemanticConfig config;
        config.integerTightening = (flags & 1U) != 0;
        BoxDomain state = bottom
                              ? BoxDomain::bottom(config)
                              : BoxDomain::fromConstraints(constraints, config);
        return std::make_unique<BoxDomain>(std::move(state));
    }
    }
    throw std::logic_error("unknown raw state domain tag");
}

Interval bottomInterval()
{
    return Interval(Bound::plusInfinity(), Bound::minusInfinity());
}

bool singletonZero(const Interval& value)
{
    return value.lower().isFinite() && value.upper().isFinite() &&
           value.lower().value().isZero() && value.upper().value().isZero() &&
           !value.lower().isStrict() && !value.upper().isStrict();
}

std::optional<Rational> singletonValue(const Interval& value)
{
    if (!value.lower().isFinite() || !value.upper().isFinite() ||
        value.lower().isStrict() || value.upper().isStrict() ||
        value.lower().value() != value.upper().value())
        return std::nullopt;
    return value.lower().value();
}

Interval negateInterval(const Interval& value)
{
    if (value.isBottom())
        return bottomInterval();
    const Bound lower =
        value.upper().isFinite()
            ? Bound::finite(-value.upper().value(), value.upper().isStrict())
        : value.upper().isPlusInfinity() ? Bound::minusInfinity()
                                         : Bound::plusInfinity();
    const Bound upper =
        value.lower().isFinite()
            ? Bound::finite(-value.lower().value(), value.lower().isStrict())
        : value.lower().isMinusInfinity() ? Bound::plusInfinity()
                                          : Bound::minusInfinity();
    return Interval(lower, upper);
}

Interval addIntervals(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return bottomInterval();
    Bound lower = Bound::minusInfinity();
    Bound upper = Bound::plusInfinity();
    if (lhs.lower().isFinite() && rhs.lower().isFinite())
        lower = Bound::finite(lhs.lower().value() + rhs.lower().value(),
                              lhs.lower().isStrict() || rhs.lower().isStrict());
    if (lhs.upper().isFinite() && rhs.upper().isFinite())
        upper = Bound::finite(lhs.upper().value() + rhs.upper().value(),
                              lhs.upper().isStrict() || rhs.upper().isStrict());
    return Interval(lower, upper);
}

struct ExtendedRational
{
    /// -1 is minus infinity, 0 is finite, and 1 is plus infinity.
    int infinity = 0;
    Rational value;
};

ExtendedRational extended(const Bound& bound)
{
    if (bound.isMinusInfinity())
        return {-1, Rational()};
    if (bound.isPlusInfinity())
        return {1, Rational()};
    return {0, bound.value()};
}

int compareExtended(const ExtendedRational& lhs, const ExtendedRational& rhs)
{
    if (lhs.infinity != rhs.infinity)
        return lhs.infinity < rhs.infinity ? -1 : 1;
    if (lhs.infinity != 0)
        return 0;
    if (lhs.value < rhs.value)
        return -1;
    if (rhs.value < lhs.value)
        return 1;
    return 0;
}

ExtendedRational multiplyExtended(const ExtendedRational& lhs,
                                  const ExtendedRational& rhs)
{
    if (lhs.infinity == 0 && rhs.infinity == 0)
        return {0, lhs.value * rhs.value};
    if ((lhs.infinity == 0 && lhs.value.isZero()) ||
        (rhs.infinity == 0 && rhs.value.isZero()))
        return {0, Rational()};
    const int lhsSign = lhs.infinity != 0 ? lhs.infinity : lhs.value.sign();
    const int rhsSign = rhs.infinity != 0 ? rhs.infinity : rhs.value.sign();
    return {lhsSign * rhsSign, Rational()};
}

Bound extendedBound(const ExtendedRational& value)
{
    if (value.infinity < 0)
        return Bound::minusInfinity();
    if (value.infinity > 0)
        return Bound::plusInfinity();
    return Bound::finite(value.value);
}

Interval multiplyIntervals(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return bottomInterval();
    if (singletonZero(lhs) || singletonZero(rhs))
        return Interval::singleton(Rational());
    const ExtendedRational lhsLower = extended(lhs.lower());
    const ExtendedRational lhsUpper = extended(lhs.upper());
    const ExtendedRational rhsLower = extended(rhs.lower());
    const ExtendedRational rhsUpper = extended(rhs.upper());
    const std::array<ExtendedRational, 4> products{
        multiplyExtended(lhsLower, rhsLower),
        multiplyExtended(lhsLower, rhsUpper),
        multiplyExtended(lhsUpper, rhsLower),
        multiplyExtended(lhsUpper, rhsUpper)};
    ExtendedRational lower = products.front();
    ExtendedRational upper = products.front();
    for (const ExtendedRational& product : products)
    {
        if (compareExtended(product, lower) < 0)
            lower = product;
        if (compareExtended(upper, product) < 0)
            upper = product;
    }
    return Interval(extendedBound(lower), extendedBound(upper));
}

bool containsZero(const Interval& value)
{
    if (value.isBottom())
        return false;
    const bool aboveLower =
        value.lower().isMinusInfinity() ||
        (value.lower().isFinite() &&
         (value.lower().value() < Rational() ||
          (value.lower().value().isZero() && !value.lower().isStrict())));
    const bool belowUpper =
        value.upper().isPlusInfinity() ||
        (value.upper().isFinite() &&
         (Rational() < value.upper().value() ||
          (value.upper().value().isZero() && !value.upper().isStrict())));
    return aboveLower && belowUpper;
}

Rational truncateTowardZero(const Rational& value)
{
    return value.sign() < 0 ? value.ceil() : value.floor();
}

Interval divideIntervals(const Interval& lhs, const Interval& rhs,
                         bool integerDivision)
{
    if (lhs.isBottom() || rhs.isBottom())
        return bottomInterval();
    if (containsZero(rhs))
        return Interval::top();

    Bound reciprocalLower;
    Bound reciprocalUpper;
    const bool positive =
        rhs.lower().isFinite() && rhs.lower().value().sign() >= 0;
    if (positive)
    {
        reciprocalLower =
            rhs.upper().isPlusInfinity()
                ? Bound::finite(Rational())
                : Bound::finite(Rational(1) / rhs.upper().value());
        reciprocalUpper =
            rhs.lower().value().isZero()
                ? Bound::plusInfinity()
                : Bound::finite(Rational(1) / rhs.lower().value());
    }
    else
    {
        reciprocalLower =
            rhs.upper().isFinite() && rhs.upper().value().isZero()
                ? Bound::minusInfinity()
                : Bound::finite(Rational(1) / rhs.upper().value());
        reciprocalUpper =
            rhs.lower().isMinusInfinity()
                ? Bound::finite(Rational())
                : Bound::finite(Rational(1) / rhs.lower().value());
    }
    Interval result =
        multiplyIntervals(lhs, Interval(reciprocalLower, reciprocalUpper));
    if (!integerDivision || result.isBottom())
        return result;
    const Bound lower =
        result.lower().isFinite()
            ? Bound::finite(truncateTowardZero(result.lower().value()))
            : result.lower();
    const Bound upper =
        result.upper().isFinite()
            ? Bound::finite(truncateTowardZero(result.upper().value()))
            : result.upper();
    return Interval(lower, upper);
}

Rational powerOfTwo(long exponent)
{
    mpq_class value(1);
    if (exponent >= 0)
        mpz_mul_2exp(value.get_num_mpz_t(), value.get_num_mpz_t(), exponent);
    else
        mpz_mul_2exp(value.get_den_mpz_t(), value.get_den_mpz_t(), -exponent);
    value.canonicalize();
    return Rational::fromRaw(value);
}

struct IEEEFormatBounds
{
    Rational maximum;
    Rational minimumNormal;
    Rational minimumSubnormal;
};

IEEEFormatBounds ieeeBounds(const FloatFormat& format)
{
    if (format.exponentBits < 2 || format.exponentBits >= 63 ||
        format.significandBits < 2)
        throw std::invalid_argument("invalid IEEE floating format");
    const std::uint64_t bias =
        (std::uint64_t(1) << (format.exponentBits - 1)) - 1;
    const Rational maximum =
        (Rational(2) -
         powerOfTwo(1 - static_cast<long>(format.significandBits))) *
        powerOfTwo(static_cast<long>(bias));
    const Rational minimumNormal = powerOfTwo(1 - static_cast<long>(bias));
    const long minimumExponent = 1 - static_cast<long>(bias) -
                                 static_cast<long>(format.significandBits - 1);
    return {maximum, minimumNormal, powerOfTwo(minimumExponent)};
}

Rational roundIntegral(const Rational& value, RoundingMode rounding)
{
    const Rational lower = value.floor();
    const Rational upper = value.ceil();
    switch (rounding)
    {
    case RoundingMode::TowardZero:
        return value.sign() < 0 ? upper : lower;
    case RoundingMode::TowardPositive:
        return upper;
    case RoundingMode::TowardNegative:
        return lower;
    case RoundingMode::NearestTiesToEven: {
        const Rational lowerDistance = value - lower;
        const Rational upperDistance = upper - value;
        if (lowerDistance < upperDistance)
            return lower;
        if (upperDistance < lowerDistance)
            return upper;
        return mpz_even_p(lower.value().get_num_mpz_t()) != 0 ? lower : upper;
    }
    }
    return value;
}

std::optional<Rational> roundedIEEE(const Rational& value,
                                    const FloatFormat& format,
                                    RoundingMode rounding)
{
    const IEEEFormatBounds bounds = ieeeBounds(format);
    if (bounds.maximum < value || value < -bounds.maximum)
        return std::nullopt;
    if (-bounds.minimumNormal < value && value < bounds.minimumNormal)
        return roundIntegral(value / bounds.minimumSubnormal, rounding) *
               bounds.minimumSubnormal;
    return FloatSemantics::add(value, Rational(), format.significandBits,
                               rounding);
}

Interval roundIEEEInterval(const Interval& value, const FloatFormat& format,
                           RoundingMode rounding)
{
    if (value.isBottom())
        return bottomInterval();
    if (!value.lower().isFinite() || !value.upper().isFinite())
        return Interval::top();
    const std::optional<Rational> lower =
        roundedIEEE(value.lower().value(), format, rounding);
    const std::optional<Rational> upper =
        roundedIEEE(value.upper().value(), format, rounding);
    if (!lower || !upper)
        return Interval::top();
    return Interval(Bound::finite(*lower), Bound::finite(*upper));
}

Interval remainderIntervals(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return bottomInterval();
    if (containsZero(rhs))
        return Interval::top();
    if (singletonZero(lhs))
        return Interval::singleton(Rational());
    const std::optional<Rational> lhsValue = singletonValue(lhs);
    const std::optional<Rational> rhsValue = singletonValue(rhs);
    if (lhsValue && rhsValue)
    {
        const Rational quotient = truncateTowardZero(*lhsValue / *rhsValue);
        return Interval::singleton(*lhsValue - quotient * *rhsValue);
    }
    std::optional<Rational> magnitude;
    if (rhs.lower().isFinite() && rhs.upper().isFinite())
    {
        magnitude = rhs.lower().value().sign() < 0 ? -rhs.lower().value()
                                                   : rhs.lower().value();
        const Rational upperMagnitude = rhs.upper().value().sign() < 0
                                            ? -rhs.upper().value()
                                            : rhs.upper().value();
        if (*magnitude < upperMagnitude)
            magnitude = upperMagnitude;
    }
    if (lhs.lower().isFinite() && lhs.upper().isFinite())
    {
        Rational lhsMagnitude = lhs.lower().value().sign() < 0
                                    ? -lhs.lower().value()
                                    : lhs.lower().value();
        const Rational upperMagnitude = lhs.upper().value().sign() < 0
                                            ? -lhs.upper().value()
                                            : lhs.upper().value();
        if (lhsMagnitude < upperMagnitude)
            lhsMagnitude = upperMagnitude;
        if (!magnitude || lhsMagnitude < *magnitude)
            magnitude = lhsMagnitude;
    }
    if (!magnitude)
        return Interval::top();
    if (magnitude->isZero())
        return Interval::top();
    Rational lower = -*magnitude;
    Rational upper = *magnitude;
    if (lhs.lower().isFinite() && lhs.lower().value().sign() >= 0)
        lower = Rational();
    if (lhs.upper().isFinite() && lhs.upper().value().sign() <= 0)
        upper = Rational();
    return Interval(Bound::finite(lower, true), Bound::finite(upper, true));
}

Interval squareRootInterval(const Interval& operand, const NumericType& type,
                            RoundingMode rounding)
{
    if (operand.isBottom())
        return bottomInterval();
    if (!operand.lower().isFinite() || operand.lower().value().sign() < 0)
        return Interval::top();
    const unsigned precision = type.kind == NumericKind::IEEEFloat
                                   ? type.floatFormat.significandBits
                                   : 256U;
    MpfrValue input(precision);
    MpfrValue output(precision);
    input.set(operand.lower().value(), MPFR_RNDD);
    mpfr_sqrt(output.raw(), input.raw(), MPFR_RNDD);
    const Rational lower = output.toRational();
    if (!operand.upper().isFinite())
        return Interval(Bound::finite(lower), Bound::plusInfinity());
    input.set(operand.upper().value(), MPFR_RNDU);
    mpfr_sqrt(output.raw(), input.raw(), MPFR_RNDU);
    Interval result(Bound::finite(lower), Bound::finite(output.toRational()));
    return type.kind == NumericKind::IEEEFloat
               ? roundIEEEInterval(result, type.floatFormat, rounding)
               : result;
}

Interval castInterval(const Interval& operand, const NumericType& type,
                      RoundingMode rounding)
{
    if (operand.isBottom())
        return bottomInterval();
    if (type.kind == NumericKind::Real)
        return operand;
    if (type.kind == NumericKind::IEEEFloat)
        return roundIEEEInterval(operand, type.floatFormat, rounding);
    if (!operand.lower().isFinite() || !operand.upper().isFinite())
        return Interval::top();
    Rational lower = truncateTowardZero(operand.lower().value());
    Rational upper = truncateTowardZero(operand.upper().value());
    if (upper < lower)
        std::swap(lower, upper);
    return Interval(Bound::finite(lower), Bound::finite(upper));
}

Interval evaluateTree(const NumericalDomain& state,
                      const TreeExpression& expression)
{
    switch (expression.kind())
    {
    case TreeExpression::Kind::Constant:
        return castInterval(Interval::singleton(expression.constant()),
                            expression.type(), expression.roundingMode());
    case TreeExpression::Kind::Variable:
        if (expression.variable().type() != expression.type())
            throw std::invalid_argument(
                "tree variable type does not match its stable identity");
        return state.bound(expression.variable());
    case TreeExpression::Kind::Unary: {
        const Interval operand = evaluateTree(state, expression.lhs());
        switch (expression.unaryOperator())
        {
        case UnaryOperator::Negate:
            return expression.type().kind == NumericKind::IEEEFloat
                       ? roundIEEEInterval(negateInterval(operand),
                                           expression.type().floatFormat,
                                           expression.roundingMode())
                       : negateInterval(operand);
        case UnaryOperator::Cast:
            return castInterval(operand, expression.type(),
                                expression.roundingMode());
        case UnaryOperator::SquareRoot:
            return squareRootInterval(operand, expression.type(),
                                      expression.roundingMode());
        }
    }
    case TreeExpression::Kind::Binary: {
        const Interval lhs = evaluateTree(state, expression.lhs());
        const Interval rhs = evaluateTree(state, expression.rhs());
        Interval result;
        switch (expression.binaryOperator())
        {
        case BinaryOperator::Add:
            result = addIntervals(lhs, rhs);
            break;
        case BinaryOperator::Subtract:
            result = addIntervals(lhs, negateInterval(rhs));
            break;
        case BinaryOperator::Multiply:
            result = multiplyIntervals(lhs, rhs);
            break;
        case BinaryOperator::Divide:
            result = divideIntervals(
                lhs, rhs, expression.type().kind == NumericKind::Integer);
            break;
        case BinaryOperator::Remainder:
            result = remainderIntervals(lhs, rhs);
            break;
        }
        return expression.type().kind == NumericKind::IEEEFloat
                   ? roundIEEEInterval(result, expression.type().floatFormat,
                                       expression.roundingMode())
                   : result;
    }
    }
    return Interval::top();
}

bool definitelyTrue(const Interval& value, ConstraintKind kind)
{
    if (value.isBottom())
        return true;
    switch (kind)
    {
    case ConstraintKind::Equal:
        return singletonZero(value);
    case ConstraintKind::NotEqual:
        return !containsZero(value);
    case ConstraintKind::LessEqual:
        return value.upper().isFinite() && value.upper().value() <= Rational();
    case ConstraintKind::LessThan:
        return value.upper().isFinite() &&
               (value.upper().value() < Rational() ||
                (value.upper().value().isZero() && value.upper().isStrict()));
    case ConstraintKind::GreaterEqual:
        return value.lower().isFinite() && Rational() <= value.lower().value();
    case ConstraintKind::GreaterThan:
        return value.lower().isFinite() &&
               (Rational() < value.lower().value() ||
                (value.lower().value().isZero() && value.lower().isStrict()));
    }
    return false;
}

bool definitelyFalse(const Interval& value, ConstraintKind kind)
{
    if (value.isBottom())
        return false;
    switch (kind)
    {
    case ConstraintKind::Equal:
        return !containsZero(value);
    case ConstraintKind::NotEqual:
        return singletonZero(value);
    case ConstraintKind::LessEqual:
        return value.lower().isFinite() &&
               (Rational() < value.lower().value() ||
                (value.lower().value().isZero() && value.lower().isStrict()));
    case ConstraintKind::LessThan:
        return value.lower().isFinite() && Rational() <= value.lower().value();
    case ConstraintKind::GreaterEqual:
        return value.upper().isFinite() &&
               (value.upper().value() < Rational() ||
                (value.upper().value().isZero() && value.upper().isStrict()));
    case ConstraintKind::GreaterThan:
        return value.upper().isFinite() && value.upper().value() <= Rational();
    }
    return false;
}

struct BilinearDecomposition
{
    LinearExpression affine;
    LinearExpression lhs;
    LinearExpression rhs;
    Rational factor;
    bool hasProduct = false;
};

bool affineConstant(const std::optional<LinearExpression>& expression,
                    Rational& value)
{
    if (!expression || !expression->terms().empty())
        return false;
    value = expression->constant();
    return true;
}

bool decomposeSingleProduct(const TreeExpression& expression,
                            const Rational& scale,
                            BilinearDecomposition& result)
{
    if (scale.isZero())
        return true;
    if (const std::optional<LinearExpression> linear = expression.asLinear())
    {
        result.affine += *linear * scale;
        return true;
    }
    if (expression.kind() == TreeExpression::Kind::Unary &&
        expression.unaryOperator() == UnaryOperator::Negate)
        return decomposeSingleProduct(expression.lhs(), -scale, result);
    if (expression.kind() != TreeExpression::Kind::Binary)
        return false;

    if (expression.binaryOperator() == BinaryOperator::Add ||
        expression.binaryOperator() == BinaryOperator::Subtract)
    {
        if (!decomposeSingleProduct(expression.lhs(), scale, result))
            return false;
        const Rational rhsScale =
            expression.binaryOperator() == BinaryOperator::Add ? scale : -scale;
        return decomposeSingleProduct(expression.rhs(), rhsScale, result);
    }

    const std::optional<LinearExpression> lhs = expression.lhs().asLinear();
    const std::optional<LinearExpression> rhs = expression.rhs().asLinear();
    if (expression.binaryOperator() == BinaryOperator::Multiply)
    {
        if (lhs && rhs)
        {
            if (result.hasProduct)
                return false;
            result.lhs = *lhs;
            result.rhs = *rhs;
            result.factor = scale;
            result.hasProduct = true;
            return true;
        }
        Rational constant;
        if (affineConstant(lhs, constant))
            return decomposeSingleProduct(expression.rhs(), scale * constant,
                                          result);
        if (affineConstant(rhs, constant))
            return decomposeSingleProduct(expression.lhs(), scale * constant,
                                          result);
        return false;
    }
    if (expression.binaryOperator() == BinaryOperator::Divide)
    {
        Rational divisor;
        return affineConstant(rhs, divisor) && !divisor.isZero() &&
               decomposeSingleProduct(expression.lhs(), scale / divisor,
                                      result);
    }
    return false;
}

} // namespace

Interval add(const Interval& lhs, const Interval& rhs)
{
    return addIntervals(lhs, rhs);
}

Interval subtract(const Interval& lhs, const Interval& rhs)
{
    return addIntervals(lhs, negateInterval(rhs));
}

Interval multiply(const Interval& lhs, const Interval& rhs)
{
    return multiplyIntervals(lhs, rhs);
}

Interval divide(const Interval& lhs, const Interval& rhs, bool integerDivision)
{
    return divideIntervals(lhs, rhs, integerDivision);
}

Interval remainder(const Interval& lhs, const Interval& rhs)
{
    return remainderIntervals(lhs, rhs);
}

namespace
{

std::optional<mpz_class> singletonInteger(const Interval& interval)
{
    if (!interval.isSingleton() || !interval.singletonValue().isInteger())
        return std::nullopt;
    return mpz_class(mpq_numref(interval.singletonValue().value().get_mpq_t()));
}

std::optional<mpz_class> finiteInteger(const Bound& bound)
{
    if (!bound.isFinite() || bound.isStrict() || !bound.value().isInteger())
        return std::nullopt;
    return mpz_class(mpq_numref(bound.value().value().get_mpq_t()));
}

Interval integerSingleton(const mpz_class& value)
{
    return Interval::singleton(Rational(value.get_str()));
}

Interval closedIntegers(const mpz_class& lower, const mpz_class& upper)
{
    return Interval::closed(Rational(lower.get_str()),
                            Rational(upper.get_str()));
}

Interval booleanInterval(bool value)
{
    return Interval::singleton(Rational(value ? 1 : 0));
}

Interval unknownBoolean()
{
    return Interval::closed(Rational(0), Rational(1));
}

template <typename Operation>
Interval singletonBitwise(const Interval& lhs, const Interval& rhs,
                          Operation operation)
{
    const std::optional<mpz_class> left = singletonInteger(lhs);
    const std::optional<mpz_class> right = singletonInteger(rhs);
    return left && right ? integerSingleton(operation(*left, *right))
                         : Interval::top();
}

bool intervalsDisjoint(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return true;
    auto separated = [](const Bound& upper, const Bound& lower) {
        if (!upper.isFinite() || !lower.isFinite())
            return false;
        return upper.value() < lower.value() ||
               (upper.value() == lower.value() &&
                (upper.isStrict() || lower.isStrict()));
    };
    if (separated(lhs.upper(), rhs.lower()) ||
        separated(rhs.upper(), lhs.lower()))
        return true;
    return false;
}

std::optional<std::pair<mpz_class, mpz_class>> boundedIntegerRange(
    const Interval& interval)
{
    const std::optional<mpz_class> lower = finiteInteger(interval.lower());
    const std::optional<mpz_class> upper = finiteInteger(interval.upper());
    if (!lower || !upper)
        return std::nullopt;
    return std::make_pair(*lower, *upper);
}

Interval nonnegativeBitwiseRange(const Interval& lhs, const Interval& rhs)
{
    const auto left = boundedIntegerRange(lhs);
    const auto right = boundedIntegerRange(rhs);
    if (!left || !right || left->first < 0 || right->first < 0)
        return Interval::top();
    const mpz_class maximum = std::max(left->second, right->second);
    if (maximum == 0)
        return integerSingleton(0);
    const mp_bitcnt_t bits = mpz_sizeinbase(maximum.get_mpz_t(), 2);
    mpz_class upper = 1;
    mpz_mul_2exp(upper.get_mpz_t(), upper.get_mpz_t(), bits);
    --upper;
    return closedIntegers(0, upper);
}

template <typename Operation>
Interval shiftedRange(const Interval& lhs, const Interval& rhs,
                      Operation operation)
{
    const auto values = boundedIntegerRange(lhs);
    const auto shifts = boundedIntegerRange(rhs);
    if (!values || !shifts)
        return Interval::top();
    if (shifts->second < 0)
        return Interval::bottom();
    const mpz_class firstShift = std::max(mpz_class(0), shifts->first);
    if (!mpz_fits_ulong_p(firstShift.get_mpz_t()) ||
        !mpz_fits_ulong_p(shifts->second.get_mpz_t()))
        return Interval::top();
    const unsigned long lowShift = firstShift.get_ui();
    const unsigned long highShift = shifts->second.get_ui();
    std::array<mpz_class, 4> candidates = {
        operation(values->first, lowShift), operation(values->first, highShift),
        operation(values->second, lowShift),
        operation(values->second, highShift)};
    const auto [minimum, maximum] =
        std::minmax_element(candidates.begin(), candidates.end());
    return closedIntegers(*minimum, *maximum);
}

} // namespace

Interval bitwiseAnd(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return Interval::bottom();
    const Interval exact = singletonBitwise(
        lhs, rhs, [](const mpz_class& left, const mpz_class& right) {
            return left & right;
        });
    if (!exact.isTop())
        return exact;
    const std::optional<mpz_class> lhsLower = finiteInteger(lhs.lower());
    const std::optional<mpz_class> lhsUpper = finiteInteger(lhs.upper());
    const std::optional<mpz_class> rhsLower = finiteInteger(rhs.lower());
    const std::optional<mpz_class> rhsUpper = finiteInteger(rhs.upper());
    if (lhsLower && lhsUpper && rhsLower && rhsUpper && *lhsLower >= 0 &&
        *rhsLower >= 0)
        return closedIntegers(0, std::min(*lhsUpper, *rhsUpper));
    return Interval::top();
}

Interval bitwiseOr(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return Interval::bottom();
    const Interval exact = singletonBitwise(
        lhs, rhs, [](const mpz_class& left, const mpz_class& right) {
            return left | right;
        });
    return exact.isTop() ? nonnegativeBitwiseRange(lhs, rhs) : exact;
}

Interval bitwiseXor(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return Interval::bottom();
    const Interval exact = singletonBitwise(
        lhs, rhs, [](const mpz_class& left, const mpz_class& right) {
            return left ^ right;
        });
    return exact.isTop() ? nonnegativeBitwiseRange(lhs, rhs) : exact;
}

Interval shiftLeft(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return Interval::bottom();
    return shiftedRange(lhs, rhs,
                        [](const mpz_class& value, unsigned long shift) {
                            return value << shift;
                        });
}

Interval shiftRight(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return Interval::bottom();
    return shiftedRange(
        lhs, rhs, [](const mpz_class& value, unsigned long shift) {
            mpz_class result;
            mpz_fdiv_q_2exp(result.get_mpz_t(), value.get_mpz_t(), shift);
            return result;
        });
}

Interval equalTo(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return Interval::bottom();
    if (lhs.isSingleton() && rhs.isSingleton())
        return booleanInterval(lhs.singletonValue() == rhs.singletonValue());
    return intervalsDisjoint(lhs, rhs) ? booleanInterval(false)
                                       : unknownBoolean();
}

Interval notEqualTo(const Interval& lhs, const Interval& rhs)
{
    const Interval equal = equalTo(lhs, rhs);
    if (equal.isBottom() || !equal.isSingleton())
        return equal;
    return booleanInterval(equal.isZero());
}

Interval lessThan(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return Interval::bottom();
    if (lhs.upper().isFinite() && rhs.lower().isFinite() &&
        (lhs.upper().value() < rhs.lower().value() ||
         (lhs.upper().value() == rhs.lower().value() &&
          (lhs.upper().isStrict() || rhs.lower().isStrict()))))
        return booleanInterval(true);
    if (lhs.lower().isFinite() && rhs.upper().isFinite() &&
        rhs.upper().value() <= lhs.lower().value())
        return booleanInterval(false);
    return unknownBoolean();
}

Interval lessEqual(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom() || rhs.isBottom())
        return Interval::bottom();
    if (lhs.upper().isFinite() && rhs.lower().isFinite() &&
        lhs.upper().value() <= rhs.lower().value())
        return booleanInterval(true);
    if (lhs.lower().isFinite() && rhs.upper().isFinite() &&
        rhs.upper().value() < lhs.lower().value())
        return booleanInterval(false);
    return unknownBoolean();
}

Interval greaterThan(const Interval& lhs, const Interval& rhs)
{
    return lessThan(rhs, lhs);
}

Interval greaterEqual(const Interval& lhs, const Interval& rhs)
{
    return lessEqual(rhs, lhs);
}

void NumericalDomain::assignParallel(const TreeAssignmentList& assignments)
{
    std::set<Variable> targets;
    LinearAssignmentList affine;
    std::vector<std::pair<Variable, Interval>> intervalized;
    affine.reserve(assignments.size());
    intervalized.reserve(assignments.size());
    for (const TreeAssignment& assignment : assignments)
    {
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument(
                "parallel tree assignment contains a duplicate target");
        if (const std::optional<LinearExpression> linear =
                assignment.expression.asLinear())
            affine.push_back({assignment.target, *linear});
        else
            intervalized.emplace_back(
                assignment.target,
                evaluateTreeExpression(assignment.expression));
    }

    // Every nonlinear RHS interval was evaluated above from the common
    // incoming state. Affine right-hand sides now run simultaneously, then the
    // precomputed nonlinear intervals are committed without rereading targets.
    assignParallel(affine);
    for (const auto& [target, value] : intervalized)
        assignInterval(target, value);
    if (!intervalized.empty())
        recordOperation(OperationKind::Assignment,
                        ApproximationKind::SoundOverApproximation, false,
                        "parallel nonlinear or finite IEEE assignments were "
                        "interval-linearized");
}

void NumericalDomain::assumeAll(const LinearConstraintSet& constraints)
{
    if (constraints.empty())
    {
        recordOperation(OperationKind::Assumption, ApproximationKind::Exact,
                        true);
        return;
    }
    if (constraints.size() < 2)
    {
        for (const LinearConstraint& constraint : constraints)
            assume(constraint);
        return;
    }

    // One pass per constraint and variable bounds any propagation chain that
    // terminates at all; the equivalence test stops earlier in practice, and
    // immediately for a domain that is exact on linear constraints.
    std::set<Variable> variables;
    for (const LinearConstraint& constraint : constraints)
        for (const auto& [variable, coefficient] :
             constraint.expression().terms())
        {
            (void)coefficient;
            variables.insert(variable);
        }
    const std::size_t limit = constraints.size() * (variables.size() + 1) + 1;
    for (std::size_t pass = 0; pass < limit; ++pass)
    {
        const std::unique_ptr<AbstractDomain> before = clone();
        for (const LinearConstraint& constraint : constraints)
            assume(constraint);
        if (isBottom() || isEquivalentTo(*before) == CheckResult::True)
            return;
    }
}

NumericalDomain::RawBuffer NumericalDomain::serializeRaw() const
{
    Writer writer;
    writer.writeMagic();
    writer.writeU16(RawVersion);
    const DomainTag tag = domainTag(*this);
    writer.writeByte(static_cast<std::uint8_t>(tag));
    writer.writeByte(configurationFlags(*this, tag));
    writer.writeByte(isBottom() ? 1U : 0U);
    writeConstraints(writer, canonicalConstraints(*this, tag));
    return writer.finish();
}

void NumericalDomain::substitute(Variable target,
                                 const TreeExpression& expression)
{
    if (const std::optional<LinearExpression> linear = expression.asLinear())
    {
        substitute(target, *linear);
        return;
    }
    // Existentially eliminate the unknown post-value. No fact involving that
    // value can soundly constrain the pre-state without nonlinear/machine
    // semantics for the right-hand side.
    forget(target);
    recordOperation(OperationKind::Substitution,
                    ApproximationKind::UnsupportedFallback, false,
                    "nonlinear backward substitution projected the output");
}

void NumericalDomain::substituteParallel(const TreeAssignmentList& assignments)
{
    std::set<Variable> targets;
    LinearAssignmentList affine;
    std::vector<Variable> unsupported;
    affine.reserve(assignments.size());
    unsupported.reserve(assignments.size());
    for (const TreeAssignment& assignment : assignments)
    {
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument(
                "parallel substitution contains a duplicate target");
        if (const std::optional<LinearExpression> linear =
                assignment.expression.asLinear())
            affine.push_back({assignment.target, *linear});
        else
            unsupported.push_back(assignment.target);
    }

    // Unknown output variables are existentially projected before the
    // remaining simultaneous affine preimage is formed.
    for (Variable target : unsupported)
        forget(target);
    substituteParallel(affine);
    if (!unsupported.empty())
        recordOperation(OperationKind::Substitution,
                        ApproximationKind::UnsupportedFallback, false,
                        "parallel nonlinear backward substitution projected "
                        "unsupported outputs");
}

Interval NumericalDomain::bound(const TreeExpression& expression) const
{
    if (const std::optional<LinearExpression> linear = expression.asLinear())
        return bound(*linear);
    if (isBottom())
        return bottomInterval();
    return evaluateTreeExpression(expression);
}

Interval NumericalDomain::evaluateTreeExpression(
    const TreeExpression& expression) const
{
    if (isBottom())
        return bottomInterval();
    return evaluateTree(*this, expression);
}

LinearConstraintSet NumericalDomain::treeConstraintConsequences(
    const TreeConstraint& constraint) const
{
    const Interval value = evaluateTreeExpression(constraint.expression());
    if (definitelyFalse(value, constraint.kind()))
        return {LinearConstraint(LinearExpression(Rational(1)),
                                 ConstraintKind::LessEqual)};
    if (definitelyTrue(value, constraint.kind()))
        return {};

    const TreeExpression& expression = constraint.expression();
    if (expression.type().kind == NumericKind::IEEEFloat)
        return {};
    BilinearDecomposition decomposition;
    if (!decomposeSingleProduct(expression, Rational(1), decomposition) ||
        !decomposition.hasProduct)
        return {};
    const Interval lhsBounds = bound(decomposition.lhs);
    const Interval rhsBounds = bound(decomposition.rhs);
    if (!lhsBounds.lower().isFinite() || !lhsBounds.upper().isFinite() ||
        !rhsBounds.lower().isFinite() || !rhsBounds.upper().isFinite())
        return {};

    const Rational& lx = lhsBounds.lower().value();
    const Rational& ux = lhsBounds.upper().value();
    const Rational& ly = rhsBounds.lower().value();
    const Rational& uy = rhsBounds.upper().value();
    std::vector<LinearExpression> productLowerForms{
        decomposition.rhs * lx + decomposition.lhs * ly -
            LinearExpression(lx * ly),
        decomposition.rhs * ux + decomposition.lhs * uy -
            LinearExpression(ux * uy)};
    std::vector<LinearExpression> productUpperForms{
        decomposition.rhs * ux + decomposition.lhs * ly -
            LinearExpression(ux * ly),
        decomposition.rhs * lx + decomposition.lhs * uy -
            LinearExpression(lx * uy)};
    if (decomposition.factor.sign() < 0)
        std::swap(productLowerForms, productUpperForms);
    std::vector<LinearExpression> lowerForms;
    std::vector<LinearExpression> upperForms;
    lowerForms.reserve(productLowerForms.size());
    upperForms.reserve(productUpperForms.size());
    for (const LinearExpression& form : productLowerForms)
        lowerForms.push_back(decomposition.affine +
                             form * decomposition.factor);
    for (const LinearExpression& form : productUpperForms)
        upperForms.push_back(decomposition.affine +
                             form * decomposition.factor);

    LinearConstraintSet result;
    const auto appendLower = [&](ConstraintKind kind) {
        for (const LinearExpression& form : lowerForms)
            result.emplace_back(form, kind);
    };
    const auto appendUpper = [&](ConstraintKind kind) {
        for (const LinearExpression& form : upperForms)
            result.emplace_back(form, kind);
    };
    switch (constraint.kind())
    {
    case ConstraintKind::LessEqual:
        appendLower(ConstraintKind::LessEqual);
        break;
    case ConstraintKind::LessThan:
        appendLower(ConstraintKind::LessThan);
        break;
    case ConstraintKind::GreaterEqual:
        appendUpper(ConstraintKind::GreaterEqual);
        break;
    case ConstraintKind::GreaterThan:
        appendUpper(ConstraintKind::GreaterThan);
        break;
    case ConstraintKind::Equal:
        appendLower(ConstraintKind::LessEqual);
        appendUpper(ConstraintKind::GreaterEqual);
        break;
    case ConstraintKind::NotEqual:
        break;
    }
    return result;
}

void NumericalDomain::assignInterval(Variable target, const Interval& value)
{
    if (isBottom())
        return;
    forget(target);
    if (value.isBottom())
    {
        assume(LinearConstraint(LinearExpression(Rational(1)),
                                ConstraintKind::LessEqual));
        return;
    }
    if (value.lower().isFinite())
        assume(LinearConstraint(
            LinearExpression(target) - LinearExpression(value.lower().value()),
            value.lower().isStrict() ? ConstraintKind::GreaterThan
                                     : ConstraintKind::GreaterEqual));
    if (value.upper().isFinite())
        assume(LinearConstraint(
            LinearExpression(target) - LinearExpression(value.upper().value()),
            value.upper().isStrict() ? ConstraintKind::LessThan
                                     : ConstraintKind::LessEqual));
}

void NumericalDomain::recordOperation(OperationKind operation,
                                      ApproximationKind approximation,
                                      bool best, std::string reason) const
{
    lastOperation_ = {operation, approximation,
                      approximation == ApproximationKind::Exact, best,
                      std::move(reason)};
}

std::uint64_t NumericalDomain::hash() const
{
    const RawBuffer raw = serializeRaw();
    return readTrailingU64(raw);
}

std::unique_ptr<NumericalDomain> NumericalDomain::deserializeRaw(
    const RawBuffer& buffer)
{
    Reader reader(buffer);
    reader.readMagic();
    if (reader.readU16() != RawVersion)
        throw std::invalid_argument("raw state has an unsupported version");
    const DomainTag tag = decodeDomainTag(reader.readByte());
    const std::uint8_t flags = reader.readByte();
    const std::uint8_t bottomByte = reader.readByte();
    if (bottomByte > 1)
        throw std::invalid_argument("raw state has an invalid bottom flag");
    const LinearConstraintSet constraints = readConstraints(reader);
    if (!reader.empty())
        throw std::invalid_argument("raw state has trailing data");
    if (bottomByte != 0 && !constraints.empty())
        throw std::invalid_argument(
            "raw bottom state unexpectedly contains constraints");
    return restore(tag, flags, bottomByte != 0, constraints);
}

namespace
{

int compareLower(const Bound& lhs, const Bound& rhs)
{
    return compareIntervalLower(lhs, rhs);
}

Bound minLower(const Bound& lhs, const Bound& rhs)
{
    return compareLower(lhs, rhs) <= 0 ? lhs : rhs;
}

Bound maxLower(const Bound& lhs, const Bound& rhs)
{
    return compareLower(lhs, rhs) >= 0 ? lhs : rhs;
}

Bound scaleBound(const Bound& bound, const Rational& coefficient)
{
    if (coefficient.isZero())
        return Bound::finite(Rational());
    if (bound.isMinusInfinity())
        return coefficient.sign() > 0 ? Bound::minusInfinity()
                                      : Bound::plusInfinity();
    if (bound.isPlusInfinity())
        return coefficient.sign() > 0 ? Bound::plusInfinity()
                                      : Bound::minusInfinity();
    return Bound::finite(bound.value() * coefficient, bound.isStrict());
}

Interval scaleInterval(const Interval& interval, const Rational& coefficient)
{
    if (coefficient.isZero())
        return Interval::singleton(Rational());
    if (coefficient.sign() > 0)
        return Interval(scaleBound(interval.lower(), coefficient),
                        scaleBound(interval.upper(), coefficient));
    return Interval(scaleBound(interval.upper(), coefficient),
                    scaleBound(interval.lower(), coefficient));
}

Interval addBoxIntervals(const Interval& lhs, const Interval& rhs)
{
    return Interval(Bound::add(lhs.lower(), rhs.lower()),
                    Bound::add(lhs.upper(), rhs.upper()));
}

Interval joinIntervals(const Interval& lhs, const Interval& rhs)
{
    return Interval(minLower(lhs.lower(), rhs.lower()),
                    Bound::max(lhs.upper(), rhs.upper()));
}

Interval meetIntervals(const Interval& lhs, const Interval& rhs)
{
    return Interval(maxLower(lhs.lower(), rhs.lower()),
                    Bound::min(lhs.upper(), rhs.upper()));
}

bool intervalIncluded(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom())
        return true;
    if (rhs.isBottom())
        return false;
    return compareLower(lhs.lower(), rhs.lower()) >= 0 &&
           Bound::compare(lhs.upper(), rhs.upper()) <= 0;
}

Interval evaluate(const BoxDomain& state, const LinearExpression& expression,
                  std::optional<Variable> excluded = std::nullopt)
{
    Interval result = Interval::singleton(expression.constant());
    for (const auto& [variable, coefficient] : expression.terms())
    {
        if (excluded && variable == *excluded)
            continue;
        result = addBoxIntervals(
            result, scaleInterval(state.bound(variable), coefficient));
    }
    return result;
}

Bound integerLower(Bound bound)
{
    if (!bound.isFinite())
        return bound;
    const Rational value = bound.isStrict()
                               ? bound.value().floor() + Rational(1)
                               : bound.value().ceil();
    return Bound::finite(value);
}

Bound integerUpper(Bound bound)
{
    if (!bound.isFinite())
        return bound;
    const Rational value = bound.isStrict() ? bound.value().ceil() - Rational(1)
                                            : bound.value().floor();
    return Bound::finite(value);
}

LinearConstraint normalizedLessEqual(const LinearConstraint& constraint,
                                     bool& strict)
{
    strict = constraint.kind() == ConstraintKind::LessThan ||
             constraint.kind() == ConstraintKind::GreaterThan;
    if (constraint.kind() == ConstraintKind::GreaterEqual ||
        constraint.kind() == ConstraintKind::GreaterThan)
        return LinearConstraint(-constraint.expression(),
                                strict ? ConstraintKind::LessThan
                                       : ConstraintKind::LessEqual);
    return LinearConstraint(constraint.expression(),
                            strict ? ConstraintKind::LessThan
                                   : ConstraintKind::LessEqual);
}

} // namespace

BoxDomain::BoxDomain(BoxSemanticConfig config, bool bottom)
    : config_(std::move(config)), bottom_(bottom)
{
}

BoxDomain::BoxDomain(const BoxDomain& other)
    : NumericalDomain(other), config_(other.config_),
      boundPages_(other.boundPages_), bottom_(other.bottom_)
{
}

BoxDomain BoxDomain::top(const BoxSemanticConfig& config)
{
    BoxDomain result(config, false);
    return result;
}

BoxDomain BoxDomain::bottom(const BoxSemanticConfig& config)
{
    BoxDomain result(config, true);
    return result;
}

BoxDomain BoxDomain::fromConstraints(const LinearConstraintSet& constraints,
                                     const BoxSemanticConfig& config)
{
    BoxDomain result = top(config);
    result.assumeAll(constraints);
    return result;
}

std::unique_ptr<AbstractDomain> BoxDomain::clone() const
{
    return std::make_unique<BoxDomain>(*this);
}

void BoxDomain::assign(Variable target, const LinearExpression& expression)
{
    recordOperation(OperationKind::Assignment, ApproximationKind::Exact, true);
    if (bottom_)
        return;
    setBound(target, evaluate(*this, expression));
}

void BoxDomain::assign(Variable target, const TreeExpression& expression)
{
    const std::optional<LinearExpression> linear = expression.asLinear();
    if (linear)
    {
        assign(target, *linear);
        return;
    }
    const Interval value = evaluateTreeExpression(expression);
    if (!bottom_)
        setBound(target, value);
    report(OperationKind::Assignment, ApproximationKind::SoundOverApproximation,
           "nonlinear or finite IEEE assignment was interval-linearized",
           false);
}

void BoxDomain::assignParallel(const LinearAssignmentList& assignments)
{
    std::set<Variable> targets;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument(
                "parallel assignment contains a duplicate target");
    }
    recordOperation(OperationKind::Assignment, ApproximationKind::Exact, true);
    if (bottom_)
        return;

    std::vector<std::pair<Variable, Interval>> updates;
    updates.reserve(assignments.size());
    for (const LinearAssignment& assignment : assignments)
        updates.emplace_back(assignment.target,
                             evaluate(*this, assignment.expression));
    for (auto& [variable, value] : updates)
        setBound(variable, std::move(value));
}

void BoxDomain::substitute(Variable target, const LinearExpression& expression)
{
    substituteParallel({{target, expression}});
}

void BoxDomain::substituteParallel(const LinearAssignmentList& assignments)
{
    std::map<Variable, LinearExpression> replacements;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!replacements.emplace(assignment.target, assignment.expression)
                 .second)
            throw std::invalid_argument(
                "parallel substitution contains a duplicate target");
    }
    recordOperation(OperationKind::Substitution, ApproximationKind::Exact,
                    true);
    if (assignments.empty() || bottom_)
        return;

    LinearConstraintSet preimage;
    for (const LinearConstraint& constraint : toConstraints())
        preimage.emplace_back(constraint.expression().substituted(replacements),
                              constraint.kind());
    *this = fromConstraints(preimage, config_);
}

void BoxDomain::assume(const LinearConstraint& constraint)
{
    recordOperation(OperationKind::Assumption, ApproximationKind::Exact, true);
    if (bottom_)
        return;
    if (constraint.kind() == ConstraintKind::NotEqual)
    {
        const Interval value = evaluate(*this, constraint.expression());
        if (!value.lower().isFinite() || !value.upper().isFinite() ||
            value.lower().value() != Rational() ||
            value.upper().value() != Rational() || value.lower().isStrict() ||
            value.upper().isStrict())
            return;
        makeBottom();
        return;
    }

    if (constraint.kind() == ConstraintKind::Equal)
    {
        assume(LinearConstraint(constraint.expression(),
                                ConstraintKind::LessEqual));
        assume(LinearConstraint(-constraint.expression(),
                                ConstraintKind::LessEqual));
        return;
    }

    bool strict = false;
    const LinearConstraint normalized = normalizedLessEqual(constraint, strict);
    const LinearExpression& expression = normalized.expression();

    // Repeating interval propagation lets bounds inferred for one variable
    // tighten another without introducing an unbounded worklist.
    for (std::size_t pass = 0; pass <= expression.terms().size(); ++pass)
    {
        bool changed = false;
        for (const auto& [variable, coefficient] : expression.terms())
        {
            if (coefficient.isZero())
                continue;
            const Interval rest = evaluate(*this, expression, variable);
            if (!rest.lower().isFinite())
                continue;

            const Rational rhs = -rest.lower().value() / coefficient;
            const bool resultStrict = strict || rest.lower().isStrict();
            Interval next = boundAt(variable);
            if (coefficient.sign() > 0)
            {
                next = meetIntervals(
                    next, Interval(Bound::minusInfinity(),
                                   Bound::finite(rhs, resultStrict)));
            }
            else
            {
                next = meetIntervals(next,
                                     Interval(Bound::finite(rhs, resultStrict),
                                              Bound::plusInfinity()));
            }
            const Interval previous = boundAt(variable);
            setBound(variable, next);
            if (bottom_)
                return;
            changed = changed ||
                      !intervalIncluded(previous, boundAt(variable)) ||
                      !intervalIncluded(boundAt(variable), previous);
        }
        if (!changed)
            break;
    }

    const Interval value = evaluate(*this, expression);
    if (value.lower().isFinite())
    {
        const int sign = value.lower().value().sign();
        if (sign > 0 || (sign == 0 && (strict || value.lower().isStrict())))
            makeBottom();
    }
}

void BoxDomain::assume(const TreeConstraint& constraint)
{
    const std::optional<LinearExpression> linear =
        constraint.expression().asLinear();
    if (linear)
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    const LinearConstraintSet consequences =
        treeConstraintConsequences(constraint);
    assumeAll(consequences);
    report(OperationKind::Assumption, ApproximationKind::SoundOverApproximation,
           consequences.empty()
               ? "nonlinear or finite IEEE guard had no affine consequence"
               : "nonlinear guard was reduced to sound affine consequences",
           false);
}

void BoxDomain::forget(Variable variable)
{
    if (!bottom_)
        eraseBound(variable);
    recordOperation(OperationKind::Forget, ApproximationKind::Exact, true);
}

std::vector<Variable> BoxDomain::constrainedVariables() const
{
    std::vector<Variable> variables;
    if (bottom_)
        return variables;
    return boundedVariables();
}

void BoxDomain::expand(Variable source, const std::vector<Variable>& copies)
{
    std::set<Variable> seen;
    for (Variable copy : copies)
    {
        if (copy == source || !seen.insert(copy).second)
            throw std::invalid_argument(
                "expanded variables must be distinct from the source");
        if (copy.type() != source.type())
            throw std::invalid_argument(
                "expanded variables must have the source numeric type");
    }
    if (copies.empty())
    {
        recordOperation(OperationKind::Expand, ApproximationKind::Exact, true);
        return;
    }
    const Interval sourceValue = bound(source);
    for (Variable copy : copies)
        if (!bottom_)
            setBound(copy, sourceValue);
    recordOperation(OperationKind::Expand, ApproximationKind::Exact, true);
}

void BoxDomain::fold(Variable target, const std::vector<Variable>& folded)
{
    std::set<Variable> seen;
    std::vector<Variable> sources{target};
    for (Variable variable : folded)
    {
        if (variable == target || !seen.insert(variable).second)
            throw std::invalid_argument(
                "folded variables must be distinct non-target variables");
        if (variable.type() != target.type())
            throw std::invalid_argument(
                "folded variables must have the target numeric type");
        sources.push_back(variable);
    }
    if (folded.empty())
    {
        recordOperation(OperationKind::Fold, ApproximationKind::Exact, true);
        return;
    }

    BoxDomain result = bottom(config_);
    for (Variable source : sources)
    {
        BoxDomain branch = *this;
        if (source != target)
            branch.setBound(target, bound(source));
        result = result.join(branch);
    }
    for (Variable variable : folded)
        result.forget(variable);
    *this = std::move(result);
    recordOperation(OperationKind::Fold, ApproximationKind::Exact, true);
}

CheckResult BoxDomain::entails(const LinearConstraint& constraint) const
{
    if (bottom_)
        return CheckResult::True;
    const Interval value = evaluate(*this, constraint.expression());
    const auto upperAtMostZero = [&]() {
        if (!value.upper().isFinite())
            return false;
        return value.upper().value().sign() <= 0;
    };
    const auto upperBelowZero = [&]() {
        return value.upper().isFinite() &&
               (value.upper().value().sign() < 0 ||
                (value.upper().value().isZero() && value.upper().isStrict()));
    };
    const auto lowerAtLeastZero = [&]() {
        return value.lower().isFinite() && value.lower().value().sign() >= 0;
    };
    const auto lowerAboveZero = [&]() {
        return value.lower().isFinite() &&
               (value.lower().value().sign() > 0 ||
                (value.lower().value().isZero() && value.lower().isStrict()));
    };

    switch (constraint.kind())
    {
    case ConstraintKind::LessEqual:
        return upperAtMostZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::LessThan:
        return upperBelowZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::GreaterEqual:
        return lowerAtLeastZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::GreaterThan:
        return lowerAboveZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::Equal:
        return upperAtMostZero() && lowerAtLeastZero() ? CheckResult::True
                                                       : CheckResult::Unknown;
    case ConstraintKind::NotEqual:
        return upperBelowZero() || lowerAboveZero() ? CheckResult::True
                                                    : CheckResult::Unknown;
    }
    return CheckResult::Unknown;
}

Interval BoxDomain::bound(Variable variable) const
{
    if (bottom_)
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    return boundAt(variable);
}

Interval BoxDomain::bound(const LinearExpression& expression) const
{
    if (bottom_)
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    return evaluate(*this, expression);
}

LinearConstraintSet BoxDomain::toConstraints() const
{
    LinearConstraintSet result;
    if (bottom_)
    {
        result.emplace_back(LinearExpression(Rational(1)),
                            ConstraintKind::LessEqual);
        return result;
    }
    for (Variable variable : boundedVariables())
    {
        const Interval& interval = boundAt(variable);
        if (interval.lower().isFinite())
        {
            result.emplace_back(LinearExpression(variable) -
                                    LinearExpression(interval.lower().value()),
                                interval.lower().isStrict()
                                    ? ConstraintKind::GreaterThan
                                    : ConstraintKind::GreaterEqual);
        }
        if (interval.upper().isFinite())
        {
            result.emplace_back(LinearExpression(variable) -
                                    LinearExpression(interval.upper().value()),
                                interval.upper().isStrict()
                                    ? ConstraintKind::LessThan
                                    : ConstraintKind::LessEqual);
        }
    }
    return result;
}

void BoxDomain::close()
{
    recordOperation(OperationKind::TopologicalClosure, ApproximationKind::Exact,
                    true, "topological closure");
    if (bottom_)
        return;
    for (Variable variable : boundedVariables())
    {
        const Interval& interval = boundAt(variable);
        const Bound lower = interval.lower().isFinite()
                                ? Bound::finite(interval.lower().value())
                                : interval.lower();
        const Bound upper = interval.upper().isFinite()
                                ? Bound::finite(interval.upper().value())
                                : interval.upper();
        setBound(variable, Interval(lower, upper));
    }
}

void BoxDomain::canonicalize()
{
    for (Variable variable : boundedVariables())
        canonicalize(variable);
    recordOperation(OperationKind::Canonicalization, ApproximationKind::Exact,
                    true, "canonicalization");
}

BoxDomain BoxDomain::join(const BoxDomain& other) const
{
    BoxDomain result(*this);
    result.joinDomain(other);
    result.recordOperation(OperationKind::Join, ApproximationKind::Exact, true);
    return result;
}

BoxDomain BoxDomain::meet(const BoxDomain& other) const
{
    BoxDomain result(*this);
    result.meetDomain(other);
    result.recordOperation(OperationKind::Meet, ApproximationKind::Exact, true);
    return result;
}

BoxDomain BoxDomain::widen(const BoxDomain& next) const
{
    return widen(next, WideningPolicy{});
}

BoxDomain BoxDomain::widen(const BoxDomain& next,
                           const WideningPolicy& policy) const
{
    requireBox(next);
    if (bottom_)
    {
        BoxDomain result(next);
        result.recordOperation(OperationKind::Widening,
                               ApproximationKind::SoundOverApproximation, true);
        return result;
    }
    if (next.bottom_)
    {
        BoxDomain result(*this);
        result.recordOperation(OperationKind::Widening,
                               ApproximationKind::SoundOverApproximation, true);
        return result;
    }
    BoxDomain result(*this);
    for (Variable variable : boundedVariables())
    {
        Bound lower = boundAt(variable).lower();
        Bound upper = boundAt(variable).upper();
        const Interval& following = next.boundAt(variable);
        if (compareLower(following.lower(), lower) < 0)
        {
            lower = Bound::minusInfinity();
            if (following.lower().isFinite())
            {
                for (const Rational& threshold : policy.thresholds)
                {
                    if (threshold <= following.lower().value() &&
                        (lower.isMinusInfinity() || lower.value() < threshold))
                        lower = Bound::finite(threshold);
                }
            }
        }
        if (Bound::compare(following.upper(), upper) > 0)
        {
            upper = Bound::plusInfinity();
            if (following.upper().isFinite())
            {
                for (const Rational& threshold : policy.thresholds)
                {
                    if (following.upper().value() <= threshold &&
                        (upper.isPlusInfinity() || threshold < upper.value()))
                        upper = Bound::finite(threshold);
                }
            }
        }
        result.setBound(variable, Interval(lower, upper));
    }
    for (const LinearConstraint& threshold : policy.linearThresholds)
    {
        if (entails(threshold) == CheckResult::True &&
            next.entails(threshold) == CheckResult::True)
            result.assume(threshold);
    }
    result.recordOperation(OperationKind::Widening,
                           ApproximationKind::SoundOverApproximation, true);
    return result;
}

BoxDomain BoxDomain::narrow(const BoxDomain& next) const
{
    requireBox(next);
    if (bottom_ || next.bottom_)
    {
        BoxDomain result = bottom(config_);
        result.recordOperation(OperationKind::Narrowing,
                               ApproximationKind::Exact, true);
        return result;
    }
    BoxDomain result(*this);
    for (Variable variable : next.boundedVariables())
    {
        Bound lower = boundAt(variable).lower();
        Bound upper = boundAt(variable).upper();
        if (lower.isMinusInfinity())
            lower = next.boundAt(variable).lower();
        if (upper.isPlusInfinity())
            upper = next.boundAt(variable).upper();
        result.setBound(variable, Interval(lower, upper));
    }
    result.recordOperation(OperationKind::Narrowing, ApproximationKind::Exact,
                           true);
    return result;
}

bool BoxDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    const auto* box = other.isDomain<BoxDomain>()
                          ? &static_cast<const BoxDomain&>(other)
                          : nullptr;
    return box && config_.operationCompatible(box->config_);
}

void BoxDomain::joinDomain(const AbstractDomain& other)
{
    const BoxDomain& box = requireBox(other);
    if (box.bottom_)
        return;
    if (bottom_)
    {
        *this = box;
        return;
    }
    for (Variable variable : boundedVariables())
        setBound(variable,
                 joinIntervals(boundAt(variable), box.boundAt(variable)));
}

void BoxDomain::meetDomain(const AbstractDomain& other)
{
    const BoxDomain& box = requireBox(other);
    if (bottom_ || box.bottom_)
    {
        makeBottom();
        return;
    }
    for (Variable variable : box.boundedVariables())
    {
        setBound(variable,
                 meetIntervals(boundAt(variable), box.boundAt(variable)));
        if (bottom_)
            return;
    }
}

void BoxDomain::widenDomain(const AbstractDomain& next)
{
    *this = widen(requireBox(next));
}

void BoxDomain::narrowDomain(const AbstractDomain& next)
{
    *this = narrow(requireBox(next));
}

bool BoxDomain::isBottomDomain() const
{
    return bottom_;
}

bool BoxDomain::isTopDomain() const
{
    return !bottom_ && boundPages_.empty();
}

bool BoxDomain::leqDomain(const AbstractDomain& other) const
{
    const BoxDomain& box = requireBox(other);
    if (bottom_ == box.bottom_ && boundPages_.size() == box.boundPages_.size())
    {
        bool equal = true;
        for (std::size_t index = 0; index < boundPages_.size(); ++index)
        {
            if (boundPages_[index].index != box.boundPages_[index].index ||
                (boundPages_[index].page != box.boundPages_[index].page &&
                 boundPages_[index].page->bounds !=
                     box.boundPages_[index].page->bounds))
            {
                equal = false;
                break;
            }
        }
        if (equal)
            return true;
    }
    if (bottom_)
        return true;
    if (box.bottom_)
        return false;
    for (Variable variable : box.boundedVariables())
    {
        if (!intervalIncluded(boundAt(variable), box.boundAt(variable)))
            return false;
    }
    return true;
}

std::string BoxDomain::domainToString() const
{
    if (bottom_)
        return "bottom";
    std::ostringstream output;
    output << "{";
    bool first = true;
    for (Variable variable : boundedVariables())
    {
        if (!first)
            output << ", ";
        first = false;
        output << "v" << variable.id() << "=" << boundAt(variable).toString();
    }
    output << "}";
    return output.str();
}

const BoxDomain& BoxDomain::requireBox(const AbstractDomain& other) const
{
    requireCompatible(other);
    return static_cast<const BoxDomain&>(other);
}

void BoxDomain::canonicalize(Variable variable)
{
    if (bottom_)
        return;
    Interval interval = boundAt(variable);
    if (config_.integerTightening &&
        variable.type().kind == NumericKind::Integer)
    {
        interval = Interval(integerLower(interval.lower()),
                            integerUpper(interval.upper()));
    }
    if (interval.isBottom())
    {
        makeBottom();
        return;
    }
    if (interval.isTop())
        eraseBound(variable);
    else
        writablePage(variable.id() / BoundsPerPage)
            .bounds[variable.id() % BoundsPerPage] =
            BoundSlot{variable, std::move(interval)};
}

void BoxDomain::setBound(Variable variable, Interval interval)
{
    // A stable ID denotes one typed variable for the lifetime of an analysis.
    // Detect accidental ID reuse before touching the physical slot.
    (void)boundAt(variable);
    if (interval.isTop())
        eraseBound(variable);
    else
        writablePage(variable.id() / BoundsPerPage)
            .bounds[variable.id() % BoundsPerPage] =
            BoundSlot{variable, std::move(interval)};
    canonicalize(variable);
}

const Interval& BoxDomain::boundAt(Variable variable) const
{
    static const Interval top = Interval::top();
    const std::size_t pageIndex = variable.id() / BoundsPerPage;
    const auto iterator =
        std::lower_bound(boundPages_.begin(), boundPages_.end(), pageIndex,
                         [](const BoundPageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (iterator == boundPages_.end() || iterator->index != pageIndex)
        return top;
    const auto& slot = iterator->page->bounds[variable.id() % BoundsPerPage];
    if (!slot)
        return top;
    if (slot->variable != variable)
        throw std::invalid_argument(
            "Variable ID was reused with a different numeric type");
    return slot->interval;
}

BoxDomain::BoundPage& BoxDomain::writablePage(std::size_t pageIndex)
{
    auto iterator =
        std::lower_bound(boundPages_.begin(), boundPages_.end(), pageIndex,
                         [](const BoundPageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (iterator == boundPages_.end() || iterator->index != pageIndex)
        iterator = boundPages_.insert(
            iterator, {pageIndex, std::make_shared<BoundPage>()});
    else if (iterator->page.use_count() != 1)
        iterator->page = std::make_shared<BoundPage>(*iterator->page);
    return *iterator->page;
}

void BoxDomain::eraseBound(Variable variable)
{
    const std::size_t pageIndex = variable.id() / BoundsPerPage;
    auto existing =
        std::lower_bound(boundPages_.begin(), boundPages_.end(), pageIndex,
                         [](const BoundPageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (existing == boundPages_.end() || existing->index != pageIndex)
        return;
    const std::size_t offset = variable.id() % BoundsPerPage;
    if (!existing->page->bounds[offset])
        return;
    if (existing->page->bounds[offset]->variable != variable)
        throw std::invalid_argument(
            "Variable ID was reused with a different numeric type");
    auto iterator =
        std::lower_bound(boundPages_.begin(), boundPages_.end(), pageIndex,
                         [](const BoundPageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (iterator->page.use_count() != 1)
        iterator->page = std::make_shared<BoundPage>(*iterator->page);
    iterator->page->bounds[offset].reset();
    if (pageIsEmpty(*iterator->page))
        boundPages_.erase(iterator);
}

bool BoxDomain::pageIsEmpty(const BoundPage& page)
{
    return std::none_of(page.bounds.begin(), page.bounds.end(),
                        [](const auto& bound) { return bound.has_value(); });
}

std::vector<Variable> BoxDomain::boundedVariables() const
{
    std::vector<Variable> variables;
    for (const BoundPageEntry& entry : boundPages_)
    {
        for (std::size_t offset = 0; offset < BoundsPerPage; ++offset)
        {
            if (entry.page->bounds[offset])
            {
                variables.push_back(entry.page->bounds[offset]->variable);
            }
        }
    }
    return variables;
}

void BoxDomain::makeBottom()
{
    bottom_ = true;
    boundPages_.clear();
}

void BoxDomain::report(OperationKind operation, ApproximationKind approximation,
                       std::string reason, bool best) const
{
    recordOperation(operation, approximation, best, reason);
    if (config_.diagnostics)
        config_.diagnostics->report(
            {operation, approximation, std::move(reason)});
}

} // namespace SVF::AbstractDomain
