# <img src="logo_thumbnail.jpg" width="50" height="50" valign="middle"/> flux_foundry

> Lightweight C++14 foundations for async flow orchestration, lock-free queues, and low-level memory building blocks.

## ✨ Overview

`flux_foundry` is a **header-only** C++14 library that combines:

- ⚡ Low-overhead flow pipeline execution (`flow_runner`, `flow_fast_runner`)
- 🔁 Flow DSL operators (`transform`, `then`, `on_error`, `catch_exception`, `via`, `await`, `await_when_all`, `await_when_any`, `await_when_all_fast`, `await_when_any_fast`)
- 🧠 Memory/data primitives (`result_t`, `either_t`, `flat_storage`, `lite_ptr`, `inplace_t`, `pooling allocator`)
- 🧵 Queue/executor infrastructure (`spsc/mpsc/mpmc/spmc`, `simple_executor`, `gsource_executor`)

The project is tuned for predictable behavior and explicit contracts under concurrency.

## 🎯 Positioning

- `flux_foundry` is built for low-latency C++ systems where predictable behavior, explicit concurrency contracts, and composable flow semantics matter more than beginner ergonomics.
- Best-fit scenarios: systems infrastructure, trading/market-data style pipelines, and custom backend integration (for example io_uring/DPDK-style poll loops via awaitable + executor contracts).
- Hot-path focus: `fast_runner` + sync/inline execution targets near hand-written overhead in benchmarked scenarios.
- Tradeoff: the library expects strong C++ template/concurrency fluency; it is a power-user DSL, not a beginner abstraction layer.

## 🧩 Components

| Module | Main files                                                                                                               | What it provides |
|---|--------------------------------------------------------------------------------------------------------------------------|---|
| `flow/` | `flow_node.h`, `flow_blueprint.h`, `flow_runner.h`, `flow_async_aggregator.h`, `flow_awaitable.h`                        | Pipeline DSL (`transform/then/on_error/catch_exception/via/await`), node graph flattening, async steps, `when_all/when_any`, cancel/error propagation |
| `extension/` | `external_async_awaitable.h`, `cuda_awaitable.h`                                                                      | Generic external-async awaitable contract (`await_external_async`); CUDA naming kept as compatibility alias |
| `executor/` | `simple_executor.h`, `gsource_executor.h`                                                                                | MPSC single-consumer executor, GLib source-backed executor |
| `utility/` | `concurrent_queues.h`, `callable_wrapper.h`, `back_off.h`                                                                | Lock-free queues, callable type-erasure with SBO, backoff policies |
| `task/` | `task_wrapper.h`, `future_task.h`                                                                                        | Task wrappers and future-related task abstraction |
| `memory/` | `result_t.h`, `either_t.h`, `flat_storage.h`, `lite_ptr.h`, `inplace_t.h`, `padded_t.h`, `hazard_ptr.h`, `pooling.h` | Result/error transport, tagged unions, storage composition, smart pointer, inplace construction helpers, padding/alignment primitives |
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
ctest --test-dir build -C Release -L demo   --output-on-failure
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
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | then([](result_t<int, E>&& in) noexcept -> result_t<int, E> {
            if (!in.has_value()) {
                return result_t<int, E>(error_tag, std::move(in).error());
            }
            return result_t<int, E>(value_tag, in.value() * 2);
        })
        | on_error([](result_t<int, E>&&) noexcept -> result_t<int, E> {
            return result_t<int, E>(value_tag, -1); // recover from non-exception errors
        })
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
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

### `await_when_all` minimal demo (compilable)

```cpp
#include "flow/flow.h"
#include <cstdio>
#include <utility>

using namespace flux_foundry;
using E = flow_async_agg_err_t;
using out_t = result_t<int, E>;

struct print_receiver {
    using value_type = out_t;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            std::printf("when_all value=%d\n", r.value());
        } else {
            std::printf("when_all error\n");
        }
    }
};

int main() {
    auto leaf1 = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 10; })
        | end();

    auto leaf2 = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 20; })
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_all(
        [](int a, int b) noexcept {
            return out_t(value_tag, a + b);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1,
        p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, print_receiver{});
    runner(1, 2); // prints: when_all value=33
}
```

### `await_when_any` minimal demo (compilable)

```cpp
#include "flow/flow.h"
#include <cstdio>
#include <utility>

using namespace flux_foundry;
using E = flow_async_agg_err_t;
using out_t = result_t<int, E>;

struct print_receiver {
    using value_type = out_t;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            std::printf("when_any winner value=%d\n", r.value());
        } else {
            std::printf("when_any error\n");
        }
    }
};

int main() {
    auto leaf1 = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 100; })
        | end();

    auto leaf2 = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 200; })
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_any(
        [](size_t i, int x) noexcept {
            (void)i; // winner index
            return out_t(value_tag, x);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1,
        p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, print_receiver{});
    runner(1, 2); // winner value is usually 101 or 202
}
```

## 🏗 Architecture sketch

High-resolution export (architecture + sequence in one SVG): [`architecture_overview.svg`](architecture_overview.svg)

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'fontSize': '16px',
    'lineColor': '#475569',
    'primaryTextColor': '#0f172a',
    'clusterBorder': '#64748b',
    'clusterBkg': '#f8fafc'
  },
  'flowchart': {
    'nodeSpacing': 60,
    'rankSpacing': 70,
    'padding': 20
  }
}}%%
flowchart TB
  subgraph LAYERS[" "]
    direction LR

    subgraph DSL["DSL Layer (Public API)"]
      D0["make_blueprint()"]
      D1["operators:<br/>transform / then / on_error / catch_exception / via / await / await_when_all / await_when_any / end"]
      D2["output blueprint:<br/>flow_blueprint(I, O, Nodes...)"]
      D0 --> D1 --> D2
    end

    subgraph IMPL["Internal Implementation Layer"]
    direction LR

    subgraph ASYNC_MOD["Module 3: Async/Cancel Control"]
      A0["awaitable_factory / aggregator_awaitable_factory"]
      A1["awaitable_base / fast_awaitable_base<br/>external_async_awaitable (extension)<br/>access_delegate: submit_async / resume"]
      A2["flow_controller state machine<br/>(normal runner only)"]
      A3["when_all / when_any awaitables:<br/>launch sub-runners + merge/winner"]
      A0 --> A1
      A3 --> A0
      A2 -. cancel .-> A1
    end

    subgraph BP_MOD["Module 1: BP Builder"]
      B0["node wrappers:<br/>calc / via / async / end"]
      B1["build rules:<br/>calc+calc fusion -> zipped_callable (MAX_ZIP_N)<br/>via+via keeps newest via<br/>async+via forbidden, end is terminal"]
      B2["normalized storage:<br/>flat_storage + node tags<br/>end node fixed at index 0"]
      B0 --> B1 --> B2
    end

    subgraph RUN_MOD["Module 2: Runner Engine"]
      R0["flow_runner:<br/>normal path (cancel-aware)"]
      R1["flow_fast_runner:<br/>fast path (no flow_controller)"]
      R2["shared execution core:<br/>ipc(N-1) + node_tag dispatch"]
      R0 --> R2
      R1 --> R2
    end

    subgraph CORE_MOD["Module 4: Runtime Core"]
      C0["task_wrapper + callable_wrapper<br/>continuation transport (SBO/type-erasure)"]
      C1["result_t / lite_ptr / flat_storage<br/>typed payload and storage"]
      C2["pooling allocator + queue/static_stack primitives"]
      C0 --> C1 --> C2
    end
  end
  end

  subgraph EXT["External Integration"]
    X0["external executor:<br/>simple_executor / gsource_executor / custom"]
    X1["result sink:<br/>receiver.emplace(result_t(T, E))"]
    X2["optional fork template:<br/>flow/flow_fork_receiver_tmp.h<br/>fork_receiver&lt;Derived, FromBP, To...&gt; + forward(copy result_t)"]
  end

  D2 -->|"operator| normalization -> runnable graph"| B2
  B2 -->|"node tags + storage"| R0
  B2 -->|"node tags + storage"| R1
  R0 -->|"ipc start"| R2
  R1 -->|"ipc start"| R2
  R2 -->|"await/aggregate factory call"| A0
  A1 -->|"resume(result_t) -> continue ipc"| R2
  R0 -->|"register/drop cancel handler"| A2
  R2 -->|"continuation/payload/core primitives"| C0
  R2 -->|"via/non-inline continuation submit"| X0
  X0 -->|"execute task and re-enter runner"| R2
  R2 -->|"final result delivery"| X1
  X1 -. "optional fan-out" .-> X2
  X2 -->|"start downstream runner(s) + optional join bp"| R2

  classDef big font-size:16px,stroke-width:1.8px,padding:12px;
  class D0,D1,D2,B0,B1,B2,R0,R1,R2,A0,A1,A2,A3,C0,C1,C2,X0,X1,X2 big;

  style LAYERS fill:transparent,stroke:transparent,stroke-width:0px
  style DSL fill:#eef6ff,stroke:#2563eb,stroke-width:3px
  style IMPL fill:#f8fafc,stroke:#334155,stroke-width:3px
  style EXT fill:#ecfeff,stroke:#0891b2,stroke-width:3px
  style BP_MOD fill:#ffffff,stroke:#64748b,stroke-width:2px
  style RUN_MOD fill:#ffffff,stroke:#64748b,stroke-width:2px
  style ASYNC_MOD fill:#ffffff,stroke:#64748b,stroke-width:2px
  style CORE_MOD fill:#ffffff,stroke:#64748b,stroke-width:2px
```

```mermaid
sequenceDiagram
  participant U as User Code
  participant D as DSL/BP Builder
  participant R as flow_runner / flow_fast_runner
  participant A as Factory+Awaitable
  participant X as External Executor
  participant C as flow_controller
  participant S as Receiver

  U->>D: build pipeline by operator chain
  D-->>U: runnable blueprint (end at index 0)
  U->>R: make_runner(bp, receiver)<br/>or make_fast_runner(...)
  U->>R: operator()(input...)
  R->>R: ipc(N-1) + node_tag dispatch

  alt calc segment
    R->>R: inline eval, then ipc(I-1)
  else via / non-inline continuation
    R->>X: dispatch(task_wrapper)
    X-->>R: run continuation
  else await / when_all / when_any
    R->>A: factory(input result_t)
    A-->>R: access_delegate
    opt normal runner
      R->>C: lock_and_set_cancel_handler(...)
    end
    R->>A: submit_async()
    A-->>R: resume(result_t) exactly once
    R->>R: ipc<I-1> continue
  end

  R->>S: emplace(result_t(T, E))

  opt fork/fan-out receiver path
    S->>R: receiver.forward(copy result_t) starts downstream runner(s)
    R->>R: run sub-bp and optional join bp
    R->>S: deliver downstream result
  end

  opt external cancel (normal runner path)
    U->>C: cancel(soft/hard)
    C->>A: cancel handler
    A-->>R: resume(cancel_error)
  end
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

### Fork pattern (template reference)

- `flux_foundry` does not enforce a single fork topology API because downstream start/join strategy is user-defined.
- If you need fork-style fan-out, use the template helper in `flow/flow_fork_receiver_tmp.h` and implement `forward(result_t<T, E>&&) noexcept` in your derived receiver.
- Base template shape: `fork_receiver<Derived, FromBP, To...>`; `Derived::forward(...)` is where you copy/route payload and start downstream runners.
- A complete fork+join reference implementation is in `test/flow_fork_join_semantics_test.cpp` (normal runner, fast runner, and cancel/error branches).

### Fast async lane

- `fast_awaitable_base`:
  - designed for throughput-first async path
  - no awaitable internal state-machine CAS on `submit/resume`
  - contract-based: if `submit()==0`, backend must guarantee `resume()` is called exactly once; if `submit()!=0`, backend must not call `resume()` afterward
- `await_when_all_fast(...)` / `await_when_any_fast(...)`:
  - use fast aggregator awaitables (`flow_when_all_fast_awaitable`, `flow_when_any_fast_awaitable`)
  - run subgraphs with `fast_runner`
  - fast path does not register `flow_controller` cancel handlers
- Recommended combinations:
  - max throughput: `fast_runner + fast awaitable/fast aggregator`
  - full cancellation semantics: `flow_runner + awaitable_base/normal aggregator`

## 📊 Benchmark snapshots

Microbenchmark tables and run notes now live in [`bench/README.md`](bench/README.md) to keep this `README` focused.

That document includes:
- `flow_perf` snapshots
- `flux_foundry` vs standalone Asio horizontal compare
- real-backend async overhead snapshots
- macOS / Windows / Linux host notes and command blocks

For the standalone horizontal-compare harness itself, see [`bench/horizontal_compare/README.md`](bench/horizontal_compare/README.md).

## 🧪 Stress validation snapshot

Command:

```bash
cmake -S . -B build -DFLUX_FOUNDRY_BUILD_TESTS=ON
cmake --build build --config Release --target flux_foundry_flow_state_stress
ctest --test-dir build -C Release -R flow_state_stress --output-on-failure
```

Result summary (local run):

- ✅ `[PASS] state-machine stress passed`
- `async cancel race`: 5000/5000 completed, `timeout=0`, `duplicate_callback=0`
- `submit fail path`: 5000/5000 completed, all classified as submit-fail as expected
- `when_all` matrix: `normal/cancel`, `normal/no_cancel`, `fast/cancel`, `fast/no_cancel`
- `when_any` matrix: `normal/cancel`, `normal/no_cancel`, `fast/cancel`, `fast/no_cancel`

## 🔬 Formal model checking (TLA+)

In addition to stress/TSAN validation, the repository includes small **TLA+** models for key concurrency protocols in `test/model/`:

- `flow_controller` cancel-state protocol
- `simple_executor` ticket accounting / shutdown-drain
- `awaitable_base` / `fast_awaitable_base` callback lifecycle
- `flow_runner` async-node handshake path
- `flow_async_aggregator` (`when_all` / `when_any`, normal + fast)

These are **abstract protocol models** (not line-by-line C++ proofs) and are intended to complement runtime stress tests and sanitizers.

Run with TLC (example, from repo root):

```bash
java -cp /path/to/tla2tools.jar tlc2.TLC -config test/model/FlowControllerCancel.cfg test/model/FlowControllerCancel.tla
java -cp /path/to/tla2tools.jar tlc2.TLC -config test/model/SimpleExecutorTickets.cfg test/model/SimpleExecutorTickets.tla
```

See `test/model/README.md` for the full model list, assumptions, and additional TLC commands.

## 📁 Repository layout

```text
base/
CMakeLists.txt
executor/
extension/
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
- CUDA runtime/image backends are shipped as demo targets (`ctest -L demo`).
- Generated binaries are written to `test/bin/`.

## 📜 License

MIT
