/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TYPES_HPP_INCLUDED
#define LIBCYPHAL_TYPES_HPP_INCLUDED

#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>
#include <cetl/pmr/interface_ptr.hpp>
#include <cetl/unbounded_variant.hpp>
#include <cetl/variable_length_array.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <ratio>
#include <type_traits>

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

};  // MonotonicClock

using TimePoint = MonotonicClock::time_point;
using Duration  = MonotonicClock::duration;

template <typename T>
using UniquePtr = cetl::pmr::InterfacePtr<T>;

// TODO: Maybe introduce `cetl::expected` at CETL repo.
template <typename Success, typename Failure>
using Expected = cetl::variant<Success, Failure>;

/// A generalized implementation of https://www.fluentcpp.com/2021/01/29/inheritance-without-pointers/
/// that works with any `cetl::unbounded_variant`.
///
/// The instance is always initialized with a valid value, but it may turn valueless if the value is moved.
/// The `Any` type can be a `cetl::unbounded_variant`.
///
template <typename Interface, typename Any>
class ImplementationCell final
{
public:
    template <typename Impl,
              typename ImplD = std::decay_t<Impl>,
              typename       = std::enable_if_t<std::is_base_of<Interface, ImplD>::value>>
    explicit ImplementationCell(Impl&& object)
        : any_(std::forward<Impl>(object))
        , fn_getter_mut_{[](Any& any) -> Interface* { return cetl::get_if<ImplD>(&any); }}
        , fn_getter_const_{[](const Any& any) -> const Interface* { return cetl::get_if<ImplD>(&any); }}
    {
    }

    Interface* operator->()
    {
        return fn_getter_mut_(any_);
    }

    const Interface* operator->() const
    {
        return fn_getter_const_(any_);
    }

    explicit operator bool() const
    {
        return any_.has_value();
    }

private:
    Any any_;
    Interface* (*fn_getter_mut_)(Any&);
    const Interface* (*fn_getter_const_)(const Any&);

};  // ImplementationCell

/// Internal implementation details.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

template <typename Concrete>
using PmrAllocator = cetl::pmr::polymorphic_allocator<Concrete>;

template <typename T>
using VarArray = cetl::VariableLengthArray<T, PmrAllocator<T>>;

template <typename I, typename C>
struct UniquePtrSpec
{
    using Interface = I;
    using Concrete  = C;
};

template <typename UniquePtrSpec, typename... Args>
CETL_NODISCARD UniquePtr<typename UniquePtrSpec::Interface> makeUniquePtr(cetl::pmr::memory_resource& memory,
                                                                          Args&&... args)
{
    return cetl::pmr::InterfaceFactory::make_unique<
        typename UniquePtrSpec::Interface>(PmrAllocator<typename UniquePtrSpec::Concrete>{&memory},
                                           std::forward<Args>(args)...);
}

/// @brief Makes a new variant type by appending set of additional types.
///
/// In use f.e. for extending an existing `Failure` type (which is `cetl::variant` of possible `Error`s)
/// with extra set of additional failure modes which are specific to the particular functionality.
///
/// @tparam T Type of the original variant.
/// @tparam Args Set of the additional types.
///
template <typename T, typename... Args>
struct AppendType;
//
template <typename... A, typename... B>
struct AppendType<cetl::variant<A...>, B...>
{
    using Result = cetl::variant<A..., B...>;

};  // AppendType

/// @brief Upcasts a variant type value to a new variant type with additional types.
///
/// @tparam UpVariant The destination variant type upcast to.
/// @tparam Variant The source variant type to be upcasted.
/// @param variant Value of the source variant type.
/// @return Value of the upcasted variant type.
///
template <typename UpVariant, typename Variant>
CETL_NODISCARD UpVariant upcastVariant(Variant&& variant)
{
    return cetl::visit([](auto&& value) -> UpVariant { return std::forward<decltype(value)>(value); },
                       std::forward<Variant>(variant));
}

/// @brief Wraps the given action into a lambda and performs it without throwing exceptions.
///
/// In use f.e. for `cetl::visit` which might hypothetically throw an exceptions.
///
/// @return `true` if the action was performed successfully, `false` if an exception was thrown.
///         Always `true` if exceptions are disabled.
///
template <typename Exception = std::exception, typename Action>
CETL_NODISCARD bool performWithoutThrowing(Action&& action) noexcept
{
#if defined(__cpp_exceptions)
    try
    {
#endif
        std::forward<Action>(action)();
        return true;
#if defined(__cpp_exceptions)
    } catch (const Exception& ex)
    {
        CETL_DEBUG_ASSERT(false, ex.what());
        return false;
    }
#endif
}

}  // namespace detail

/// @brief A deleter which uses Polymorphic Memory Resource (PMR) for de-allocation of raw bytes memory buffers.
///
/// Alignment of the memory buffers is expected to be the same as the PMR default (`alignof(std::max_align_t)`).
///
struct PmrRawBytesDeleter final
{
    /// @brief Constructs default deleter with zero size and no PMR resource attached.
    ///
    /// Suitable only as an initial "empty" state for the deleter, f.e. for a default-constructed `std::unique_ptr`.
    ///
    PmrRawBytesDeleter()
        : size_bytes_{0}
        , memory_resource_{nullptr}
    {
    }

    /// @brief Constructs the deleter with the given size of a memory buffer and the PMR resource.
    ///
    /// The same instance of deleter can be used for multiple memory buffers with the same size and PMR resource.
    ///
    /// @param size_bytes Size of the memory buffer in bytes. Could be zero.
    /// @param memory_resource PMR to be used for de-allocation of the memory buffer.
    ///                        Should be the same PMR which is used for memory buffer allocation.
    ///
    PmrRawBytesDeleter(const std::size_t size_bytes, cetl::pmr::memory_resource* const memory_resource)
        : size_bytes_{size_bytes}
        , memory_resource_{memory_resource}
    {
        CETL_DEBUG_ASSERT(memory_resource != nullptr, "Memory resource should not be nullptr.");
    }

    /// @brief Gets size of a memory buffer (in bytes) to be de-allocated by this deleter.
    ///
    std::size_t size() const noexcept
    {
        return size_bytes_;
    }

    /// @brief Gets PMR to be used for a memory buffer de-allocation.
    ///
    cetl::pmr::memory_resource* resource() const noexcept
    {
        return memory_resource_;
    }

    /// @brief De-allocates the memory buffer (if any) using the PMR resource (if available).
    ///
    /// Alignment of the memory buffers is expected to be `alignof(std::max_align_t)`.
    ///
    /// @param raw_bytes_ptr Pointer to the memory buffer to be de-allocated.
    ///                      The buffer size is expected to be the same as the size passed to the constructor.
    ///                      If `nullptr` is passed, no action is taken.
    ///
    void operator()(cetl::byte* const raw_bytes_ptr) const
    {
        CETL_DEBUG_ASSERT((nullptr != memory_resource_) || (nullptr == raw_bytes_ptr),
                          "Memory resource should not be `nullptr` in case of non-`nullptr` buffer.");

        if ((nullptr != memory_resource_) && (nullptr != raw_bytes_ptr))
        {
            CETL_DEBUG_ASSERT(isAligned(raw_bytes_ptr, size_bytes_), "Unexpected alignment of the memory buffer.");

            // No Sonar `cpp:S5356` b/c we integrate here with low level PMR management.
            memory_resource_->deallocate(raw_bytes_ptr, size_bytes_);  // NOSONAR:cpp:S5356
        }
    }

private:
    // No Sonar `cpp:S5356` b/c we check here low level PMR alignment expectation.
    static bool isAligned(cetl::byte* const raw_bytes_ptr, const std::size_t size)
    {
        std::size_t space = size;
        auto*       ptr   = static_cast<void*>(raw_bytes_ptr);                            // NOSONAR:cpp:S5356
        return raw_bytes_ptr == std::align(alignof(std::max_align_t), size, ptr, space);  // NOSONAR:cpp:S5356
    }

    // MARK: Data members:

    std::size_t                 size_bytes_;
    cetl::pmr::memory_resource* memory_resource_;

};  // PmrRawBytesDeleter

/// @brief Helper function which creates a new `Concrete` type object in a dynamically allocated memory from the given
/// Polymorphic Memory Resource (PMR). The object's pointer is wrapped into libcyphal compatible/expected `UniquePtr`.
/// The result `Interface`-typed smart-pointer (aka `std::unique_ptr`) has the concrete-type-erased PMR deleter.
///
/// Internally it uses `cetl::pmr::InterfaceFactory` factory which in turn uses:
/// - `PmrAllocator = cetl::pmr::polymorphic_allocator<Concrete>` as the PMR (de)allocator type;
/// - `cetl::pmr::PmrInterfaceDeleter<Interface>` as the concrete-type-erased PMR deleter type.
///
/// @tparam Interface The interface template type - should be base of `Concrete` template type.
///                   Such interface type could be without exposed destructor (protected or private).
/// @tparam Concrete The concrete type the object to be created. Its `~Concrete()` destructor have to be public.
///                  The result smart pointer deleter will use the destructor to de-initialize memory of the object.
/// @tparam Args     The types of arguments to be passed to the constructor of the `Concrete` object.
/// @param memory    The PMR resource to be used for memory allocation and de-allocation.
///                  NB! It's captured by reference inside of the deleter, so smart pointer should not outlive
///                  the PMR resource (or use `reset` to release the object earlier).
/// @param args      The arguments to be forwarded to the constructor of the `Concrete` object.
///
template <typename Interface, typename Concrete, typename... Args>
CETL_NODISCARD UniquePtr<Interface> makeUniquePtr(cetl::pmr::memory_resource& memory, Args&&... args)
{
    return cetl::pmr::InterfaceFactory::make_unique<Interface>(detail::PmrAllocator<Concrete>{&memory},
                                                               std::forward<Args>(args)...);
}

}  // namespace libcyphal

#endif  // LIBCYPHAL_TYPES_HPP_INCLUDED
