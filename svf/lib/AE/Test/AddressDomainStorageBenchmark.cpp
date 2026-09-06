//===- AddressDomainStorageBenchmark.cpp -- Address storage benchmark ----===//

#include "AE/Core/AddressDomain.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace AD = SVF::AbstractDomain;

namespace
{
volatile std::uint64_t observation = 0;

template <typename Operation>
double measure(std::size_t iterations, Operation operation)
{
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration)
        operation(iteration);
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - begin).count() /
           static_cast<double>(iterations);
}

AD::AddressSet makeSet(std::size_t size, std::uint32_t offset = 1)
{
    AD::AddressSet result = AD::AddressSet::bottom();
    for (std::size_t index = 0; index < size; ++index)
        result.insert(AD::Location(offset + static_cast<std::uint32_t>(index)));
    return result;
}

AD::AddressDomain makeDomain(std::size_t size, std::uint32_t stride)
{
    AD::AddressDomain result = AD::AddressDomain::top();
    for (std::size_t index = 0; index < size; ++index)
        result.assign(
            AD::Variable(1 + static_cast<std::uint32_t>(index) * stride),
            AD::AddressSet::singleton(
                AD::Location(1 + static_cast<std::uint32_t>(index))));
    return result;
}

void report(const char* family, const std::string& operation,
            std::size_t size, std::uint32_t stride, double nanoseconds)
{
    std::cout << family << ',' << operation << ',' << size << ',' << stride
              << ',' << std::fixed << std::setprecision(3) << nanoseconds
              << '\n';
}

void benchmarkAddressSets(std::size_t scale)
{
    for (std::size_t size : {1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U})
    {
        const AD::AddressSet base = makeSet(size);
        const AD::AddressSet overlap =
            makeSet(size, static_cast<std::uint32_t>(size / 2 + 1));
        const std::size_t iterations = std::max<std::size_t>(1000, scale / size);
        report("set", "contains-hit", size, 1,
               measure(iterations, [&](std::size_t iteration) {
                   observation += base.contains(AD::Location(
                       1 + static_cast<std::uint32_t>(iteration % size)));
               }));
        report("set", "contains-miss", size, 1,
               measure(iterations, [&](std::size_t iteration) {
                   observation += base.contains(AD::Location(
                       1000000 + static_cast<std::uint32_t>(iteration)));
               }));
        report("set", "copy-insert", size, 1,
               measure(iterations, [&](std::size_t iteration) {
                   AD::AddressSet copy = base;
                   copy.insert(AD::Location(
                       1000000 + static_cast<std::uint32_t>(iteration)));
                   observation += copy.size();
               }));
        report("set", "join-overlap", size, 1,
               measure(iterations, [&](std::size_t) {
                   AD::AddressSet copy = base;
                   copy.joinWith(overlap);
                   observation += copy.size();
               }));
        report("set", "meet-overlap", size, 1,
               measure(iterations, [&](std::size_t) {
                   AD::AddressSet copy = base;
                   copy.meetWith(overlap);
                   observation += copy.size();
               }));
    }
}

void benchmarkDomains(std::size_t scale)
{
    for (std::uint32_t stride : {1U, 97U})
    {
        for (std::size_t size : {1U, 4U, 16U, 64U, 1024U, 16384U})
        {
            const AD::AddressDomain base = makeDomain(size, stride);
            const AD::AddressDomain other = makeDomain(size, stride);
            const std::size_t iterations =
                std::max<std::size_t>(100, scale / std::max<std::size_t>(1, size));
            report("domain", "lookup-hit", size, stride,
                   measure(iterations, [&](std::size_t iteration) {
                       const std::uint32_t index =
                           static_cast<std::uint32_t>(iteration % size);
                       observation +=
                           base.addressSet(AD::Variable(1 + index * stride))
                               .isSingleton();
                   }));
            report("domain", "lookup-miss", size, stride,
                   measure(iterations, [&](std::size_t iteration) {
                       observation += base.addressSet(AD::Variable(
                           2 + static_cast<std::uint32_t>(iteration % size) *
                                   stride))
                                          .isTop();
                   }));
            report("domain", "copy-mutate", size, stride,
                   measure(iterations, [&](std::size_t iteration) {
                       AD::AddressDomain copy = base;
                       const std::uint32_t index =
                           static_cast<std::uint32_t>(iteration % size);
                       copy.assign(
                           AD::Variable(1 + index * stride),
                           AD::AddressSet::singleton(AD::Location(
                               1000000 + static_cast<std::uint32_t>(iteration))));
                       observation += copy.addressSet(
                                              AD::Variable(1 + index * stride))
                                          .isSingleton();
                   }));
            AD::AddressDomain unique = makeDomain(size, stride);
            report("domain", "unique-mutate", size, stride,
                   measure(iterations, [&](std::size_t iteration) {
                       const std::uint32_t index =
                           static_cast<std::uint32_t>(iteration % size);
                       unique.assign(
                           AD::Variable(1 + index * stride),
                           AD::AddressSet::singleton(AD::Location(
                               2000000 + static_cast<std::uint32_t>(iteration))));
                       observation += unique.addressSet(
                                              AD::Variable(1 + index * stride))
                                          .isSingleton();
                   }));
            if (size <= 1024)
                report("domain", "copy-join", size, stride,
                       measure(iterations, [&](std::size_t) {
                           AD::AddressDomain copy = base;
                           copy.joinWith(other);
                           observation +=
                               copy.addressSet(AD::Variable(1)).isSingleton();
                       }));
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    const std::size_t scale =
        argc == 2 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10))
                  : 1000000;
    if (scale == 0)
        return EXIT_FAILURE;
    std::cout << "sizeof_address_set," << sizeof(AD::AddressSet) << '\n'
              << "sizeof_address_domain," << sizeof(AD::AddressDomain) << '\n'
              << "family,operation,size,stride,nanoseconds\n";
    benchmarkAddressSets(scale);
    benchmarkDomains(scale);
    std::cerr << "observation=" << observation << '\n';
    return EXIT_SUCCESS;
}
