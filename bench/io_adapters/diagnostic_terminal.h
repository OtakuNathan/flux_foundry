#pragma once
// Benchmark-only alternatives, NOT public adapters. No controller cancellation.
// The host guarantees exactly one terminal callback per accepted submission,
// and no callback after a failed submission. Blueprint-view cases additionally
// require the blueprint to outlive every completion.
namespace probe {
#if defined(FF_DIAG_URING)
template <typename Operation>
struct uring_terminal final : flux_foundry::fast_awaitable_base<uring_terminal<Operation>,
        flux_foundry::extension::io_uring_result<Operation>, flux_foundry::flow_async_agg_err_t> {
    using E = flux_foundry::flow_async_agg_err_t;
    using request_type = flux_foundry::extension::io_uring_request<Operation>;
    using async_result_type = flux_foundry::result_t<flux_foundry::extension::io_uring_result<Operation>, E>;
    static_assert(std::is_nothrow_move_constructible<Operation>::value
        && std::is_nothrow_destructible<Operation>::value, "diagnostic operation must move/destroy without throwing");
    request_type request;
    explicit uring_terminal(flux_foundry::result_t<request_type, E>&& in) noexcept
        : request(std::move(in).value()) {}
    bool available() const noexcept { return request.context != nullptr; }
    int submit() noexcept {
        if (!available()) return -EINVAL;
        // fast_awaitable_base retains its initial ownership through terminal
        // resume. There is no independent logical-cancel path to consume it.
        return request.context->enqueue(request.operation, &complete, this);
    }
    static void complete(void* data, int result, unsigned flags) noexcept {
        auto* self = static_cast<uring_terminal*>(data);
        if (result < 0)
            self->resume(async_result_type(flux_foundry::error_tag,
                flux_foundry::extension::detail::io_error<E>::make(-result)));
        else
            self->resume(async_result_type(flux_foundry::value_tag,
                flux_foundry::extension::io_uring_result<Operation>{std::move(self->request.operation), result, flags}));
        // No access to self after resume: it may have been deleted.
    }
};
#endif

#if defined(FF_DIAG_DPDK)
template <std::size_t Capacity>
struct dpdk_terminal final : flux_foundry::fast_awaitable_base<dpdk_terminal<Capacity>,
        flux_foundry::extension::dpdk_packet_batch<Capacity>, flux_foundry::flow_async_agg_err_t> {
    using E = flux_foundry::flow_async_agg_err_t;
    using context_type = flux_foundry::extension::dpdk_rx_context<Capacity>;
    using batch_type = typename context_type::batch_type;
    using async_result_type = flux_foundry::result_t<batch_type, E>;
    context_type* context;
    explicit dpdk_terminal(flux_foundry::result_t<context_type*, E>&& in) noexcept : context(in.value()) {}
    bool available() const noexcept { return context != nullptr; }
    int submit() noexcept {
        return available() ? context->enqueue(&complete, &canceled, this) : -EINVAL;
    }
    static bool canceled(void*) noexcept { return false; }
    static void complete(void* data, batch_type&& batch, int error) noexcept {
        auto* self = static_cast<dpdk_terminal*>(data);
        if (error)
            self->resume(async_result_type(flux_foundry::error_tag,
                flux_foundry::extension::detail::io_error<E>::make(error)));
        else self->resume(async_result_type(flux_foundry::value_tag, std::move(batch)));
    }
};
#endif
} // namespace probe
