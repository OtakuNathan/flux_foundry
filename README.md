# lite_fnds

**lite_fnds** — a lightweight C++14 foundations library for zero-overhead, composable system components.

This project started as a personal experiment to understand the fundamentals of C++ systems programming — and has grown into a small but expressive collection of low-level building blocks: memory primitives, lock-free concurrency pieces, task abstractions, and a static “Flow” pipeline engine.

---

## 🧩 Modules

| Category | Components |
|-----------|-------------|
| **Base** | `inplace_base`, `traits`, `type_erase_base` |
| **Memory** | `inplace_t`, `either_t`, `result_t`, `static_mem_pool`, `hazard_ptr` |
| **Concurrency** | `spsc_queue`, `mpsc_queue`, `mpmc_queue` |
| **Utility** | `compressed_pair`, `callable_wrapper`, `static_list` |
| **Task** | `task_core`, `future_task`, `task_wrapper` |
| **Flow** | `flow_blueprint`, `flow_node`, `flow_runner` |

---

# 🚀 Flow — Static Execution Blueprints (Core Highlight)

`lite_fnds::flow` is a **zero-overhead, compile-time optimized execution pipeline**, designed around three ideas:

## **1. Blueprint — Pipelines as Static Structures**

Instead of building dynamic chains (`std::function`/virtual dispatch),  
Flow compiles the entire pipeline **into a static blueprint**:

- All nodes exist as a `std::tuple`
- Node types are known at compile time
- The pipeline is fully type-checked and exception-safe
- No heap allocations
- No dynamic polymorphism

A blueprint is simply:

```cpp
auto bp = make_blueprint<int>()
        | map(...)
        | then(...)
        | on_error(...)
        | via(...)
        | end(...);
```

## **2. Node Fusion — Compile-Time Optimization**

Flow distinguishes two kinds of nodes:
* calc nodes (value transformations)
* control nodes (thread dispatch, scheduling, fan-out)

During blueprint construction, Flow performs automatic fusion:
* Multiple calc nodes merge into one giant callable (zero dispatch overhead)
* Control nodes override previous control points to avoid nonsensical chains
* The final blueprint is minimized and efficient

This means:
- A pipeline:
 <br/> `calc + calc + calc + control + calc + calc + control ... + 1 end`
 <br/> becomes 
 <br/> `calc + control + calc + ... + 1 end`.

Execution stays fast and stack-friendly.

## **3. Runner — A Tiny Execution Engine**

A flow_runner only stores:
* pointer to the blueprint
* pointer to the controller (for cancellation)

It is extremely lightweight:

```cpp
auto runner = make_runner(bp);
runner(42);  // start execution
```

✔ Soft & Hard cancellation
* Soft cancel: finishes the current stage and terminates at the next control boundary
* Hard cancel: jumps directly to the end node with a cancel error

✔ End node = result sink
* `end(...)` is the only place side-effects are guaranteed.
* You can branch/fan-out here safely without corrupting blueprint semantics.

✔ Exception-safe pipeline
Any exception inside a node is captured as std::exception_ptr and forwarded.
```cpp
auto bp = make_blueprint<int>()
    | map([](int x) noexcept { return x + 10; })
    | then([](auto r) {
        if (r.value() > 20) throw std::logic_error("too big");
        return r;
    })
    | catch_exception<std::logic_error>([](const auto& e) {
        return -1;      // recover value
    })
    | end([](auto r) {
        std::cout << "result: " << r.value() << "\n";
        return r;
    });

auto runner = make_runner(bp);
runner(5);  // -> prints "result: -1"
```

⚙️ Build
Almost header-only — no special build steps.
 * Requires C++14
 * No dependencies other than the C++ Standard Library

💡 Design Philosophy

lite_fnds aims to stay:

* ***Lightweight*** — minimal runtime overhead and zero heap allocation where possible
* ***Safe*** — strong ownership, clear invariants, explicit lifetime control
* ***Composable*** — modules work independently or combine naturally
* ***Predictable*** — no surprising behavior, strong noexcept discipline
* ***Executable*** — Flow blueprints encode logic at compile time, runners execute with minimal runtime cost

📄 License

MIT License © 2025 Nathan