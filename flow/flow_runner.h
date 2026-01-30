#ifndef LITE_FNDS_FLOW_RUNNER_H
#define LITE_FNDS_FLOW_RUNNER_H

#include <atomic>
#include <type_traits>
#include <utility>

#include "../memory/padded_t.h"
#include "../memory/lite_ptr.h"
#include "../task/task_wrapper.h"
#include "../utility/callable_wrapper.h"
#include "../utility/back_off.h"

#include "flow_def.h"
#include "flow_blueprint.h"

namespace lite_fnds {
    template <typename flow_bp>
    struct flow_runner;

    enum class runner_cancel {
        none,
        locked,
        hard,
        soft,
    };

    struct flow_controller {
        template <typename flow_bp>
        friend struct flow_runner;
    private:
        padded_t<std::atomic<runner_cancel>, CACHE_LINE_SIZE> state_{ runner_cancel::none };
        flow_async_cancel_handler_t cancel_handler{ cancel_stub };
        flow_async_notify_handler_dropped_t notify_handler_dropped{ drop_sub };
        flow_async_cancel_param_t cancel_param{ nullptr };

        // never ever call this
        runner_cancel lock_and_set_cancel_handler(flow_async_cancel_handler_t cancel_handler,
            flow_async_notify_handler_dropped_t notify_handler_dropped,
            flow_async_cancel_param_t param) noexcept
        {
            auto& state = state_.get();
            for (backoff_strategy<> backoff;; backoff.yield()) {
                runner_cancel exp = runner_cancel::none;
                if (state.compare_exchange_weak(exp, runner_cancel::locked, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }

                if (exp == runner_cancel::hard || exp == runner_cancel::soft) {
                    return exp;
                }
            }

            this->cancel_handler = cancel_handler;
            this->notify_handler_dropped = notify_handler_dropped;
            cancel_param = param;
            return runner_cancel::none;
        }

        // never ever call this
        void reset_cancel_handler_when_locked() noexcept {
            notify_handler_dropped(cancel_param);
            this->notify_handler_dropped = drop_sub;
            this->cancel_handler = cancel_stub;
            this->cancel_param = nullptr;
        }

        void unlock() noexcept {
            auto& state = state_.get();
            state.store(runner_cancel::none, std::memory_order_release);
        }

        void reset_cancel_handler() noexcept {
            auto& state = state_.get();
            for (backoff_strategy<> backoff;; backoff.yield()) {
                runner_cancel exp = runner_cancel::none;
                if (state.compare_exchange_weak(exp, runner_cancel::locked, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    notify_handler_dropped(cancel_param);
                    this->notify_handler_dropped = drop_sub;
                    this->cancel_handler = cancel_stub;
                    this->cancel_param = nullptr;
                    state.store(runner_cancel::none, std::memory_order_release);
                    break;
                }

                if (exp == runner_cancel::hard || exp == runner_cancel::soft) {
                    break;
                }
            }
        }

        static void cancel_stub(void*, cancel_kind) noexcept {};
        static void drop_sub(void*) noexcept {};

    public:
        ~flow_controller() noexcept {
            reset_cancel_handler();
        }

        void cancel(bool force = false) noexcept {
            const auto kind = force ? runner_cancel::hard : runner_cancel::soft;
            auto& state = state_.get();

            for (backoff_strategy<> backoff;; backoff.yield()) {
                runner_cancel exp = runner_cancel::none;
                if (state.compare_exchange_weak(exp, kind, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    cancel_handler(cancel_param, force ? cancel_kind::hard : cancel_kind::soft);

                    notify_handler_dropped(cancel_param);
                    this->notify_handler_dropped = drop_sub;
                    this->cancel_handler = cancel_stub;
                    this->cancel_param = nullptr;
                    break;
                }

                if (exp == runner_cancel::soft || exp == runner_cancel::hard) {
                    break;
                }
            }
        }

        bool is_force_canceled() const noexcept {
            auto& state = state_.get();
            return state.load(std::memory_order_acquire) == runner_cancel::hard;
        }

        bool is_soft_canceled() const noexcept {
            auto& state = state_.get();
            return state.load(std::memory_order_acquire) == runner_cancel::soft;
        }

        bool is_canceled() const noexcept {
            auto& state = state_.get();
            auto s = state.load(std::memory_order_acquire);
            return s == runner_cancel::soft || s == runner_cancel::hard;
        }
    };

    template <typename flow_bp>
    struct flow_runner {
        static_assert(flow_impl::is_blueprint_v<flow_bp>, "flow_bp must be a flow_blueprint");

        static constexpr std::size_t node_count = flow_bp::node_count;
        static_assert(node_count > 0, "attempting to run an empty blueprint");

        using bp_t = std::decay_t<flow_bp>;
        using I_t = typename bp_t::I_t;
        using O_t = typename bp_t::O_t;
        using storage_t = typename bp_t::storage_t;

        using first_node_t = flat_storage_element_t<0, storage_t>;
        static_assert(std::is_same<typename first_node_t::tag, flow_impl::node_tag_end>::value,
            "A valid blueprint must end with an end");

        using bp_ptr = lite_ptr<bp_t>;
        using controller_ptr = lite_ptr<flow_controller>;
    private:
        controller_ptr controller;
        bp_ptr bp;
    public:
        flow_runner() = delete;

        explicit flow_runner(bp_ptr bp_, controller_ptr ctrl = controller_ptr())
            : controller(ctrl ? std::move(ctrl) : make_lite_ptr<flow_controller>())
            , bp(std::move(bp_)) {
        }

        controller_ptr get_controller() const noexcept {
            return controller;
        }

        template <typename In,
            std::enable_if_t<std::is_convertible<In, typename I_t::value_type>::value>* = nullptr>
        void operator()(In&& in) noexcept {
            if (!bp) {
                return;
            }
            ipc<node_count - 1>::run(*this, I_t(value_tag, std::forward<In>(in)));
        }
    private:
        template <std::size_t I>
        struct ipc {
            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void run(flow_runner& self, param_t&& in) noexcept {
                using node_t = flat_storage_element_t<I, storage_t>;
                using node_i_t = typename node_t::I_t;
                using error_type = typename node_i_t::error_type;

                UNLIKELY_IF(self.controller->is_force_canceled()) {
                    using end_node_t = flat_storage_element_t<0, storage_t>;
                    using end_in_t = typename end_node_t::I_t;
                    using end_err_t = typename end_in_t::error_type;
                    ipc<0>::run(self, end_in_t(error_tag, cancel_error<end_err_t>::make(cancel_kind::hard)));
                    return;
                }

                auto& node = get<I>(self.bp->nodes_);
                UNLIKELY_IF(self.controller->is_soft_canceled()) {
                    dispatch(node, self, node_i_t(error_tag, cancel_error<error_type>::make(cancel_kind::soft)), typename node_t::tag{}, std::true_type{});
                    return;
                }

                dispatch(node, self, std::forward<param_t>(in), typename node_t::tag{}, std::false_type{});
            }

            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ == 0>* = nullptr>
            static void run(flow_runner& self, param_t&& param) noexcept {
                get<0>(self.bp->nodes_).f(std::forward<param_t>(param));
            }

        private:
            template <typename node_t, typename param_t, typename canceled, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_runner& self, param_t&& in, flow_impl::node_tag_calc, canceled) noexcept {
                ipc<I - 1>::run(self, node.f(std::forward<param_t>(in)));
            }

            template <typename node_t, typename param_t, typename canceled, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_runner& self, param_t&& in, flow_impl::node_tag_via, canceled) noexcept {
                node.p(task_wrapper_sbo([bp = self.bp,
                    controller = self.controller,
                    in = std::forward<param_t>(in)]() mutable noexcept {
                        flow_runner next_runner(std::move(bp), std::move(controller));
                        ipc<I - 1>::run(next_runner, std::move(in));
                    }));
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_runner& self, param_t&& in, flow_impl::node_tag_async, std::true_type /*canceled?*/) noexcept {
                using node_output_t = typename node_t::O_t;
                node.data.first()(task_wrapper_sbo([bp = self.bp,
                    controller = self.controller]() mutable noexcept {
                        flow_runner next_runner(std::move(bp), std::move(controller));
                        ipc<I - 1>::run(next_runner, node_output_t(error_tag, cancel_error<typename node_output_t::error_type>::make(cancel_kind::soft)));
                    }));
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_runner& self, param_t&& in, flow_impl::node_tag_async, std::false_type /*canceled?*/) noexcept {
                auto awaitable_or_error = node.data.second()(std::forward<param_t>(in));

                using node_output_t = typename node_t::O_t;
                // failed to create the awaitable
                if (!awaitable_or_error.has_value()) {
                    node.data.first()(task_wrapper_sbo([bp = self.bp,
                        controller = self.controller,
                        err = std::move(awaitable_or_error.error())]() mutable noexcept {
                            flow_runner next_runner(std::move(bp), std::move(controller));
                            ipc<I - 1>::run(next_runner, node_output_t(error_tag, std::move(err)));
                        }));
                    return;
                }

                // awaitable is successfully created
                auto awaitable = awaitable_or_error.value();
                flow_async_cancel_handler_t cancel_handler = nullptr;
                flow_async_cancel_param_t param = nullptr;
                flow_async_notify_handler_dropped_t notify_dropped = nullptr;
                awaitable.provide_cancel_handler(&cancel_handler, &notify_dropped, &param);

                auto state = self.controller->lock_and_set_cancel_handler(cancel_handler, notify_dropped, param);
                UNLIKELY_IF(state != runner_cancel::none) {
                    notify_dropped(param);
                    auto cancel_type = self.controller->is_soft_canceled() ? cancel_kind::soft : cancel_kind::hard;
                    // operation is canceled
                    awaitable.release();
                    node.data.first()(task_wrapper_sbo([bp = self.bp,
                        controller = self.controller,
                        cancel_type = cancel_type]() mutable noexcept {
                            flow_runner next_runner(std::move(bp), std::move(controller));
                            ipc<I - 1>::run(next_runner, node_output_t(error_tag, cancel_error<typename node_output_t::error_type>::make(cancel_type)));
                        }));
                    return;
                }

                // ok prepare to submit asio
                struct guard {
                    flow_controller* controller;
                    ~guard() noexcept {
                        if (controller) controller->unlock();
                    }
                };

                guard g{ self.controller.get() };
                using next_t = callable_wrapper<void(node_output_t&&)>;
                awaitable.emplace_nextstep(next_t([bp = self.bp,
                                                   go = node.data.first(),
                                                   controller = self.controller](node_output_t&& in) {
                        go(task_wrapper_sbo([bp = bp,
                            controller = controller,
                            in = std::move(in)]() mutable noexcept {
                                controller->reset_cancel_handler();
                                flow_runner next_runner(std::move(bp), std::move(controller));
                                ipc<I - 1>::run(next_runner, std::move(in));
                          })
                        );
                    }
                ));

                // failed to submit the io.
                UNLIKELY_IF(awaitable.submit_async() != 0) {
                    self.controller->reset_cancel_handler_when_locked();
                    g.controller->unlock();
                    g.controller = nullptr;

                    awaitable.release();
                    node.data.first()(task_wrapper_sbo([bp = self.bp,
                                                        controller = self.controller]() mutable noexcept {
                            flow_runner next_runner(std::move(bp), std::move(controller));
                            ipc<I - 1>::run(next_runner,
                            node_output_t(error_tag, async_submission_failed_error<typename node_output_t::error_type>::make()));
                        }
                    ));
                }
            }
        };
    };

    template <typename I_t, typename O_t, typename... Nodes>
    auto make_runner(lite_ptr<flow_impl::flow_blueprint<I_t, O_t, Nodes...>> bp,
        lite_ptr<flow_controller> ctrl = {}) noexcept {
        return flow_runner<flow_impl::flow_blueprint<I_t, O_t, Nodes...>>(std::move(bp), std::move(ctrl));
    }

    // one-short runner.
    namespace fast_runner_impl {
        template <typename flow_bp>
        struct bp_storage {
            static_assert(flow_impl::is_blueprint_v<flow_bp>, "flow_bp must be a flow_blueprint");
            static_assert(sizeof(flow_bp) == 0, "a flat bp storage should have a raw pointer or a unique_ptr or a shared_ptr or a reference.");
        };

        template <typename flow_bp>
        struct bp_storage <flow_bp*> {
            static_assert(flow_impl::is_blueprint_v<flow_bp>, "flow_bp must be a flow_blueprint");

            static constexpr std::size_t node_count = flow_bp::node_count;
            static_assert(node_count > 0, "attempting to run an empty blueprint");

            using bp_t = std::decay_t<flow_bp>;
            using I_t = typename bp_t::I_t;
            using O_t = typename bp_t::O_t;
            using storage_t = typename bp_t::storage_t;

            flow_bp* p;
            explicit bp_storage(flow_bp* p_)
                noexcept : p(p_) {
            }

            inline flow_bp* operator->() const {
                return p;
            }

            explicit operator bool() const noexcept {
                return p;
            }
        };

        template <typename flow_bp>
        struct bp_storage <lite_ptr<flow_bp>> {
            static_assert(flow_impl::is_blueprint_v<flow_bp>, "flow_bp must be a flow_blueprint");

            static constexpr std::size_t node_count = flow_bp::node_count;
            static_assert(node_count > 0, "attempting to run an empty blueprint");

            using bp_t = std::decay_t<flow_bp>;
            using I_t = typename bp_t::I_t;
            using O_t = typename bp_t::O_t;
            using storage_t = typename bp_t::storage_t;

            lite_ptr<flow_bp> p;
            explicit bp_storage(lite_ptr<flow_bp> p_)
                noexcept : p(std::move(p_)) {
            }

            FORCE_INLINE flow_bp* operator->() const {
                return p.get();
            }

            explicit operator bool() const noexcept {
                return p;
            }
        };

        template <typename flow_bp>
        struct bp_storage <flow_bp&> {
            static_assert(flow_impl::is_blueprint_v<flow_bp>, "flow_bp must be a flow_blueprint");

            static constexpr std::size_t node_count = flow_bp::node_count;
            static_assert(node_count > 0, "attempting to run an empty blueprint");

            using bp_t = std::decay_t<flow_bp>;
            using I_t = typename bp_t::I_t;
            using O_t = typename bp_t::O_t;
            using storage_t = typename bp_t::storage_t;

            flow_bp& p;

            explicit bp_storage(flow_bp& p_)
                noexcept : p(p_) {
            }

            FORCE_INLINE flow_bp* operator->() const {
                return &p;
            }

            explicit operator bool() const noexcept {
                return true;
            }
        };

        template <typename flow_bp>
        struct bp_storage <flow_bp&&> {
            static_assert(flow_impl::is_blueprint_v<flow_bp>, "flow_bp must be a flow_blueprint");

            static constexpr std::size_t node_count = flow_bp::node_count;
            static_assert(node_count > 0, "attempting to run an empty blueprint");

            using bp_t = std::decay_t<flow_bp>;
            using I_t = typename bp_t::I_t;
            using O_t = typename bp_t::O_t;
            using storage_t = typename bp_t::storage_t;

            lite_ptr<flow_bp> p;

            explicit bp_storage(flow_bp&& p_)
                : p(make_lite_ptr<flow_bp>(std::move(p_))) {
            }

            FORCE_INLINE flow_bp* operator->() const {
                return p.get();
            }

            explicit operator bool() const noexcept {
                return static_cast<bool>(p);
            }
        };
    }

    template <typename flow_bp_storage>
    struct flow_fast_runner {
        static constexpr std::size_t node_count = flow_bp_storage::node_count;
        static_assert(node_count > 0, "attempting to run a empty blueprint");

        using bp_t = typename flow_bp_storage::bp_t;
        using I_t = typename bp_t::I_t;

        using first_node_t = flat_storage_element_t<0, typename bp_t::storage_t>;
        static_assert(std::is_same<typename first_node_t::tag, flow_impl::node_tag_end>::value,
            "A valid blueprint must end with an end");

        using storage_t = typename bp_t::storage_t;
    private:
        flow_bp_storage bp;
    public:
        flow_fast_runner() = delete;

        explicit flow_fast_runner(flow_bp_storage&& bp_)
            noexcept(std::is_nothrow_move_constructible<flow_bp_storage>::value)
            : bp(std::move(bp_)) {
        }

        template <typename In,
            std::enable_if_t<std::is_convertible<In, typename I_t::value_type>::value>* = nullptr>
        void operator()(In&& in) noexcept {
            ipc<node_count - 1>::run(*this, I_t(value_tag, std::forward<In>(in)));
        }
    private:
        template <std::size_t I>
        struct ipc {
            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void run(flow_fast_runner& self, param_t&& in) noexcept {
                using node_t = flat_storage_element_t<I, storage_t>;
                dispatch(get<I>(self.bp->nodes_), self, std::forward<param_t>(in), typename node_t::tag{});
            }

            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ == 0>* = nullptr>
            static void run(flow_fast_runner& self, param_t&& param) noexcept {
                get<0>(self.bp->nodes_).f(std::forward<param_t>(param));
            }
        private:
            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_fast_runner& self, param_t&& in, flow_impl::node_tag_calc) noexcept {
                ipc<I - 1>::run(self, node.f(std::forward<param_t>(in)));
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_fast_runner& self, param_t&& in, flow_impl::node_tag_via) noexcept {
                node.p(task_wrapper_sbo([bp = std::move(self.bp),
                    in = std::forward<param_t>(in)]() mutable noexcept {
                        flow_fast_runner next_runner(std::move(bp));
                        ipc<I - 1>::run(next_runner, std::move(in));
                    }));
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_fast_runner& self, param_t&& in, flow_impl::node_tag_async) noexcept {
                auto awaitable_or_err = node.data.second()(std::forward<param_t>(in));

                using node_output_t = typename node_t::O_t;
                if (!awaitable_or_err.has_value()) {
                    // failed to create the awaitable
                    node.data.first()(task_wrapper_sbo([bp = std::move(self.bp),
                        err = std::move(awaitable_or_err.error())]() mutable noexcept {
                            flow_fast_runner next_runner(std::move(bp));
                            ipc<I - 1>::run(next_runner, node_output_t(error_tag, std::move(err)));
                        }));
                    return;
                }

                // ok prepare to submit asio
                auto bp = self.bp;

                auto awaitable = awaitable_or_err.value();
                using next_t = callable_wrapper<void(node_output_t&&)>;
                awaitable.emplace_nextstep(next_t([bp = std::move(self.bp),
                    go = node.data.first()](node_output_t&& in) {
                        go(task_wrapper_sbo([bp = std::move(bp), in = std::move(in)]() mutable noexcept {
                            flow_fast_runner next_runner(std::move(bp));
                            ipc<I - 1>::run(next_runner, std::move(in));
                            }));
                    }));

                // failed to submit the io.
                UNLIKELY_IF(awaitable.submit_async() != 0) {
                    awaitable.release();
                    node.data.first()(task_wrapper_sbo([bp = std::move(bp)]() mutable noexcept {
                        flow_fast_runner next_runner(std::move(bp));
                        ipc<I - 1>::run(next_runner,
                            node_output_t(error_tag, async_submission_failed_error<typename node_output_t::error_type>::make()));
                        }));
                }
            }
        };
    };

    template <typename bp_t>
    auto make_fast_runner(std::add_rvalue_reference_t<std::decay_t<bp_t>> bp) noexcept {
        using bp_storage = fast_runner_impl::bp_storage<std::add_rvalue_reference_t<std::decay_t<bp_t>>>;
        return flow_fast_runner<bp_storage>(bp_storage(std::move(bp)));
    }

    template <typename bp_t>
    auto make_fast_runner_view(bp_t* bp) noexcept {
        using bp_storage = fast_runner_impl::bp_storage<bp_t*>;
        return flow_fast_runner<bp_storage>(bp_storage(bp));
    }

    template <typename bp_t>
    auto make_fast_runner_view(bp_t& bp) noexcept {
        using bp_storage = fast_runner_impl::bp_storage<bp_t&>;
        return flow_fast_runner<bp_storage>(bp_storage(bp));
    }

    template <typename bp_t>
    auto make_fast_runner(lite_ptr<bp_t> bp) noexcept {
        using bp_storage = fast_runner_impl::bp_storage<lite_ptr<bp_t>>;
        return flow_fast_runner<bp_storage>(bp_storage(bp));
    }
} // namespace lite_fnds

#endif // LITE_FNDS_FLOW_RUNNER_H