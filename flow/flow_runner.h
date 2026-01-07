#ifndef LITE_FNDS_FLOW_RUNNER_H
#define LITE_FNDS_FLOW_RUNNER_H

#include <atomic>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <stdexcept>

#include "../task/task_wrapper.h"
#include "flow_blueprint.h"

namespace lite_fnds {
    enum class cancel_kind {
        soft,
        hard,
    };

    template<class E>
    struct cancel_error {
        static E make(cancel_kind) {
            static_assert(sizeof(E) == 0,
                          "lite_fnds::cancel_error<E> is not specialized for this error type E. "
                          "Please provide `template<> struct lite_fnds::cancel_error<E>` "
                          "with a static `E make(cancel_kind)` member.");
            return 0;
        }
    };

    template<>
    struct cancel_error<std::exception_ptr> {
        static std::exception_ptr make(cancel_kind kind) {
            const char *msg = (kind == cancel_kind::hard)
                                  ? "flow hard-canceled"
                                  : "flow soft-canceled";
            return std::make_exception_ptr(std::logic_error(msg));
        }
    };

    struct flow_controller {
    private:
        enum class runner_cancel {
            none,
            hard,
            soft,
        };
        std::atomic<runner_cancel> data{runner_cancel::none};
    public:
        void cancel(bool force = false) noexcept {
            data.store(force ? runner_cancel::hard : runner_cancel::soft, std::memory_order_relaxed);
        }

        bool is_force_canceled() const noexcept {
            return data.load(std::memory_order_relaxed) == runner_cancel::hard;
        }

        bool is_soft_canceled() const noexcept {
            return data.load(std::memory_order_relaxed) == runner_cancel::soft;
        }

        bool is_canceled() const noexcept {
            auto s = data.load(std::memory_order_relaxed);
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
        static_assert(flow_impl::is_end_node_v<first_node_t>, "A valid blueprint must end with an end");

        using bp_ptr = std::shared_ptr<bp_t>;
        using controller_ptr = std::shared_ptr<flow_controller>;
    private:
        controller_ptr controller;
        bp_ptr bp;
    public:
        flow_runner() = delete;

        explicit flow_runner(bp_ptr bp_, controller_ptr ctrl = controller_ptr())
            : controller(ctrl ? std::move(ctrl) : std::make_shared<flow_controller>())
              , bp(std::move(bp_)) {
        }

        controller_ptr get_controller() const noexcept {
            return controller;
        }

        template <typename In,
            std::enable_if_t<std::is_convertible<In, typename I_t::value_type>::value>* = nullptr>
        void operator()(In &&in) noexcept {
            if (!bp) {
                return;
            }
            ipc<node_count - 1>::run(*this, I_t(value_tag, std::forward<In>(in)));
        }
    private:
        template <std::size_t I>
        struct ipc {
            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void run(flow_runner &self, param_t &&in) noexcept {
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

                auto &node = get<I>(self.bp->nodes_);
                using is_ctrl = flow_impl::is_control_node_t<node_t>;
                UNLIKELY_IF(self.controller->is_soft_canceled()) {
                    dispatch(is_ctrl{}, node, self, node_i_t(error_tag, cancel_error<error_type>::make(cancel_kind::soft)));
                    return;
                }

                dispatch(is_ctrl{}, node, self, std::forward<param_t>(in));
            }

            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ == 0>* = nullptr>
            static void run(flow_runner& self, param_t &&param) noexcept {
                get<0>(self.bp->nodes_).f(std::forward<param_t>(param));
            }
        private:
            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(std::false_type /*calc*/,
                                 node_t& node, flow_runner &self, param_t &&in) noexcept {
                ipc<I - 1>::run(self, node.f(std::forward<param_t>(in)));
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(std::true_type /*control*/,
                                 node_t& node, flow_runner &self, param_t &&in) noexcept {
                node.p(task_wrapper_sbo([bp = self.bp,
                                                controller = self.controller,
                                                in = std::forward<param_t>(in)]() mutable noexcept {
                    flow_runner next_runner(std::move(bp), std::move(controller));
                    ipc<I - 1>::run(next_runner, std::move(in));
                }));
            }
        };
    };

    template <typename I_t, typename O_t, typename... Nodes>
    auto make_runner(std::shared_ptr<flow_impl::flow_blueprint<I_t, O_t, Nodes...>> bp,
        std::shared_ptr<flow_controller> ctrl = nullptr) noexcept {
        return flow_runner<flow_impl::flow_blueprint<I_t, O_t, Nodes...>>(std::move(bp), std::move(ctrl));
    }

    // one-short runner.
    namespace fast_runner_impl {
        template <typename> struct F : std::false_type {};

        template <typename flow_bp>
        struct bp_storage {
            static_assert(flow_impl::is_blueprint_v<flow_bp>, "flow_bp must be a flow_blueprint");
            static_assert(!F<bp_storage>::value, "a flat bp storage should have a raw pointer or a unique_ptr or a shared_ptr or a reference.");
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

            flow_bp *p;
            explicit bp_storage(flow_bp* p_)
                noexcept : p(p_) {}

            inline flow_bp* operator->() const {
                return p;
            }

            explicit operator bool () const noexcept {
                return p;
            }
        };

        template <typename flow_bp>
        struct bp_storage <std::unique_ptr<flow_bp>> {
            static_assert(flow_impl::is_blueprint_v<flow_bp>, "flow_bp must be a flow_blueprint");

            static constexpr std::size_t node_count = flow_bp::node_count;
            static_assert(node_count > 0, "attempting to run an empty blueprint");

            using bp_t = std::decay_t<flow_bp>;
            using I_t = typename bp_t::I_t;
            using O_t = typename bp_t::O_t;
            using storage_t = typename bp_t::storage_t;

            std::unique_ptr<flow_bp> p;
            explicit bp_storage(std::unique_ptr<flow_bp> p_)
                noexcept : p(std::move(p_)) {}

            FORCE_INLINE flow_bp* operator->() const {
                return p.get();
            }

            explicit operator bool () const noexcept {
                return p;
            }
        };

        template <typename flow_bp>
        struct bp_storage <std::shared_ptr<flow_bp>> {
            static_assert(flow_impl::is_blueprint_v<flow_bp>, "flow_bp must be a flow_blueprint");

            static constexpr std::size_t node_count = flow_bp::node_count;
            static_assert(node_count > 0, "attempting to run an empty blueprint");

            using bp_t = std::decay_t<flow_bp>;
            using I_t = typename bp_t::I_t;
            using O_t = typename bp_t::O_t;
            using storage_t = typename bp_t::storage_t;

            std::shared_ptr<flow_bp> p;
            explicit bp_storage(std::shared_ptr<flow_bp> p_)
                noexcept : p(std::move(p_)) {}

            FORCE_INLINE flow_bp* operator->() const {
                return p.get();
            }

            explicit operator bool () const noexcept {
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
                noexcept : p(p_) {}

            FORCE_INLINE flow_bp* operator->() const {
                return &p;
            }

            explicit operator bool () const noexcept {
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

            std::unique_ptr<flow_bp> p;

            explicit bp_storage(flow_bp&& p_)
                : p(std::make_unique<flow_bp>(std::move(p_))) {}

            FORCE_INLINE flow_bp* operator->() const {
                return p.get();
            }

            explicit operator bool () const noexcept {
                return true;
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
        static_assert(flow_impl::is_end_node_v<first_node_t>, "A valid blueprint must end with an end");

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
        void operator()(In &&in) noexcept {
            ipc<node_count - 1>::run(*this, I_t(value_tag, std::forward<In>(in)));
        }
    private:
        template <std::size_t I>
        struct ipc {
            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void run(flow_fast_runner &self, param_t &&in) noexcept {
                using node_t = flat_storage_element_t<I, storage_t>;
                dispatch(flow_impl::is_control_node_t<node_t>{},
                         get<I>(self.bp->nodes_), self, std::forward<param_t>(in));
            }

            template <typename param_t, size_t I_ = I, std::enable_if_t<I_ == 0>* = nullptr>
            static void run(flow_fast_runner& self, param_t &&param) noexcept {
                get<0>(self.bp->nodes_).f(std::forward<param_t>(param));
            }
        private:
            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(std::false_type /*calc*/,
                                 node_t& node, flow_fast_runner &self, param_t &&in) noexcept {
                ipc<I - 1>::run(self, node.f(std::forward<param_t>(in)));
            }

            template <typename node_t, typename param_t, size_t I_ = I, std::enable_if_t<I_ != 0>* = nullptr>
            static void dispatch(std::true_type /*control*/,
                                 node_t& node, flow_fast_runner &self, param_t &&in) noexcept {
                node.p(task_wrapper_sbo([bp = std::move(self.bp),
                                         in = std::forward<param_t>(in)]() mutable noexcept {
                    flow_fast_runner next_runner(std::move(bp));
                    ipc<I - 1>::run(next_runner, std::move(in));
                }));
            }
        };
    };

    template <typename bp_t>
    auto make_fast_runner(bp_t&& bp) noexcept {
        using bp_storage = fast_runner_impl::bp_storage<bp_t&&>;
        return flow_fast_runner<bp_storage>(bp_storage(std::forward<bp_t>(bp)));
    }

    template <typename bp_t>
    auto make_fast_runner_view(bp_t* bp) noexcept {
        using bp_storage = fast_runner_impl::bp_storage<bp_t*>;
        return flow_fast_runner<bp_storage>(bp_storage(bp));
    }

    template <typename bp_t>
    auto make_fast_runner(std::unique_ptr<bp_t> bp) noexcept {
        using bp_storage = fast_runner_impl::bp_storage<std::unique_ptr<bp_t>>;
        return flow_fast_runner<bp_storage>(bp_storage(std::move(bp)));
    }

    template <typename bp_t>
    auto make_fast_runner(std::shared_ptr<bp_t> bp) noexcept {
        using bp_storage = fast_runner_impl::bp_storage<std::shared_ptr<bp_t>>;
        return flow_fast_runner<bp_storage>(bp_storage(bp));
    }
} // namespace lite_fnds

#endif // __LITE_FNDS_FLOW_RUNNER_H__