#ifndef FLUX_FOUNDRY_FLOW_RUNNER_H
#define FLUX_FOUNDRY_FLOW_RUNNER_H

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

namespace flux_foundry {
    namespace flow_impl {
        template <typename T, typename D = void>
        struct check_receiver : std::false_type {};

        template <typename T>
        struct check_receiver<T,
            void_t<typename T::value_type,
                std::enable_if_t<
                    conjunction_v<
                        std::is_nothrow_move_constructible<T>,
                        std::is_nothrow_copy_constructible<T>,
                        is_result_t<typename T::value_type>
                    >
                >
            >
        > : std::true_type { };

        template <typename T, typename R>
        struct check_receiver_compatible {
            template <typename ...>
            static auto check(...) -> std::false_type;


            template <typename T_, typename R_>
            static auto check(int) -> std::integral_constant<bool,
                noexcept(std::declval<R_&>().emplace(std::declval<T_>()))>;

            constexpr static bool value = decltype(check<T, R>(0))::value;
        };

        template <typename T>
        struct stub_receiver {
            using value_type = T;
            void emplace(T&& val) noexcept { }
        };

        template <typename T>
        struct is_stub_receiver : std::false_type {};

        template <typename T>
        struct is_stub_receiver <stub_receiver<T>> : std::true_type {};

        template <typename T>
        constexpr bool is_stub_receiver_v = is_stub_receiver<T>::value;

        template <typename T, typename = void>
        struct awaitable_supports_cancel : std::true_type {};

        template <typename T>
        struct awaitable_supports_cancel<T, void_t<decltype(T::support_cancel)>>
            : std::integral_constant<bool, static_cast<bool>(T::support_cancel)> {};

        template <typename T>
        constexpr bool awaitable_supports_cancel_v = awaitable_supports_cancel<T>::value;
    }

    struct flow_controller;

    template <typename flow_bp, typename receiver_t, typename controller_ptr_t>
    struct flow_runner;

    enum runner_cancel {
        none,   // 0
        hard,   // 1
        soft,   // 2
        locked, // 3
        msk = locked,
        epoch = msk + 1,
    };

#if FLUEX_FOUNDRY_FLOW_CONTROLLER_CACHE_ALIGN
    struct alignas(CACHE_LINE_SIZE) flow_controller {
#else
    struct flow_controller {
#endif
        template <typename flow_bp, typename receiver_t, typename controller_ptr_t>
        friend struct flow_runner;
    private:
        padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> state_{ runner_cancel::none };
        detail::flow_async_cancel_handler_t cancel_handler{ cancel_stub };
        detail::flow_async_notify_handler_dropped_t notify_handler_dropped { drop_sub };
        detail::flow_async_cancel_param_t cancel_param { nullptr };

        // never ever call this
        auto lock_and_set_cancel_handler(detail::flow_async_cancel_handler_t new_cancel_handler,
            detail::flow_async_notify_handler_dropped_t new_notify_handler_dropped,
            detail::flow_async_cancel_param_t param) noexcept
        {
            auto& state = state_.get();

            auto exp = state.load(std::memory_order_acquire);
            if (exp & runner_cancel::msk) {
                return exp;
            }

            for (backoff_strategy<> backoff;; backoff.yield()) {
                if (state.compare_exchange_weak(exp, exp | runner_cancel::locked,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }

                if (exp & runner_cancel::msk) {
                    return exp;
                }
            }

            this->cancel_handler = new_cancel_handler;
            this->notify_handler_dropped = new_notify_handler_dropped;
            this->cancel_param = param;
            return exp | runner_cancel::locked;
        }

        // never ever call this
        void reset_cancel_handler_when_locked() noexcept {
            notify_handler_dropped(cancel_param);
            this->notify_handler_dropped = drop_sub;
            this->cancel_handler = cancel_stub;
            this->cancel_param = nullptr;
        }

        void unlock(size_t token) noexcept {
            auto& state = state_.get();
            state.compare_exchange_strong(token, token + 1,
                std::memory_order_release, std::memory_order_relaxed);
        }

        auto reset_cancel_handler() noexcept {
            auto& state = state_.get();

            auto exp = state.load(std::memory_order_acquire);
            if (exp & runner_cancel::msk) {
                return exp;
            }

            for (backoff_strategy<> backoff;; backoff.yield()) {
                if (state.compare_exchange_weak(exp, exp | runner_cancel::locked,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                    notify_handler_dropped(cancel_param);
                    this->notify_handler_dropped = drop_sub;
                    this->cancel_handler = cancel_stub;
                    this->cancel_param = nullptr;
                    state.store(exp + epoch, std::memory_order_release);
                    return exp;
                }

                if (exp & runner_cancel::msk) {
                    return exp;
                }
            }
        }

        static void cancel_stub(void*, cancel_kind) noexcept {};
        static void drop_sub(void*) noexcept {};

    public:
        // cancel() is thread-safe and may be called from external threads.
        // Internal handler/state transitions are coordinated with runner via lock bits + epoch.
        ~flow_controller() noexcept {
            reset_cancel_handler();
        }

        void cancel(bool force = false) noexcept {
            const auto kind = force ? runner_cancel::hard : runner_cancel::soft;
            auto& state = state_.get();

            auto exp = state.load(std::memory_order_acquire);
            if ((exp & runner_cancel::msk) == runner_cancel::soft 
                || (exp & runner_cancel::msk) == runner_cancel::hard) {
                return;
            }

            for (backoff_strategy<> backoff;; backoff.yield()) {
                exp &= ~size_t(runner_cancel::msk);
                auto target = exp | kind;
                if (state.compare_exchange_weak(exp, target, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    cancel_handler(cancel_param, force ? cancel_kind::hard : cancel_kind::soft);
                    notify_handler_dropped(cancel_param);
                    this->notify_handler_dropped = drop_sub;
                    this->cancel_handler = cancel_stub;
                    this->cancel_param = nullptr;
                    break;
                }

                if ((exp & runner_cancel::msk) == runner_cancel::soft 
                    || (exp & runner_cancel::msk) == runner_cancel::hard) {
                    return;
                }
            }
        }

        bool is_force_canceled() const noexcept {
            auto& state = state_.get();
            return (state.load(std::memory_order_acquire) & runner_cancel::msk) == runner_cancel::hard;
        }

        bool is_soft_canceled() const noexcept {
            auto& state = state_.get();
            return (state.load(std::memory_order_acquire) & runner_cancel::msk) == runner_cancel::soft;
        }

        bool is_canceled() const noexcept {
            auto& state = state_.get();
            auto s = state.load(std::memory_order_acquire) & runner_cancel::msk;
            return s == runner_cancel::soft || s == runner_cancel::hard;
        }
    };

    // Concurrency contract:
    // - flow_runner object is NOT thread-safe.
    // - do not call operator() concurrently on the same runner instance.
    // - async/via steps transfer execution by move-capturing runner state into continuations.
    // - flow_controller::cancel() may be invoked concurrently from other threads.
    template <typename flow_bp, typename receiver_type, typename controller_ptr_t = lite_ptr<flow_controller>>
    struct flow_runner {
        static constexpr std::size_t node_count = flow_bp::node_count;
        static_assert(node_count > 0, "attempting to run an empty blueprint");

        using bp_t = std::decay_t<flow_bp>;
        using receiver_t = std::decay_t<receiver_type>;

        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        static_assert(flow_impl::check_receiver<receiver_t>::value,
            "a valid receiver should:\n"
            "1. be nothrow move constructible.\n"
            "2. be nothrow copy constructible.\n"
            "in order to fully enable non-alloc in pipeline running, please make your receiver shared handle");
        static_assert(flow_impl::check_receiver_compatible<typename bp_t::O_t, receiver_t>::value,
            "the provided receiver isn't compatible with the current bp's output, A valid receiver should: "
            "1. has member:: typename value_type, which should be a result_t<T, E>, represents the result it receives\n"
            "2. has member function, whose signature is [ void emplace(result_t<T, E>&&) noexcept ]\n");

        using I_t = typename bp_t::I_t;
        using O_t = typename bp_t::O_t;
        using storage_t = typename bp_t::storage_t;

        using first_node_t = flat_storage_element_t<0, storage_t>;
        static_assert(std::is_same<typename first_node_t::tag, flow_impl::node_tag_end>::value,
            "A valid blueprint must end with an end");

        using bp_ptr = lite_ptr<bp_t>;
        using controller_ptr = std::decay_t<controller_ptr_t>;

    private:
        compressed_pair<bp_ptr, receiver_t> data;
        controller_ptr controller;

        bp_ptr& bp() noexcept {
            return data.first();
        }

        receiver_t& receiver() noexcept {
            return data.second();
        }
    public:
        flow_runner() = delete;

        template <typename R = receiver_t, std::enable_if_t<flow_impl::is_stub_receiver_v<R>>* = nullptr>
        explicit flow_runner(bp_ptr bp_, controller_ptr controller_, receiver_t receiver_ = receiver_t()) noexcept
            : data(std::move(bp_), std::move(receiver_)), controller(std::move(controller_)) {
        }

        template <typename R = receiver_t, std::enable_if_t<!flow_impl::is_stub_receiver_v<R>>* = nullptr>
        explicit flow_runner(bp_ptr bp_, controller_ptr controller_, receiver_t receiver_) 
            noexcept(std::is_nothrow_move_constructible<receiver_t>::value)
            : data(std::move(bp_), std::move(receiver_)), controller(std::move(controller_)) {
        }
        
        // only call this after you start a runner(calling operator())
        // this will return the controller for the THIS run.
        controller_ptr get_controller() const noexcept {
            return controller;
        }

        template <typename ... Args,
            std::enable_if_t<std::is_constructible<typename I_t::value_type, Args&& ...>::value>* = nullptr>
        void operator()(Args&& ... params) noexcept {
            // Controller is created lazily per run-start so a moved-from runner can be
            // safely resumed by internal continuations without sharing external controller state.
            if (!bp()) {
                return;
            }

            LIKELY_IF (!controller) {
                init_controller();
                UNLIKELY_IF (!controller) {
                    return;
                }
            }
            ipc<node_count - 1>::run(*this, I_t(value_tag, std::forward<Args>(params)...));
        }

    private:
        template <typename P = controller_ptr, std::enable_if_t<std::is_same<P, lite_ptr<flow_controller>>::value, int> = 0>
        void init_controller() noexcept {
#if FLUEX_FOUNDRY_FLOW_CONTROLLER_ALIGNED_ALLOC
            controller = make_lite_ptr_with_allocator<flow_controller>(aligned_malloc_allocator{});
#else
            controller = make_lite_ptr<flow_controller>();
#endif
        }

        template <typename P = controller_ptr, std::enable_if_t<!std::is_same<P, lite_ptr<flow_controller>>::value, int> = 0>
        void init_controller() noexcept {
        }

        template <typename D, size_t AlignN, typename A>
        static flow_controller* controller_raw_ptr(const lite_ptr<flow_controller, D, AlignN, A>& c) noexcept {
            return c.get();
        }

        static flow_controller* controller_raw_ptr(flow_controller* c) noexcept {
            return c;
        }

        template <std::size_t I>
        struct ipc {
            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void run(flow_runner& self, param_t&& in) noexcept {
                using node_t = flat_storage_element_t<I, storage_t>;
                using node_i_t = typename node_t::I_t;
                using error_type = typename node_i_t::error_type;

                UNLIKELY_IF (self.controller->is_force_canceled()) {
                    using end_node_t = flat_storage_element_t<0, storage_t>;
                    using end_in_t = typename end_node_t::I_t;
                    using end_err_t = typename end_in_t::error_type;
                    ipc<0>::run(self, end_in_t(error_tag, cancel_error<end_err_t>::make(cancel_kind::hard)));
                    return;
                }

                auto& node = get<I>(self.bp()->nodes_);
                UNLIKELY_IF (self.controller->is_soft_canceled()) {
                    dispatch(node, self, node_i_t(error_tag, cancel_error<error_type>::make(cancel_kind::soft)), typename node_t::tag{}, std::true_type{});
                    return;
                }

                dispatch(node, self, std::forward<param_t>(in), typename node_t::tag{}, std::false_type{});
            }

            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ == 0>* = nullptr>
            static void run(flow_runner& self, param_t&& param) noexcept {
                self.receiver().emplace(get<0>(self.bp()->nodes_).f(std::forward<param_t>(param)));
            }

        private:
            template <typename node_t, typename param_t, typename canceled, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_runner& self, param_t&& in, flow_impl::node_tag_calc, canceled) noexcept {
                ipc<I - 1>::run(self, node.f(std::forward<param_t>(in)));
            }

            template <typename node_t, typename param_t, typename canceled, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_runner& self, param_t&& in, flow_impl::node_tag_via, canceled) noexcept {
                node.p(
                    task_wrapper_sbo([data = self.data,
                                      controller = std::move(self.controller),
                                      in = std::forward<param_t>(in)]() mutable noexcept {
                        flow_runner next_runner(std::move(data.first()), std::move(controller), std::move(data.second()));
                        ipc<I - 1>::run(next_runner, std::move(in));
                    })
                );
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_runner& self, param_t&& in, flow_impl::node_tag_async, std::true_type /*canceled?*/) noexcept {
                using node_output_t = typename node_t::O_t;

                node.dispatcher()(
                    task_wrapper_sbo([data = self.data,
                                      controller = std::move(self.controller)]() mutable noexcept {
                        flow_runner next_runner(std::move(data.first()), std::move(controller), std::move(data.second()));
                        ipc<I - 1>::run(next_runner,
                            node_output_t(error_tag, cancel_error<typename node_output_t::error_type>::make(cancel_kind::soft)));
                    })
                );
            }

            template <typename node_t, typename param_t, size_t I_ = I>
            static void dispatch_async_node(node_t& node, flow_runner& self, param_t&& in, std::true_type /* fast awaitable, no cancel*/) noexcept {
                auto& dispatcher = node.dispatcher();
                auto& adaptor = node.adaptor();
                auto& factory = node.factory();

                auto awaitable_or_error = factory(std::forward<param_t>(in));

                using node_output_t = typename node_t::O_t;
                // failed to create the awaitable
                UNLIKELY_IF (!awaitable_or_error.has_value()) {
                    dispatcher(
                            task_wrapper_sbo([data = self.data,
                                                     controller = std::move(self.controller),
                                                     err = std::move(awaitable_or_error.error())]() mutable noexcept {
                                flow_runner next_runner(std::move(data.first()), std::move(controller), std::move(data.second()));
                                ipc<I - 1>::run(next_runner, node_output_t(error_tag, std::move(err)));
                            })
                    );
                    return;
                }

                auto &awaitable = awaitable_or_error.value();
                auto controller = self.controller;

                using resume_param_t = typename node_t::Df_t::awaitable_t::async_result_type;
                using next_t = callable_wrapper<void(resume_param_t&&)>;
                awaitable.emplace_nextstep(next_t([data = self.data,
                                                          go = dispatcher,
                                                          adaptor = adaptor,
                                                          controller = std::move(self.controller)](resume_param_t&& in) mutable noexcept {
                    go(task_wrapper_sbo([data = std::move(data),
                                         controller = std::move(controller),
                                         adaptor = std::move(adaptor),
                                         in = std::move(in)]() mutable noexcept {
                                flow_runner next_runner(std::move(data.first()), std::move(controller), std::move(data.second()));
                                ipc<I - 1>::run(next_runner, adaptor(std::move(in)));
                            })
                        );
                    })
                );

                UNLIKELY_IF (awaitable.submit_async() != 0) {
                    awaitable.release();
                    dispatcher(
                        task_wrapper_sbo([data = self.data,
                                          controller = std::move(controller)]() mutable noexcept {
                            flow_runner next_runner(std::move(data.first()), std::move(controller), std::move(data.second()));
                            ipc<I - 1>::run(next_runner,
                                            node_output_t(error_tag, async_submission_failed_error<typename node_output_t::error_type>::make()));
                        })
                    );
                }
            }

            template <typename node_t, typename param_t, size_t I_ = I>
            static void dispatch_async_node(node_t& node, flow_runner& self, param_t&& in, std::false_type /* normal */) noexcept {
                auto& dispatcher = node.dispatcher();
                auto& adaptor = node.adaptor();
                auto& factory = node.factory();

                auto awaitable_or_error = factory(std::forward<param_t>(in));

                using node_output_t = typename node_t::O_t;
                // failed to create the awaitable
                UNLIKELY_IF (!awaitable_or_error.has_value()) {
                    dispatcher(
                        task_wrapper_sbo([data = self.data,
                                          controller = std::move(self.controller),
                                          err = std::move(awaitable_or_error.error())]() mutable noexcept {
                            flow_runner next_runner(std::move(data.first()), std::move(controller), std::move(data.second()));
                            ipc<I - 1>::run(next_runner, node_output_t(error_tag, std::move(err)));
                        })
                    );
                    return;
                }

                // awaitable is successfully created
                auto &awaitable = awaitable_or_error.value();
                detail::flow_async_cancel_handler_t cancel_handler = nullptr;
                detail::flow_async_cancel_param_t param = nullptr;
                detail::flow_async_notify_handler_dropped_t notify_dropped = nullptr;
                awaitable.provide_cancel_handler(&cancel_handler, &notify_dropped, &param);
                
                // first make a copy here
                auto controller = self.controller;

                auto state = self.controller->lock_and_set_cancel_handler(cancel_handler, notify_dropped, param);
                UNLIKELY_IF ((state & runner_cancel::msk) != runner_cancel::locked) {
                    notify_dropped(param);
                    auto cancel_type = self.controller->is_soft_canceled() ? cancel_kind::soft : cancel_kind::hard;
                    // operation is canceled
                    awaitable.release();
                    dispatcher(
                        task_wrapper_sbo([data = self.data,
                                          controller = std::move(controller),
                                          cancel_type = cancel_type]() mutable noexcept {
                            flow_runner next_runner(std::move(data.first()), std::move(controller), std::move(data.second()));
                            ipc<I - 1>::run(next_runner,
                                node_output_t(error_tag, cancel_error<typename node_output_t::error_type>::make(cancel_type)));
                        })
                    );
                    return;
                }

                // ok prepare to submit asio
                struct guard {
                    flow_controller* controller;
                    size_t token;
                    ~guard() noexcept {
                        if (controller) controller->unlock(token);
                    }
                };

                guard g{ controller_raw_ptr(controller), state };
                using resume_param_t = typename node_t::Df_t::awaitable_t::async_result_type;
                using next_t = callable_wrapper<void(resume_param_t&&)>;
                awaitable.emplace_nextstep(next_t([data = self.data,
                                                   go = dispatcher,
                                                   adaptor = adaptor,
                                                   state = state,
                                                   controller = std::move(self.controller)](resume_param_t&& in) mutable noexcept {
                        go(task_wrapper_sbo([data = std::move(data),
                                             controller = std::move(controller),
                                             adaptor = std::move(adaptor),
                                             state = state,
                                             in = std::move(in)]() mutable noexcept {
                            auto& cs = controller->state_.get();
                            auto exp = state;
                            // am I still have the lock?
                            LIKELY_IF (!cs.compare_exchange_strong(exp, state + epoch,
                                                                   std::memory_order_acq_rel, std::memory_order_relaxed)) {
                                // no it is freed by guard.
                                controller->reset_cancel_handler();
                            } else {
                                // yes, I can reset cancel handler.
                                controller->reset_cancel_handler_when_locked();
                                cs.fetch_add(1, std::memory_order_release);
                            }

                            flow_runner next_runner(std::move(data.first()), std::move(controller), std::move(data.second()));
                                ipc<I - 1>::run(next_runner, adaptor(std::move(in)));
                          })
                        );
                    }
                ));

                // failed to submit the io.
                UNLIKELY_IF (awaitable.submit_async() != 0) {
                    controller->reset_cancel_handler_when_locked();
                    g.controller->unlock(state);
                    g.controller = nullptr;

                    awaitable.release();
                    dispatcher(
                        task_wrapper_sbo([data = self.data,
                                          controller = std::move(controller)]() mutable noexcept {
                            flow_runner next_runner(std::move(data.first()), std::move(controller), std::move(data.second()));
                            ipc<I - 1>::run(next_runner,
                                node_output_t(error_tag, async_submission_failed_error<typename node_output_t::error_type>::make()));
                        })
                    );
                }
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_runner& self, param_t&& in, flow_impl::node_tag_async, std::false_type /*canceled?*/) noexcept {
                dispatch_async_node(node, self, std::forward<param_t>(in), is_fast_awaitable<typename node_t::Df_t::awaitable_t>{});
            }
        };
    };

    template <typename bp_t>
    auto make_runner(lite_ptr<bp_t> bp) {
        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        return flow_runner<bp_t, flow_impl::stub_receiver<typename bp_t::O_t>>(std::move(bp), make_lite_ptr<flow_controller>());
    }

    template <typename bp_t, typename receiver_t>
    auto make_runner(lite_ptr<bp_t> bp, receiver_t receiver) {
        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        static_assert(flow_impl::check_receiver<receiver_t>::value,
            "a valid receiver should:\n"
            "1. be nothrow move constructible.\n"
            "2. be nothrow copy constructible.\n"
            "in order to fully enable non-alloc in pipeline running, please make your receiver shared handle");
        static_assert(flow_impl::check_receiver_compatible<typename bp_t::O_t, receiver_t>::value,
            "the provided receiver isn't compatible with the current bp's output, A valid receiver should: "
            "1. has member:: typename value_type, which should be a result_t<T, E>, represents the result it receives\n"
            "2. has member function, whose signature is [ void emplace(result_t<T, E>&&) noexcept ]\n");
        return flow_runner<bp_t, receiver_t>(std::move(bp), lite_ptr<flow_controller>(), std::move(receiver));
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

            FORCE_INLINE flow_bp* operator->() const {
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
                return static_cast<bool>(p);
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

    template <typename flow_bp_storage, typename receiver_type>
    struct flow_fast_runner {
        static constexpr std::size_t node_count = flow_bp_storage::node_count;
        static_assert(node_count > 0, "attempting to run a empty blueprint");

        using bp_t = typename flow_bp_storage::bp_t;
        using receiver_t = std::decay_t<receiver_type>;

        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        static_assert(flow_impl::check_receiver<receiver_t>::value,
            "a valid receiver should:\n"
            "1. be nothrow move constructible.\n"
            "2. be nothrow copy constructible.\n"
            "in order to fully enable non-alloc in pipeline running, please make your receiver shared handle");
        static_assert(flow_impl::check_receiver_compatible<typename bp_t::O_t, receiver_t>::value,
            "the provided receiver isn't compatible with the current bp's output, A valid receiver should: "
            "1. has member:: typename value_type, which should be a result_t<T, E>, represents the result it receives\n"
            "2. has member function, whose signature is [ void emplace(result_t<T, E>&&) noexcept ]\n");

        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        static_assert(flow_impl::check_receiver_compatible<typename bp_t::O_t, receiver_t>::value,
            "the provided receiver isn't compatible with the current bp's output, A valid receiver should: "
            "1. has member:: typename value_type, which should be a result_t<T, E>, represents the result it receives\n"
            "2. has member function, whose signature is [ void emplace(result_t<T, E>&&) noexcept ]\n");

        using I_t = typename bp_t::I_t;

        using first_node_t = flat_storage_element_t<0, typename bp_t::storage_t>;
        static_assert(std::is_same<typename first_node_t::tag, flow_impl::node_tag_end>::value,
            "A valid blueprint must end with an end");

        using storage_t = typename bp_t::storage_t;
    private:
        compressed_pair<flow_bp_storage, receiver_t> data;

        flow_bp_storage& bp() noexcept {
            return data.first();
        }

        receiver_t& receiver() noexcept {
            return data.second();
        }

    public:
        flow_fast_runner() = delete;

        template <typename R = receiver_t, std::enable_if_t<flow_impl::is_stub_receiver_v<R>>* = nullptr>
        explicit flow_fast_runner(flow_bp_storage&& bp_, receiver_t receiver_ = receiver_t())
            : data(std::move(bp_), std::move(receiver_)) {
        }

        template <typename R = receiver_t, std::enable_if_t<!flow_impl::is_stub_receiver_v<R>>* = nullptr>
        explicit flow_fast_runner(flow_bp_storage&& bp_, receiver_t receiver_)
            : data(std::move(bp_), std::move(receiver_)) {
        }

        template <typename ... Args,
            std::enable_if_t<std::is_constructible<typename I_t::value_type, Args&& ...>::value>* = nullptr>
        void operator()(Args&& ... params) noexcept {
            if (!bp()) {
                return;
            }
            ipc<node_count - 1>::run(*this, I_t(value_tag, std::forward<Args>(params)...));
        }

    private:
        template <std::size_t I>
        struct ipc {
            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void run(flow_fast_runner& self, param_t&& in) noexcept {
                using node_t = flat_storage_element_t<I, storage_t>;
                dispatch(get<I>(self.bp()->nodes_), self, std::forward<param_t>(in), typename node_t::tag {});
            }

            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ == 0>* = nullptr>
            static void run(flow_fast_runner& self, param_t&& param) noexcept {
                self.receiver().emplace(get<0>(self.bp()->nodes_).f(std::forward<param_t>(param)));
            }
        private:
            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_fast_runner& self, param_t&& in, flow_impl::node_tag_calc) noexcept {
                ipc<I - 1>::run(self, node.f(std::forward<param_t>(in)));
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_fast_runner& self, param_t&& in, flow_impl::node_tag_via) noexcept {
                node.p(
                    task_wrapper_sbo([data = std::move(self.data),
                                      in = std::forward<param_t>(in)]() mutable noexcept {
                        flow_fast_runner next_runner(std::move(data.first()), std::move(data.second()));
                        ipc<I - 1>::run(next_runner, std::move(in));
                    })
                );
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(node_t& node, flow_fast_runner& self, param_t&& in, flow_impl::node_tag_async) noexcept {
                auto& dispatcher = node.dispatcher();
                auto& adaptor = node.adaptor();
                auto& factory = node.factory();

                auto awaitable_or_err = factory(std::forward<param_t>(in));

                using node_output_t = typename node_t::O_t;
                UNLIKELY_IF (!awaitable_or_err.has_value()) {
                    // failed to create the awaitable
                    dispatcher(
                        task_wrapper_sbo([data = std::move(self.data),
                                          err = std::move(awaitable_or_err.error())]() mutable noexcept {
                            flow_fast_runner next_runner(std::move(data.first()), std::move(data.second()));
                            ipc<I - 1>::run(next_runner, node_output_t(error_tag, std::move(err)));
                        })
                    );
                    return;
                }

                // ok prepare to submit asio
                auto data = self.data;

                auto &awaitable = awaitable_or_err.value();
                using resume_param_t = typename node_t::Df_t::awaitable_t::async_result_type;
                using next_t = callable_wrapper<void(resume_param_t&&)>;
                awaitable.emplace_nextstep(next_t([data = std::move(self.data),
                                                   go = dispatcher,
                                                   adaptor = adaptor](resume_param_t&& in) mutable noexcept {
                        go(task_wrapper_sbo([data = std::move(data),
                                             adaptor = adaptor,
                                             in = std::move(in)]() mutable noexcept {
                                flow_fast_runner next_runner(std::move(data.first()), std::move(data.second()));
                                    ipc<I - 1>::run(next_runner, adaptor(std::move(in)));
                           })
                        );
                    })
                );

                // failed to submit the io.
                UNLIKELY_IF (awaitable.submit_async() != 0) {
                    awaitable.release();
                    dispatcher(
                        task_wrapper_sbo([data = std::move(data)]() mutable noexcept {
                            flow_fast_runner next_runner(std::move(data.first()), std::move(data.second()));
                            ipc<I - 1>::run(next_runner,
                                node_output_t(error_tag, async_submission_failed_error<typename node_output_t::error_type>::make()));
                        })
                    );
                }
            }
        };
    };

    template <typename bp_t,
        std::enable_if_t<conjunction_v<
            flow_impl::is_blueprint<std::decay_t<bp_t>>,
            negation<std::is_lvalue_reference<bp_t>>
        >, int> = 0>
    auto make_fast_runner(bp_t&& bp) noexcept {
        using bp_decay = std::decay_t<bp_t>;
        using bp_storage = fast_runner_impl::bp_storage<bp_decay&&>;
        return flow_fast_runner<bp_storage, flow_impl::stub_receiver<typename bp_decay::O_t>>(bp_storage(std::move(bp)));
    }

    template <typename bp_t,
        std::enable_if_t<flow_impl::is_blueprint_v<std::decay_t<bp_t>>, int> = 0>
    auto make_fast_runner(bp_t&) = delete;

    template <typename bp_t>
    auto make_fast_runner(lite_ptr<bp_t> bp) noexcept {
        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        using bp_storage = fast_runner_impl::bp_storage<lite_ptr<bp_t>>;
        return flow_fast_runner<bp_storage, flow_impl::stub_receiver<typename bp_t::O_t>>(bp_storage(bp));
    }

    template <typename bp_t>
    auto make_fast_runner_view(bp_t* bp) noexcept {
        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        using bp_storage = fast_runner_impl::bp_storage<bp_t*>;
        return flow_fast_runner<bp_storage, flow_impl::stub_receiver<typename bp_t::O_t>>(bp_storage(bp));
    }

    template <typename bp_t>
    auto make_fast_runner_view(bp_t& bp) noexcept {
        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        using bp_storage = fast_runner_impl::bp_storage<bp_t&>;
        return flow_fast_runner<bp_storage, flow_impl::stub_receiver<typename bp_t::O_t>>(bp_storage(bp));
    }

    template <typename bp_t, typename receiver_t,
        std::enable_if_t<conjunction_v<
            flow_impl::is_blueprint<std::decay_t<bp_t>>,
            negation<std::is_lvalue_reference<bp_t>>
        >, int> = 0>
    auto make_fast_runner(bp_t&& bp, receiver_t receiver) noexcept {
        using bp_decay = std::decay_t<bp_t>;
        static_assert(flow_impl::is_blueprint_v<bp_decay>, "bp_t must be a flow_blueprint");
        static_assert(flow_impl::check_receiver<receiver_t>::value,
            "a valid receiver should:\n"
            "1. be nothrow move constructible.\n"
            "2. be nothrow copy constructible.\n"
            "in order to fully enable non-alloc in pipeline running, please make your receiver shared handle");
        static_assert(flow_impl::check_receiver_compatible<typename bp_decay::O_t, receiver_t>::value,
            "the provided receiver isn't compatible with the current bp's output, A valid receiver should: "
            "1. has member:: typename value_type, which should be a result_t<T, E>, represents the result it receives\n"
            "2. has member function, whose signature is [ void emplace(result_t<T, E>&&) noexcept ]\n");
        using bp_storage = fast_runner_impl::bp_storage<bp_decay&&>;
        return flow_fast_runner<bp_storage, receiver_t>(bp_storage(std::move(bp)), std::move(receiver));
    }

    template <typename bp_t, typename receiver_t,
        std::enable_if_t<flow_impl::is_blueprint_v<std::decay_t<bp_t>>, int> = 0>
    auto make_fast_runner(bp_t&, receiver_t) = delete;

    template <typename bp_t, typename receiver_t>
    auto make_fast_runner(lite_ptr<bp_t> bp, receiver_t receiver) noexcept {
        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        static_assert(flow_impl::check_receiver<receiver_t>::value,
            "a valid receiver should:\n"
            "1. be nothrow move constructible.\n"
            "2. be nothrow copy constructible.\n"
            "in order to fully enable non-alloc in pipeline running, please make your receiver shared handle");
        static_assert(flow_impl::check_receiver_compatible<typename bp_t::O_t, receiver_t>::value,
            "the provided receiver isn't compatible with the current bp's output, A valid receiver should: "
            "1. has member:: typename value_type, which should be a result_t<T, E>, represents the result it receives\n"
            "2. has member function, whose signature is [ void emplace(result_t<T, E>&&) noexcept ]\n");
        using bp_storage = fast_runner_impl::bp_storage<lite_ptr<bp_t>>;
        return flow_fast_runner<bp_storage, receiver_t>(bp_storage(bp), std::move(receiver));
    }

    template <typename bp_t, typename receiver_t>
    auto make_fast_runner_view(bp_t* bp, receiver_t receiver) noexcept {
        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        static_assert(flow_impl::check_receiver<receiver_t>::value,
            "a valid receiver should:\n"
            "1. be nothrow move constructible.\n"
            "2. be nothrow copy constructible.\n"
            "in order to fully enable non-alloc in pipeline running, please make your receiver shared handle");
        static_assert(flow_impl::check_receiver_compatible<typename bp_t::O_t, receiver_t>::value,
            "the provided receiver isn't compatible with the current bp's output, A valid receiver should: "
            "1. has member:: typename value_type, which should be a result_t<T, E>, represents the result it receives\n"
            "2. has member function, whose signature is [ void emplace(result_t<T, E>&&) noexcept ]\n");
        using bp_storage = fast_runner_impl::bp_storage<bp_t*>;
        return flow_fast_runner<bp_storage, receiver_t>(bp_storage(bp), std::move(receiver));
    }

    template <typename bp_t, typename receiver_t>
    auto make_fast_runner_view(bp_t& bp, receiver_t receiver) noexcept {
        static_assert(flow_impl::is_blueprint_v<bp_t>, "bp_t must be a flow_blueprint");
        static_assert(flow_impl::check_receiver<receiver_t>::value,
            "a valid receiver should:\n"
            "1. be nothrow move constructible.\n"
            "2. be nothrow copy constructible.\n"
            "in order to fully enable non-alloc in pipeline running, please make your receiver shared handle");
        static_assert(flow_impl::check_receiver_compatible<typename bp_t::O_t, receiver_t>::value,
            "the provided receiver isn't compatible with the current bp's output, A valid receiver should: "
            "1. has member:: typename value_type, which should be a result_t<T, E>, represents the result it receives\n"
            "2. has member function, whose signature is [ void emplace(result_t<T, E>&&) noexcept ]\n");
        using bp_storage = fast_runner_impl::bp_storage<bp_t&>;
        return flow_fast_runner<bp_storage, receiver_t>(bp_storage(bp), std::move(receiver));
    }
} // namespace flux_foundry

#endif // FLUX_FOUNDRY_FLOW_RUNNER_H
