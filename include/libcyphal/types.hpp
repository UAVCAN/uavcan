/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TYPES_HPP_INCLUDED
#define LIBCYPHAL_TYPES_HPP_INCLUDED

#include <cetl/pf17/cetlpf.hpp>
#include <cetl/pf17/attribute.hpp>
#include <cetl/pf20/cetlpf.hpp>
#include <cetl/pmr/memory.hpp>

#include <cstdint>
#include <chrono>
#include <ratio>

namespace libcyphal
{

/// @brief The internal time representation is in microseconds.
///
/// This is in line with the lizards that use `uint64_t`-typed microsecond counters throughout.
///
struct MonotonicClock final
{
    using rep        = std::int64_t;
    using period     = std::micro;
    using duration   = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<MonotonicClock>;

    static constexpr bool is_steady = true;

    /// @brief Gets the current time point.
    ///
    /// Method is NOT implemented by the library; the user code is expected to provide a suitable implementation
    /// instead depending on the requirements of the application.
    /// A possible implementation on a POSIX-like platform is:
    /// ```
    /// MonotonicClock::time_point MonotonicClock::now() noexcept
    /// {
    ///     return std::chrono::time_point_cast<time_point>(std::chrono::steady_clock::now());
    /// }
    /// ```
    CETL_NODISCARD static time_point now() noexcept;

};  // MonotonicClock

using TimePoint = MonotonicClock::time_point;
using Duration  = MonotonicClock::duration;

template <typename T>
using UniquePtr = cetl::pmr::Factory::unique_ptr_t<cetl::pmr::polymorphic_allocator<T>>;

// TODO: Maybe introduce `cetl::expected` at CETL repo.
template <typename Success, typename Failure>
using Expected = cetl::variant<Success, Failure>;

}  // namespace libcyphal

#endif  // LIBCYPHAL_TYPES_HPP_INCLUDED