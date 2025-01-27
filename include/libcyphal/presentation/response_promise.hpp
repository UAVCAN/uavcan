/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_PRESENTATION_RESPONSE_PROMISE_HPP_INCLUDED
#define LIBCYPHAL_PRESENTATION_RESPONSE_PROMISE_HPP_INCLUDED

#include "client_impl.hpp"
#include "common_helpers.hpp"

#include "libcyphal/config.hpp"
#include "libcyphal/errors.hpp"
#include "libcyphal/transport/scattered_buffer.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>
#include <cetl/pmr/function.hpp>

#include <nunavut/support/serialization.hpp>

#include <cstddef>
#include <utility>

namespace libcyphal
{
namespace presentation
{

/// @brief Defines terminal 'expired' error state of the response promise.
///
/// See `response_deadline` parameter of the `Client::request` method,
/// or `setDeadline()` method of the promise itself.
///
struct ResponsePromiseExpired final
{
    /// Holds deadline of the expired (aka timed out) response waiting.
    TimePoint deadline;
};

/// @brief Defines terminal failure state of the raw (aka un-typed) response promise.
///
/// Raw response promise failure state could be only expired. In contrast see `ResponsePromiseFailure`,
/// where the set of possible failure states is extended with additional points of failures.
///
using RawResponsePromiseFailure = cetl::variant<  //
    ResponsePromiseExpired>;

/// @brief Defines terminal failure state of the strong-typed response promise.
///
/// In addition to the raw failure states, this type also includes possible memory allocation errors,
/// as well as errors from the `nunavut` library in case of response deserialization issues.
///
using ResponsePromiseFailure = libcyphal::detail::AppendType<  //
    RawResponsePromiseFailure,
    MemoryError,
    nunavut::support::Error>::Result;

/// @brief Defines internal base class for any concrete (final) response promise.
///
/// @tparam ResponsePayload Type of the response payload that the promise is supposed to handle. It's expected to be
///                         either a deserializable type (like a Nunavut tool generated service response struct),
///                         or `transport::ScatteredBuffer` for raw bytes (aka untyped) responses.
///
template <typename ResponsePayload, typename Failure>
class ResponsePromiseBase : public detail::SharedClient::CallbackNode
{
public:
    /// @brief Defines successful response and its metadata.
    ///
    struct Success
    {
        ResponsePayload              response;
        transport::ServiceRxMetadata metadata;
    };

    /// @brief Defines result of the promise.
    ///
    /// Could be either a successful received response, or final expired condition.
    ///
    using Result = Expected<Success, Failure>;

    /// @brief Umbrella type for various response callback entities.
    ///
    struct Callback
    {
        /// @brief Defines standard arguments for the response promise callback.
        ///
        struct Arg
        {
            /// Holds the result of the promise - ownership belongs to the client (the callback function),
            /// so it could be modified or moved somewhere else (f.e. into some other storage).
            Result&& result;

            /// Holds the approximate time when the callback was called. Useful for minimizing `now()` calls.
            TimePoint approx_now;
        };
        static constexpr auto FunctionSize = config::Presentation::ResponsePromiseBase_Callback_FunctionSize();
        using Function                     = cetl::pmr::function<void(const Arg& arg), FunctionSize>;
    };

    /// @brief Constructs a new promise by moving `other` promise into this one.
    ///
    /// Such operation not only moves current properties of the promise (like its request and deadline times),
    /// but also possible callback function and potentially already received (but not yet consumed) result value.
    ///
    ResponsePromiseBase(ResponsePromiseBase&& other) noexcept
        : CallbackNode{std::move(static_cast<CallbackNode&&>(other))}
        , shared_client_{std::exchange(other.shared_client_, nullptr)}
        , request_time_{other.request_time_}
        , callback_fn_{std::move(other.callback_fn_)}
        , opt_result_{std::move(other.opt_result_)}
    {
        CETL_DEBUG_ASSERT(shared_client_ != nullptr, "Not supposed to move construct from already moved `other`.");
        // No need to retain the moved object, as it is supposed to be already retained.
    }

    ResponsePromiseBase(const ResponsePromiseBase& other)                = delete;
    ResponsePromiseBase& operator=(const ResponsePromiseBase& other)     = delete;
    ResponsePromiseBase& operator=(ResponsePromiseBase&& other) noexcept = delete;

    /// @brief Tries to get (aka peek) a result value from this promise.
    ///
    /// @return Constant reference to the previously received and stored result value (if any);
    ///         `cetl::nullopt` if there is no result yet, or it was already consumed
    ///         by the `fetchResult` call (or callback invocation; see `setCallback`).
    ///
    const cetl::optional<Result>& getResult() const
    {
        // Just peek result value without consuming it (see `fetchResult` in contrast).
        return opt_result_;
    }

    /// @brief Tries to fetch a result value from the promise.
    ///
    /// In contrast to `getResult`, this method "consumes" result by moving its value (if any) out of the promise.
    /// So, only one fetch of non-null value is possible (either "success" or "expired").
    /// Consequent gets & fetches will return `cetl::nullopt`.
    ///
    /// This method is also in use for the callback invocation, so it is also consuming result (see `setCallback`).
    /// It means that get/fetch method is mutually exclusive with callback-based delivery method.
    ///
    cetl::optional<Result> fetchResult()
    {
        if (opt_result_)
        {
            auto result = std::move(*opt_result_);
            opt_result_.reset();
            return result;
        }
        return cetl::nullopt;
    }

    /// @return Gets the time when the request was initiated.
    ///
    /// Useful to track the request-response latency, f.e. for implementing custom timeout/deadline handling like:
    /// - by periodically polling result of the promise (using `getResult` or `fetchResult`)
    /// - by also checking that `time_provider.now() - promise.getRequestTime()` within some limits.
    ///
    /// More simple approach is based on `response_deadline` parameter of the `Client::request` method, as well as
    /// on the `setDeadline()` method of the promise itself - `Expired` result will be automatically delivered to
    /// the callback (if any) as soon as deadline is reached (the same with `getResult`/`fetchResult` when called).
    ///
    TimePoint getRequestTime() const noexcept
    {
        return request_time_;
    }

protected:
    ResponsePromiseBase(detail::SharedClient* const shared_client,
                        const transport::TransferId transfer_id,
                        const TimePoint             response_deadline)
        : CallbackNode{transfer_id, response_deadline}
        , shared_client_{shared_client}
        , request_time_{shared_client->now()}
    {
        CETL_DEBUG_ASSERT(shared_client_ != nullptr, "");
        shared_client_->retainCallbackNode(*this);
    }

    ~ResponsePromiseBase()
    {
        if (shared_client_ != nullptr)
        {
            shared_client_->releaseCallbackNode(*this);
        }
    }

    cetl::pmr::memory_resource& memory() const noexcept
    {
        CETL_DEBUG_ASSERT(shared_client_ != nullptr, "");
        return shared_client_->memory();
    }

    void acceptResult(Result&& result, const TimePoint approx_now)
    {
        CETL_DEBUG_ASSERT(!opt_result_, "Result already set.");

        if (callback_fn_)
        {
            // Release callback function after calling it.
            const auto local_callback_fn = std::exchange(callback_fn_, nullptr);

            const typename Callback::Arg arg{std::move(result), approx_now};
            local_callback_fn(arg);
            return;
        }

        (void) opt_result_.emplace(std::move(result));
    }

    void acceptNewCallback(typename Callback::Function&& callback_fn)
    {
        if (callback_fn)
        {
            // If we already have result then we don't need to store the callback,
            // as well as continue store the result - just call the callback with just fetched result value.
            //
            if (auto result = fetchResult())
            {
                const typename Callback::Arg arg{std::move(*result), shared_client_->now()};
                callback_fn(arg);
                return;
            }
        }

        callback_fn_ = std::move(callback_fn);
    }

    void acceptNewDeadline(const TimePoint deadline)
    {
        CETL_DEBUG_ASSERT(shared_client_ != nullptr, "");
        shared_client_->updateDeadlineOfTimeoutNode(*this, deadline);
    }

    // MARK: CallbackNode

    void onResponseTimeout(const TimePoint deadline, const TimePoint approx_now) override
    {
        acceptResult(ResponsePromiseExpired{deadline}, approx_now);
    }

private:
    // MARK: Data members:

    detail::SharedClient*       shared_client_;
    const TimePoint             request_time_;
    typename Callback::Function callback_fn_;
    cetl::optional<Result>      opt_result_;

};  // ResponsePromiseBase

// MARK: -

/// @brief Defines promise class of a strong-typed response.
///
/// @tparam Response Deserializable response type of the promise.
///
template <typename Response>
class ResponsePromise final : public ResponsePromiseBase<Response, ResponsePromiseFailure>
{
    using Base = ResponsePromiseBase<Response, ResponsePromiseFailure>;
    using Base::memory;
    using Base::acceptResult;
    using Base::acceptNewCallback;
    using Base::acceptNewDeadline;

public:
    using typename Base::Result;
    using typename Base::Success;
    using typename Base::Callback;

    /// @brief Sets the callback function for the promise.
    ///
    /// Will be called (once at most!) on either successful response reception or on the response timeout.
    /// The callback function will be immediately called (in context of this `set` method)
    /// if this promise already has a result (either "success" or "expired").
    /// There well be no callback invocation if the promise result was already consumed (by `fetchResult`
    /// or by the previous callback), or if this promise itself is already destructed.
    ///
    /// @param callback_fn The callback function to be invoked. It will be released immediately after the call.
    ///                    Use `nullptr` (or `{}`) to disable callback-based delivery.
    /// @return Reference to the promise itself (so you can chain several calls).
    ///
    ResponsePromise& setCallback(typename Callback::Function&& callback_fn)
    {
        acceptNewCallback(std::move(callback_fn));
        return *this;
    }

    /// @brief Sets new deadline for this response promise.
    ///
    /// Has no effect if the promise already has a result (either "success" or "expired").
    ///
    /// @param deadline The future time point when the promise should be considered as expired.
    ///                 Use `TimePoint::max()` to disable the deadline. Anything in the past (less than `now`)
    ///                 will expire the promise very soon (on the next scheduler run). Default (initial) deadline
    ///                 value is taken from the `response_deadline` parameter of the `Client::request` method,
    ///                 but user can change it at any time by calling this method.
    /// @return Reference to the promise itself (so you can chain several calls).
    ///
    ResponsePromise& setDeadline(const TimePoint deadline)
    {
        acceptNewDeadline(deadline);
        return *this;
    }

private:
    template <typename Request, typename Response_>
    friend class Client;
    using Base::Base;

    // MARK: CallbackNode

    void onResponseRxTransfer(transport::ServiceRxTransfer& transfer, const TimePoint approx_now) override
    {
        auto&   mr = memory();
        Success success{Response{typename Response::allocator_type{&mr}}, transfer.metadata};
        if (auto failure = detail::tryDeserializePayload(transfer.payload, mr, success.response))
        {
            cetl::visit(
                [this, approx_now](const auto& error) {
                    //
                    acceptResult(error, approx_now);
                },
                *failure);
            return;
        }

        acceptResult(std::move(success), approx_now);
    }

};  // ResponsePromise<Response>

// MARK: -

/// @brief Defines promise class of a raw (aka untyped) response.
///
template <>
class ResponsePromise<void> final : public ResponsePromiseBase<transport::ScatteredBuffer, RawResponsePromiseFailure>
{
    using Base = ResponsePromiseBase;

public:
    using Base::Result;
    using Base::Success;
    using Base::Callback;

    /// @brief Sets the callback function for the promise.
    ///
    /// Will be called (once at most!) on either successful response reception or on the response timeout.
    /// The callback function will be immediately called (in context of this `set` method)
    /// if this promise already has a result (either "success" or "expired").
    /// There well be no callback invocation if the promise result was already consumed (by `fetchResult`
    /// or by the previous callback), or if this promise itself is already destructed.
    ///
    /// @param callback_fn The callback function to be invoked. It will be released immediately after the call.
    ///                    Use `nullptr` (or `{}`) to disable callback-based delivery.
    /// @return Reference to the promise itself (so you can chain several calls).
    ///
    ResponsePromise& setCallback(typename Callback::Function&& callback_fn)
    {
        acceptNewCallback(std::move(callback_fn));
        return *this;
    }

    /// @brief Sets new deadline for this response promise.
    ///
    /// Has no effect if the promise already has a result (either "success" or "expired").
    ///
    /// @param deadline The future time point when the promise should be considered as expired.
    ///                 Use `TimePoint::max()` to disable the deadline. Anything in the past (less than `now`)
    ///                 will expire the promise very soon (on the next scheduler run). Default (initial) deadline
    ///                 value is taken from the `response_deadline` parameter of the `Client::request` method,
    ///                 but user can change it at any time by calling this method.
    /// @return Reference to the promise itself (so you can chain several calls).
    ///
    ResponsePromise& setDeadline(const TimePoint deadline)
    {
        acceptNewDeadline(deadline);
        return *this;
    }

private:
    friend class RawServiceClient;
    using Base::Base;

    // MARK: CallbackNode

    void onResponseRxTransfer(transport::ServiceRxTransfer& transfer, const TimePoint approx_now) override
    {
        acceptResult(Success{std::move(transfer.payload), transfer.metadata}, approx_now);
    }

};  // ResponsePromise<void>

}  // namespace presentation
}  // namespace libcyphal

#endif  // LIBCYPHAL_PRESENTATION_RESPONSE_PROMISE_HPP_INCLUDED
