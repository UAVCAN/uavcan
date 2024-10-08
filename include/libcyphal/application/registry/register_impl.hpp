/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_APPLICATION_REGISTRY_REGISTER_IMPL_HPP_INCLUDED
#define LIBCYPHAL_APPLICATION_REGISTRY_REGISTER_IMPL_HPP_INCLUDED

#include "registry_impl.hpp"
#include "registry_value.hpp"

#include <cetl/cetl.hpp>

#include <utility>

namespace libcyphal
{
namespace application
{
namespace registry
{

/// Defines abstract base class for a register implementation.
///
/// Implements common functionality for all register types like name, options, and value accessors.
///
class RegisterBase : public IRegister
{
    using Base = IRegister;

public:
    /// Defines options used when creating a new register.
    ///
    struct Options final
    {
        /// True if the register value is retained across application restarts.
        bool persistent{false};

    };  // Options

    RegisterBase(const RegisterBase&)                      = delete;
    RegisterBase& operator=(const RegisterBase&)           = delete;
    RegisterBase& operator=(RegisterBase&& other) noexcept = delete;

    /// Gets the register create options.
    ///
    Options getOptions() const noexcept
    {
        return options_;
    }

    // MARK: IRegister

    Name getName() const override
    {
        return name_;
    }

protected:
    RegisterBase(cetl::pmr::memory_resource& memory, const Name name, const Options& options)
        : Base{name}
        , allocator_{&memory}
        , name_{name}
        , options_{options}
    {
    }

    ~RegisterBase()                       = default;
    RegisterBase(RegisterBase&&) noexcept = default;

    template <typename T>
    ValueAndFlags getImpl(const T& value, const bool is_mutable) const
    {
        ValueAndFlags out{Value{allocator_}, {}};
        registry::set(out.value_, value);
        out.flags_.mutable_    = is_mutable;
        out.flags_.persistent_ = options_.persistent;
        return out;
    }

    template <typename T, typename Setter>
    cetl::optional<SetError> setImpl(const T& value, Setter&& setter)
    {
        auto converted = get().value_;
        if (coerce(converted, value))
        {
            if (std::forward<Setter>(setter)(converted))
            {
                return cetl::nullopt;
            }
            return SetError::Semantics;
        }
        return SetError::Coercion;
    }

private:
    // MARK: Data members:

    Value::allocator_type allocator_;
    const Name            name_;
    const Options         options_;

};  // RegisterBase

// MARK: -

/// Defines a register implementation template.
///
template <typename Getter, typename Setter, bool IsMutable>
class Register;
//
/// Defines a read-only register implementation.
///
/// The actual value is provided by the getter function.
///
template <typename Getter>
class Register<Getter, void, false> final : public RegisterBase
{
    using Base = RegisterBase;

public:
    ~Register()                   = default;
    Register(Register&&) noexcept = default;

    Register(const Register&)                = delete;
    Register& operator=(const Register&)     = delete;
    Register& operator=(Register&&) noexcept = delete;

    // MARK: IRegister

    ValueAndFlags get() const override
    {
        return getImpl(getter_(), false);
    }

    cetl::optional<SetError> set(const Value&) override
    {
        return SetError::Mutability;
    }

private:
    friend class Registry;

    Register(cetl::pmr::memory_resource& memory, const Name name, const Options& options, Getter getter)
        : Base{memory, name, options}
        , getter_(std::move(getter))
    {
    }

    // MARK: Data members:

    Getter getter_;

};  // Register<IsMutable=false>
//
/// Defines a read-write register implementation.
///
/// The actual value is provided by the getter function,
/// and the setter function is used to update the value.
///
template <typename Getter, typename Setter>
class Register<Getter, Setter, true> final : public RegisterBase
{
    using Base = RegisterBase;

public:
    ~Register()                   = default;
    Register(Register&&) noexcept = default;

    Register(const Register&)                = delete;
    Register& operator=(const Register&)     = delete;
    Register& operator=(Register&&) noexcept = delete;

    // MARK: IRegister

    ValueAndFlags get() const override
    {
        return getImpl(getter_(), true);
    }

    cetl::optional<SetError> set(const Value& new_value) override
    {
        return setImpl(new_value, setter_);
    }

private:
    friend class Registry;

    Register(cetl::pmr::memory_resource& memory, const Name name, const Options& options, Getter getter, Setter setter)
        : Base{memory, name, options}
        , getter_{std::move(getter)}
        , setter_{std::move(setter)}
    {
    }

    // MARK: Data members:

    Getter getter_;
    Setter setter_;

};  // Register<IsMutable=true>

// MARK: -

/// Defines a parameter-based register implementation template.
///
/// Instead of "external" getter/setter functions, the register uses member value storage.
///
template <typename ValueType, bool IsMutable = true>
class ParamRegister final : public RegisterBase
{
public:
    /// Constructs a new detached register, which is not yet linked to any registry.
    ///
    /// A detached register must be appended to a registry before its value could be exposed by the registry.
    ///
    /// @param memory The memory resource to use for variable size-d register values.
    /// @param name The name of the register.
    /// @param default_value The initial default value of the register.
    /// @param options Extra options for the register, like "persistent" option (`true` by default).
    ///
    ParamRegister(cetl::pmr::memory_resource& memory,
                  const Name                  name,
                  const ValueType&            default_value,
                  const Options&              options = {true})
        : RegisterBase{memory, name, options}
        , value_{default_value}
    {
    }

    /// Constructs a new register, and links it to a given registry.
    ///
    /// Register will use PMR memory resource of the registry.
    ///
    /// @param rgy The registry to link the register to.
    /// @param name The name of the register. Should be unique within the registry.
    /// @param default_value The initial default value of the register.
    /// @param options Extra options for the register, like "persistent" option (`true` by default).
    ///
    ParamRegister(Registry& rgy, const Name name, const ValueType& default_value, const Options& options = {true})
        : RegisterBase{rgy.memory(), name, options}
        , value_{default_value}
    {
        const bool success = rgy.append(*this);
        CETL_DEBUG_ASSERT(success, "Register with the same name already exists.");
        (void) success;
    }

    ~ParamRegister()                        = default;
    ParamRegister(ParamRegister&&) noexcept = default;

    ParamRegister(const ParamRegister&)                = delete;
    ParamRegister& operator=(const ParamRegister&)     = delete;
    ParamRegister& operator=(ParamRegister&&) noexcept = delete;

    // MARK: IRegister

    ValueAndFlags get() const override
    {
        return getImpl(value_, IsMutable);
    }

    cetl::optional<SetError> set(const Value& new_value) override
    {
        if (!IsMutable)
        {
            return SetError::Mutability;
        }

        return setImpl(new_value, [this](const Value& value) {
            //
            value_ = registry::get<ValueType>(value).value();
            return true;
        });
    }

private:
    // MARK: Data members:

    ValueType value_;

};  // ParamRegister

}  // namespace registry
}  // namespace application
}  // namespace libcyphal

#endif  // LIBCYPHAL_APPLICATION_REGISTRY_REGISTER_IMPL_HPP_INCLUDED
