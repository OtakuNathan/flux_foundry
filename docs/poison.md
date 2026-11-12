# Debug lifetime checks

Include `flow/flow.h` or the standalone `flow/flow_poison.h` (C++14).
`FLUX_FOUNDRY_ENABLE_POISON` defaults to `FLUX_FOUNDRY_ENABLE_VACCINE`, which
is off under `NDEBUG`. Configure these macros consistently across translation
units. Position markers remain while either facility is enabled.

A `poison_scope` represents one logical usage window, not ownership. Create a
fresh scope per request/window, never per shared blueprint. Declare a stack
scope after the object it observes, so invalidation precedes object destruction.

```cpp
buffer storage;
flux_foundry::poison_scope window({"request buffer", __FILE__, __LINE__});
auto borrowed = window.borrow(storage, {"queued reader", __FILE__, __LINE__});
// Capture borrowed by value in the queued callable.
consume(*borrowed);
window.invalidate({"request retired", __FILE__, __LINE__});
// Further *borrowed or borrowed->... diagnoses an expired borrow.
```

The scope destructor also invalidates. All references from one scope share the
same qualification. Repeated invalidation preserves the first location. Every
new scope has independent metadata, including when a pooled object's address is
reused. Metadata survives with references; the business object does not.

`poison("last use", selector)` is a pipeline observer: a const-callable,
noexcept selector accepts the complete `const result_t&` and returns a
`poison_token`. Use `poison_token_of(result.value())` to select a borrowed
value; return an empty token for error results when appropriate. The observer
invalidates that token and forwards the result without copying its payload.
Downstream consumers must respect the marked end of the borrow. A hard cancel
may skip the observer; real lifetime boundaries must still invalidate.

## Backend completion

A scope's `submission(position)` creates a new copyable `poison_submission`.
Wrap the backend callback with `FLUX_FOUNDRY_POISON_CALLBACK(check, callback)`,
record the backend's
return code with `check.submitted(code)`, and optionally call `check.quiesce()`
after an actual drain/join. Wrapped callbacks must return void and be noexcept;
move-only captures are supported. Copies share one completion state.

```cpp
auto check = lifetime.submission({"read completion", __FILE__, __LINE__});
auto callback = FLUX_FOUNDRY_POISON_CALLBACK(check, [this]() noexcept {
    auto result = collect();
    resume(std::move(result));
});
return check.submitted(backend.submit(std::move(callback)));
```

Keep existing backend retain/release rules. For an instrumented owner, use the
scope as an empty base or compressed storage; invalidate at the beginning of
its destructor, before destroying observed members. Scope destruction is a
fallback. The local submission handle survives inline destruction of the owner.

Checks catch duplicate/reentrant completions, completion after rejection,
completion followed by a rejected return, and callbacks after scope retirement.
Only explicit quiescence diagnoses a successful submission with no completed
callback. Canceling a normal FF awaitable does not retire the backend object or
count as a backend callback: a properly retained late completion remains legal.
This does not validate `completes_inline` declarations or instrument arbitrary
runners automatically. C callback userdata must retain the callable wrapper
until the backend can no longer access it; a pointer to a freed wrapper cannot
be checked safely.

## Diagnostics and limits

The default hook prints the `poison_failure` description, origin, binding, and first
invalidation location, then aborts. A scope can receive a custom
`void(const poison_violation&)` hook. Hooks must not throw; returning still
aborts. Names/files must outlive all diagnostic handles; literals are suitable.
Operator overloads report the binding location, not an inferred call-site line.
Metadata allocation failure terminates rather than disabling checks silently.

Checking does not synchronize or own the observed object. An escaped raw
reference and a destruction race after a successful check remain outside its
coverage. Debug metadata synchronization can change interleavings, so run
sanitizers and lifecycle tests with checks both enabled and disabled.

When disabled, `poison_ref<T>` aliases `T*`, scopes/submission handles are empty,
the callback macro expands directly to the original callback expression, and
`operator|` erases poison nodes
before fusion. Ordinary empty members may still occupy space: use an empty base
or existing `compressed_pair`; use the existing `TS_EMPTY_BASES` annotation
for owners with multiple bases. Argument expressions still follow C++ evaluation
rules. Do not put business side effects in debug selectors.

`check.wrap(callback)` is also available as a value-building function. It returns
an owning callable by value, so a nontrivial callback can incur a move even when
checks are disabled. Use the macro for strict erasure: it avoids that extra
materialization without returning a reference to a temporary. A move-counter
regression verifies the disabled macro against the original expression.

## Initial validation (2026-11-12)

Compared against `67e5365`, existing `flow_perf` and strict no-exception
executables are byte-identical after removing symbols/build IDs: ARM GCC 14,
and Ryzen 3700X GCC 16 / Clang 23. The dedicated marked/plain erasure probe
also matches on ARM GCC 14 / Clang 19 and x86 GCC / Clang. These are concrete
build configurations, not a guarantee for every toolchain and application.

On the 3700X, seven CPU-3-pinned alternating pairs of the existing performance
matrix gave median paired changes between -0.37% and +0.37% with exceptions,
and -0.24% and +0.12% without. Since each pair uses byte-identical executables,
these differences are measurement variation, not changes in executed code.

ARM also completed seven alternating CPU-3-pinned pairs per mode. Median paired
changes ranged from -6.66% to +1.40% with exceptions and -0.86% to +2.19%
without. The unchanged executable bytes rule out new generated instructions;
these runs show greater execution-environment variation, not an established
speedup or a code regression. Raw process samples are retained with the review
artifacts rather than treating one timing ratio as a guarantee.

A separate optimized synchronous probe creating a fresh scope and submission,
borrowing one integer, invoking a callback, and declaring quiescence measured
82.57 ns/iteration enabled versus 1.77 ns disabled (medians of three runs).
This includes diagnostic metadata allocation; it is not a multithreaded backend
latency estimate. Debug instrumentation deliberately has a cost.

The x86 smoke/stress suite passed 94 tests; ARM passed 97 (including three
local Portal integration tests). Each host also passed 100 repetitions of the
58-scenario lifecycle suite in enabled, disabled, and no-exception configurations. Poison positive/negative cases and
pipeline lifecycle schedules passed ASan/UBSan; positive poison and pipeline
schedules passed TSan with checks enabled and disabled. ARM's ASan runtime could
not reserve its address space in this environment; that startup failure is not
counted as a passing test. ARM UBSan and a TSan positive probe ran successfully.
