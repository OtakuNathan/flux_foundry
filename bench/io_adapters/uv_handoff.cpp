// Scheduled one-producer handoff through the production uv_executor.
// Shared measurement helpers include entry points used by other probes.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "bench/load_probe.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "executor/uv_executor.h"
#include <string>

int main(int argc, char** argv) {
    using namespace load_probe;
    if (argc != 8) {
        std::fprintf(stderr, "usage: uv_handoff raw|ordinary-fast|normal PROFILE SAMPLES WORK BUDGET OWNER_CPU PRODUCER_CPU\n");
        return 2;
    }
    const char* mode = argv[1];
    require(!std::strcmp(mode, "raw") || !std::strcmp(mode, "ordinary-fast") ||
            !std::strcmp(mode, "normal"), "invalid mode");
    const Profile* profile = nullptr;
    for (const auto& p : profiles) if (!std::strcmp(p.name, argv[2])) profile = &p;
    require(profile != nullptr, "invalid arrival profile");
    Options o;
    o.samples = number(argv[3]);
    o.work = number(argv[4]);
    const auto budget = number(argv[5]);
    o.consumer_cpu = !std::strcmp(argv[6], "-1") ? -1 : static_cast<int>(number(argv[6]));
    o.producer_cpu = !std::strcmp(argv[7], "-1") ? -1 : static_cast<int>(number(argv[7]));
    o.capacity = 1024;
    require(o.samples > 0 && budget > 0 &&
            (o.producer_cpu < 0 || o.consumer_cpu < 0 || o.producer_cpu != o.consumer_cpu),
            "positive samples/budget and distinct CPUs required");
    pin(o.producer_cpu);

    uv_executor* executor = nullptr;
    std::atomic<bool> ready{false}, close_returned{false};
    std::thread owner([&] {
        pin(o.consumer_cpu);
        uv_loop_t loop{};
        require(uv_loop_init(&loop) == 0, "uv_loop_init failed");
        {
            uv_executor ex(&loop, o.capacity, budget);
            executor = &ex;
            ready.store(true, std::memory_order_release);
            uv_run(&loop, UV_RUN_DEFAULT);
            require(ex.closed(), "executor did not close");
            // Public object lifetime includes the producer's close() call.
            while (!close_returned.load(std::memory_order_acquire)) relax();
        }
        require(uv_loop_close(&loop) == 0, "uv_loop_close failed");
    });
    while (!ready.load(std::memory_order_acquire)) relax();
    Port port{executor, [](void* ptr, task_wrapper_sbo&& task) noexcept {
        auto* ex = static_cast<uv_executor*>(ptr);
        // No-drop benchmark policy: wait for queue space. Production dispatch
        // itself rejects full queues; this wrapper makes that policy explicit.
        for (backoff_strategy<> backoff; !ex->try_dispatch(std::move(task)); backoff.yield()) {
            require(bool(task) && !ex->closed(), "unexpected dispatch rejection");
        }
    }};
    print_csv_header("phase,budget,");
    Options warmup = o;
    warmup.samples = std::min<std::size_t>(o.samples, 4096);
    const auto warm_prefix = std::string("warmup,") + std::to_string(budget) + ",";
    measured_run(port, o.capacity, warmup, profiles[1], mode, 0, warm_prefix.c_str());
    const auto prefix = std::string("measured,") + std::to_string(budget) + ",";
    measured_run(port, o.capacity, o, *profile, mode, 1, prefix.c_str());
    executor->close();
    close_returned.store(true, std::memory_order_release);
    owner.join();
}
