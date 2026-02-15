# flux_foundry

> Lightweight C++14 foundations for async flow orchestration, lock-free queues, and low-level memory building blocks.

## ✨ Overview

`flux_foundry` is a **header-only** C++14 library that combines:

- ⚡ Low-overhead flow pipeline execution (`flow_runner`, `flow_fast_runner`)
- 🔁 Flow DSL operators (`transform`, `then`, `on_error`, `catch_exception`, `flow_async`, `await_when_all`, `await_when_any` `await_when_all_fast`, `await_when_any_fast`)
- 🧠 Memory/data primitives (`result_t`, `either_t`, `flat_storage`, `lite_ptr`, `inplace_t`, `pooling tool`)
- 🧵 Queue/executor infrastructure (`spsc/mpsc/mpmc/spmc`, `simple_executor`, `gsource_executor`)

The project is tuned for predictable behavior and explicit contracts under concurrency.

## 🧩 Components

| Module | Main files                                                                                                               | What it provides |
|---|--------------------------------------------------------------------------------------------------------------------------|---|
| `flow/` | `flow_node.h`, `flow_blueprint.h`, `flow_runner.h`, `flow_async_aggregator.h`, `flow_awaitable.h`                        | Pipeline DSL (`transform/then/on_error/catch_exception/via/await`), node graph flattening, async steps, `when_all/when_any`, cancel/error propagation |
| `executor/` | `simple_executor.h`, `gsource_executor.h`                                                                                | MPSC single-consumer executor, GLib source-backed executor |
| `utility/` | `concurrent_queues.h`, `callable_wrapper.h`, `back_off.h`                                                                | Lock-free queues, callable type-erasure with SBO, backoff policies |
| `task/` | `task_wrapper.h`, `future_task.h`                                                                                        | Task wrappers and future-related task abstraction |
| `memory/` | `result_t.h`, `either_t.h`, `flat_storage.h`, `lite_ptr.h`, `inplace_t.h`, `padded_t.h`, `hazard_ptr.h` `pooling.h` | Result/error transport, tagged unions, storage composition, smart pointer, inplace construction helpers, padding/alignment primitives |
| `base/` | `traits.h`, `type_erase_base.h`, `inplace_base.h`, `type_utility.h`                                                      | Traits/macros and low-level reusable base utilities |

## 🚀 Quick Start (CMake)

```bash
cmake -S . -B build -DFLUX_FOUNDRY_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run specific suites:

```bash
ctest --test-dir build -C Release -L smoke  --output-on-failure
ctest --test-dir build -C Release -L stress --output-on-failure
ctest --test-dir build -C Release -L perf   --output-on-failure
```

## ✅ CI

GitHub Actions workflow: `.github/workflows/ci.yml`

- OS matrix: `ubuntu-latest`, `windows-latest`
- Build: CMake + C++14
- Test gate: `ctest -L smoke`

This keeps PR checks fast while still validating the core flow path on both platforms.

## 🛠 Flow DSL at a glance

```cpp
#include "flow/flow.h"

using namespace flux_foundry;
using E = std::exception_ptr;

struct inline_executor {
    void dispatch(task_wrapper_sbo t) noexcept { t(); }
};

struct plus_one_awaitable final : awaitable_base<plus_one_awaitable, int, E> {
    using async_result_type = result_t<int, E>;
    int v;

    explicit plus_one_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {}

    int submit() noexcept {
        this->resume(async_result_type(value_tag, v + 1));
        return 0;
    }

    void cancel() noexcept {}
};

int main() {
    inline_executor ex;

    auto bp = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 1; })
        | then([](result_t<int, E>&& in) noexcept -> result_t<int, E> {
            if (!in.has_value()) {
                return result_t<int, E>(error_tag, std::move(in).error());
            }
            return result_t<int, E>(value_tag, in.value() * 2);
        })
        | on_error([](result_t<int, E>&&) noexcept -> result_t<int, E> {
            return result_t<int, E>(value_tag, -1); // recover from non-exception errors
        })
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        | catch_exception<std::runtime_error>([](const std::runtime_error&) noexcept {
            return -2; // recover from std::runtime_error
        })
#endif
        | await<plus_one_awaitable>(&ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr);
    runner(1);
}
```

## 🏗 Architecture sketch

```mermaid
flowchart LR
    A[Blueprint DSL<br/>transform/then/on_error/catch_exception/via/async/when_all/when_any] --> B[flow_blueprint]
    B --> C[flow_runner / flow_fast_runner]
    C --> D[Executor<br/>simple_executor or custom dispatch]
    C --> E[Awaitable Layer<br/>awaitable_base + factories]
    E --> F[Async Aggregators<br/>when_all / when_any /<br/>when_all_fast / when_any_fast state]
    C --> G[result_t/either_t error path]
    D --> H[Queues<br/>MPSC/MPMC/SPSC/SPMC Stack<br/>static_list(mpmc static stack)]
```

## 🔧 Key contracts

### `simple_executor`

- `run()`:
  - single-consumer only
  - non-reentrant on the same thread
- `try_shutdown()`:
  - attempts `running -> shutdown`
  - returns `true` when shutdown is already visible/succeeded
- `dispatch()` after shutdown:
  - treated as invalid usage (`assert` + `abort`)

### Flow runner

- Strongly typed node IO (`result_t<T, E>`)
- Explicit cancel path via `flow_controller`
- Async node submit/cancel lifecycle managed through `awaitable_base`

### Fast async lane

- `fast_awaitable_base`:
  - designed for throughput-first async path
  - no awaitable internal state-machine CAS on `submit/resume`
  - contract-based: backend must guarantee `resume()` is eventually called exactly once after successful `submit()`
- `await_when_all_fast(...)` / `await_when_any_fast(...)`:
  - use fast aggregator awaitables (`flow_when_all_fast_awaitable`, `flow_when_any_fast_awaitable`)
  - run subgraphs with `fast_runner`
  - cancellation semantics are intentionally removed from the fast path (`cancel()` is no-op)
- Recommended combinations:
  - max throughput: `fast_runner + fast awaitable/fast aggregator`
  - full cancellation semantics: `flow_runner + awaitable_base/normal aggregator`

## 📊 Benchmark snapshots
Command:

```bash
cmake -S . -B build -DFLUX_FOUNDRY_BUILD_TESTS=ON
cmake --build build --config Release --target lfnds_flow_perf
ctest --test-dir build -C Release -R flow_perf --output-on-failure
```

### Windows 11 (strict no-exception)

Command baseline:

```bash
clang++ -std=c++14 -O3 -fno-exceptions -DFLUEX_FOUNDRY_NO_EXCEPTION_STRICT=1
```

- Host: `Windows 11 Home` (`10.0.26200`)
- CPU: `AMD Ryzen 3700X` (`~3.6 GHz`, x64)
- Memory: `Kingston 3600MHz 32 GB`
- Compiler baseline printed by harness: `clang++ -std=c++14 -O3 -fno-exceptions -DFLUEX_FOUNDRY_NO_EXCEPTION_STRICT=1`

Lower is better (`ns/op`):

| Case | Run #1 | Run #2 | Mean |
|---|---:|---:|---:|
| `direct.loop20` | 0.42 | 0.50 | 0.46 |
| `runner.sync.20nodes` | 18.42 | 19.19 | 18.81 |
| `fast_runner.sync.20nodes` | 1.63 | 1.66 | 1.65 |
| `runner.async.4nodes` | 545.65 | 545.70 | 545.68 |
| `fast_runner.async.4nodes` | 222.95 | 225.26 | 224.11 |
| `runner.when_all.2` | 274.00 | 278.08 | 276.04 |
| `fast_runner.when_all_fast.2` | 148.66 | 147.92 | 148.29 |
| `runner.when_all_fast.2` | 231.18 | 232.17 | 231.68 |
| `runner.when_any.2` | 248.46 | 248.39 | 248.43 |
| `fast_runner.when_any_fast.2` | 123.33 | 121.99 | 122.66 |
| `runner.when_any_fast.2` | 204.35 | 205.19 | 204.77 |

### macOS snapshot (current harness, exception on/off)

Commands:

```bash
# Exceptions ON
c++ -std=c++14 -O3 -DNDEBUG -I. test/flow_perf.cpp -o /tmp/flow_perf_exc

# Exceptions OFF (strict no-exception mode)
c++ -std=c++14 -O3 -DNDEBUG -fno-exceptions -DFLUEX_FOUNDRY_NO_EXCEPTION_STRICT=1 -I. test/flow_perf_noexcept.cpp -o /tmp/flow_perf_noexc
```

Measured output (`2026-02-12`, local machine):

- OS: `Darwin 24.3.0` (`arm64`)
- CPU: `Apple M1 Max` (`10` cores)
- Memory: `64 GB` (`68719476736` bytes)
- Compiler: `clang++ -std=c++14 -O3 -DNDEBUG`
- Harness: auto-calibrated iterations (`>=50ms/round`), `7` rounds, `ns/op` shown as median

Lower is better (`ns/op`):

Commands:

```bash
# Exceptions ON
clang++ -O3 -fstrict-aliasing -mcpu=apple-m1 -std=c++14 -DNDEBUG -I. test/flow_perf.cpp -o /tmp/flow_perf_exc_latest

# Exceptions OFF (strict no-exception mode)
clang++ -O3 -fstrict-aliasing -fno-exceptions -mcpu=apple-m1 -std=c++14 -DNDEBUG -DFLUEX_FOUNDRY_NO_EXCEPTION_STRICT=1 -I. test/flow_perf_noexcept.cpp -o /tmp/flow_perf_noexc_latest
```

- OS: `Darwin` (`arm64`, Apple Silicon)
- CPU target flag: `-mcpu=apple-m1`

Lower is better (`ns/op`, median):

| Case | Exceptions ON | Exceptions OFF |
|---|---:|---:|
| `direct.loop20` | 1.54 | 2.56 |
| `runner.sync.20nodes` | 24.82 | 18.93 |
| `fast_runner.sync.20nodes` | 1.56 | 2.60 |
| `runner.async.4nodes` | 561.66 | 503.97 |
| `fast_runner.async.4nodes` | - | 121.32 |
| `runner.when_all.2` | 291.88 | 270.61 |
| `fast_runner.when_all_fast.2` | - | 85.55 |
| `runner.when_all_fast.2` | 215.89 | 210.83 |
| `runner.when_any.2` | 271.50 | 251.73 |
| `fast_runner.when_any_fast.2` | - | 67.37 |
| `runner.when_any_fast.2` | 187.93 | 186.16 |

Notes:

- This is a microbenchmark for framework overhead, not an end-to-end application benchmark.
- PC and macOS snapshots use different measurement harness/reporting formats.
- `fast_runner` still shows the lowest sync pipeline overhead in both environments.
- Absolute values depend on CPU/compiler/flags and runtime load.

## 🧪 Stress validation snapshot

Command:

```bash
cmake -S . -B build -DFLUX_FOUNDRY_BUILD_TESTS=ON
cmake --build build --config Release --target lfnds_flow_state_stress
ctest --test-dir build -C Release -R flow_state_stress --output-on-failure
```

Result summary (local run):

- ✅ `[PASS] state-machine stress passed`
- `async cancel race`: 5000/5000 completed, `timeout=0`, `duplicate_callback=0`
- `submit fail path`: 5000/5000 completed, all classified as submit-fail as expected
- `when_all` matrix: `normal/cancel`, `normal/no_cancel`, `fast/cancel`, `fast/no_cancel`
- `when_any` matrix: `normal/cancel`, `normal/no_cancel`, `fast/cancel`, `fast/no_cancel`

## 📁 Repository layout

```text
base/
CMakeLists.txt
executor/
flow/
memory/
task/
utility/
test/
test/CMakeLists.txt
test/bin/      # generated probe executables
README.md
```

## 📦 Requirements

- C++14 compiler (Clang/GCC/MSVC)
- CMake 3.16+

## 📌 Notes

- Core library remains header-only.
- Probe/stress/perf sources live in `test/` (`*.cpp`).
- Generated binaries are written to `test/bin/`.

## 📜 License

MIT