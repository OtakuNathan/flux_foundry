#ifndef __LITE_FNDS_FLOW_RUNNER_H__
#define __LITE_FNDS_FLOW_RUNNER_H__

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

    template<typename I_t, typename O_t, typename... Nodes>
    struct flow_runner {
        static constexpr std::size_t node_count = sizeof ...(Nodes);
        static_assert(node_count > 0, "attempting to run a empty blueprint");

        using bp_t = flow_impl::flow_blueprint<I_t, O_t, Nodes...>;

        using first_node_t = std::tuple_element_t<0, typename bp_t::storage_t>;
        static_assert(flow_impl::is_end_node_t<first_node_t>::value, "A valid blueprint must end with an end");

        using bp_ptr = std::shared_ptr<bp_t>;
        using controller_ptr = std::shared_ptr<flow_controller>;
        using storage_t = typename bp_t::storage_t;

    private:
        controller_ptr controller;
        bp_ptr bp;
        using self_type = flow_runner;

    public:
        flow_runner() = delete;

        explicit flow_runner(bp_ptr bp_, controller_ptr ctl = controller_ptr())
            : controller(ctl ? std::move(ctl) : std::make_shared<flow_controller>())
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

            step<node_count - 1>::run(*this, I_t(value_tag, std::forward<In>(in)));
        }

    private:
        template <std::size_t Index>
        struct step {
            template <typename In>
            static void run(flow_runner &self, In &&in) noexcept {
                using node_t = std::tuple_element_t<Index, storage_t>;
                using is_ctrl = flow_impl::is_control_node_t<node_t>;
                using is_last_node = flow_impl::is_end_node_t<node_t>;

                using node_i_t = typename node_t::I_t;
                using error_type = typename node_i_t::error_type;

                if (Index != 0 && self.controller->is_force_canceled()) {
                    using end_node_t = std::tuple_element_t<0, storage_t>;
                    using end_in_t = typename end_node_t::I_t;
                    using end_err_t = typename end_in_t::error_type;

                    end_in_t cancel_in(error_tag, cancel_error<end_err_t>::make(cancel_kind::hard));
                    step<0>::run(self, std::move(cancel_in));
                    return;
                }

                if (Index != 0 && self.controller->is_soft_canceled()) {
                    dispatch(is_ctrl{}, is_last_node{}, self,
                             node_i_t(error_tag, cancel_error<error_type>::make(cancel_kind::soft)));
                    return;
                }

                dispatch(is_ctrl{}, is_last_node{}, self, std::forward<In>(in));
            }

        private:
            template<typename In>
            static void dispatch(std::false_type /*calc*/,
                                 std::false_type, // has next
                                 flow_runner &self, In &&in) noexcept {
                auto &node = std::get<Index>(self.bp->nodes_);
                step<Index - 1>::run(self, node.f(std::forward<In>(in)));
            }

            template<typename In>
            static void dispatch(std::false_type /*calc*/,
                                 std::true_type /*last*/,
                                 flow_runner &self, In &&in) noexcept {
                (void) self;
                auto &node = std::get<Index>(self.bp->nodes_);
                (void) node.f(std::forward<In>(in));
            }

            template<typename In>
            static void dispatch(std::true_type /*control*/,
                                 std::false_type /*has_next*/,
                                 flow_runner &self, In &&in) noexcept {
                auto &node = std::get<Index>(self.bp->nodes_);

                auto bp = self.bp;
                auto controller = self.controller;
                node.p(task_wrapper_sbo([bp = std::move(bp),
                                         controller = std::move(controller),
                                         in = std::forward<In>(in)]() mutable noexcept {
                    flow_runner next_runner(std::move(bp), std::move(controller));
                    step<Index - 1>::run(next_runner, std::move(in));
                }));
            }
        };
    };

    template<typename I_t, typename O_t, typename... Nodes>
    auto make_runner(std::shared_ptr<flow_impl::flow_blueprint<I_t, O_t, Nodes...>> bp,
        std::shared_ptr<flow_controller> ctl = nullptr) noexcept {
        return flow_runner<I_t, O_t, Nodes...>(std::move(bp), std::move(ctl));
    }
} // namespace lite_fnds

#endif // __LITE_FNDS_FLOW_RUNNER_H__
