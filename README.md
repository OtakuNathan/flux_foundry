# lite_fnds & Flow+

**lite_fnds** is a lightweight, hardware-aware C++ foundation library designed to provide predictable performance for latency-sensitive systems. **Flow+** is an asynchronous task orchestration framework built upon these primitives, utilizing a static DSL (Domain Specific Language) to simplify the construction of complex execution pipelines.

The primary goal of this project is to explore the limits of zero-overhead abstractions in C++ and to minimize resource contention in high-concurrency environments through deterministic memory management and layout optimization.

## 📊 Performance Benchmarks
**Note**: The following benchmarks measure the **pure dispatch latency** and **abstraction overhead** of the framework. The test pipeline consists of 20+ lightweight integer arithmetic nodes to expose the minimal cost per stage. In real-world "heavy" workloads (e.g., I/O or heavy compute), the framework's overhead effectively becomes negligible (approaching zero impact).

**Scenario**: $10^7$ iterations, 20-stage pipeline, integer increment logic.

| Execution Mode | Total Time ($10^7$ ops) | Avg Latency per Node | Description                                                              |
| :--- | :--- | :--- |:-------------------------------------------------------------------------|
| **Standard Runner** | ~2.2 s | ~220 ns | Dynamic construction of blueprint, suitable for flexible runtime graphs. |
| **Fast Runner** | **~1.5 s** | **~150 ns** | Zero-virtual-call. Pure template expansion overhead.                     |

> *Note: Benchmarks were run on consumer-grade x86_64 hardware. Actual performance may vary depending on the CPU architecture, compiler version, and optimization flags.*

---

## 🛠️ Core Components

### 1. Deterministic Memory Management
To mitigate the non-determinism often found in standard allocators, this library provides:
* **`lite_ptr<T>`**: A lightweight reference-counted smart pointer. It optimizes memory ordering (using a `release-fence-acquire` sequence) and ensures the control block is co-located with the object to improve cache locality.
* **`static_mem_pool`**: A tiered, pre-allocated memory pool that ensures O(1) allocation and deallocation, avoiding system calls on the hot path.
* **`hazard_ptr`**: Implements a lock-free memory reclamation strategy for lock-free data structures, effectively solving the ABA problem.
* **`inplace_t<T>`**: A helper for manual object lifetime management, supporting exception-safe placement new and aggregate initialization.

### 2. Concurrency & Synchronization
Engineered to reduce false sharing and thread contention:
* **Lock-Free Queues**: Includes SPSC, MPSC, and MPMC queue implementations. These utilize CAS-based slot reservation and sequence tracking to maintain throughput.
* **Hardware Alignment**: Critical structures (such as `hazard_record`) are strictly aligned to `CACHE_LINE_SIZE` (64 bytes).
* **`simple_executor`**: A lightweight scheduler based on an MPSC queue. It employs `thread_local` state tracking to prevent deadlocks caused by task reentrancy, making it suitable for high-frequency micro-tasks.

### 3. Tasks & Type Erasure
* **`task_wrapper_sbo`**: A type-erasure container optimized for 64-byte cache lines. It uses SBO (Small Object Optimization) to avoid heap allocation for lambdas; trivial types are further optimized into `memcpy` operations.
* **`future_task`**: An asynchronous task wrapper compatible with `std::future`, enforcing strict `noexcept` move semantics to ensure stability.

### 4. Flow+ Pipeline
A template-based DSL for composing asynchronous logic:
* **Static Folding**: Pipelines constructed via the `|` operator are flattened at compile-time, minimizing runtime abstraction overhead.
* **Functional Nodes**:
* `transform`: Data processing.
* `via`: Context switching to a specific Executor.
* `on_error` / `catch_exception`: Unified error handling paths.
* **Error Propagation**: Built on `result_t` (similar to `std::expected`), allowing the system to function correctly even in environments where C++ exceptions are disabled.

---

## 💻 Usage Example

The following example demonstrates how to define and execute a flow that includes computation, thread switching, and error handling:

```cpp
#include <iostream>

#include "flow/flow_blueprint.h"
#include "flow/flow_node.h"
#include "flow/flow_runner.h"

struct fake_executor {
    void dispatch(lite_fnds::task_wrapper_sbo sbo) noexcept {
        sbo();
    }
};

int main(int argc, char *argv[]) {
    using std::cout;
    using std::endl;

    using lite_fnds::result_t;
    using lite_fnds::value_tag;
    using lite_fnds::error_tag;

    using lite_fnds::make_blueprint;
    using lite_fnds::via;
    using lite_fnds::transform;
    using lite_fnds::then;
    using lite_fnds::end;
    using lite_fnds::on_error;
    using lite_fnds::make_runner;
    using lite_fnds::catch_exception;
    using E = std::exception_ptr;

    fake_executor executor;

    int v = 100;
    // 1. Define the Blueprint
    auto bp = make_blueprint<int>()
         | via(&executor)
         | transform([&v] (int x) noexcept{
             return v += 10, (double)v + x;
         })
         | then([](result_t<double, E> f) {
             std::cout << f.value() << std::endl;
             if (f.value() > 120) {
                 throw std::logic_error("exception on then node error");
             }
             f.value() += 10;
             return f;
         })
         | on_error([&](result_t<double, E> f) {
             try {
                 std::rethrow_exception(f.error());
             } catch (const std::logic_error& e) {
                 std::cout << e.what() << std::endl;
                 // return result_t<double, E>(value_tag, 1.0);
                 throw e;
             } catch (...) {
                 return result_t<double, E>(error_tag, std::current_exception());
             }
         })
         | catch_exception<std::logic_error>([](const std::logic_error& e) {
                std::cout << e.what() << endl;
                return 3.0;
            })
         | end([](result_t<double, E> f) {
             if (f.has_value()) {
                 std::cout << "finaly value is: " << f.value() << std::endl;
             }
             return f;
         });

    using bp_t = decltype(bp);
    std::shared_ptr<bp_t> bp_ptr = std::make_shared<bp_t>(std::move(bp));
    
    // 2. Create Runner and Execute
    auto runner = make_runner(bp_ptr);

    runner(10);
    cout << "V become after one shot of bp:" << v << endl;
    cout << endl;

    /*
    * 120
    * finaly value is: 130
    * become after one shot of bp:110
    */

    runner(10);
    cout << "V become after one shot of bp:" << v << endl;
    cout << endl;

    /*
     * 130
     * exception on then node error
     * exception on then node error
     * finaly value is: 3
     * V become after one shot of bp:120
     */
    return 0;
}
```

## ⚙️ Design Philosophy
* **Pay only for what you use**: Features like monitoring or exception support are injected via policy templates, incurring zero overhead when disabled.
* **Hardware-First**: Data structures prioritize cache line alignment, branch prediction hints (LIKELY_IF), and precise memory ordering.
* **Robustness**: While full C++ exception support is available, the core library is designed to compile and run reliably in -fno-exceptions modes.

## 📦 Requirements
* **Compiler**: C++14 or later (GCC, Clang, MSVC).
* **Architecture**: Optimized primarily for x86_64 and ARMv8 (includes specific optimizations like _mm_pause).
* **Integration**: Header-only library; simply include the headers in your project.

## 📄 License
MIT License © 2025 Nathan