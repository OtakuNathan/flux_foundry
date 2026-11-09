# Choosing a runner and a synchronous hot path

Choose execution semantics first, then ownership and error transport. A strategy
or policy that reads an input and returns a decision can be an ordinary callable
inside a calculation node; it does not need an executor or an awaitable.

## Runner selection

| Need | Entry point | Contract |
| --- | --- | --- |
| Cancellation of an in-flight flow | `make_runner(lite_ptr<Blueprint>, receiver)` | Uses `flow_controller`; cancellation is observed at runner/awaitable boundaries and follows the awaitable's cancellation contract. It cannot preempt an arbitrary synchronous callback. |
| No controller-driven cancellation; own the blueprint | `make_fast_runner(std::move(blueprint), receiver)` | Stores the blueprint by value; no `flow_controller` or its cancellation-handler registration. |
| No controller-driven cancellation; share blueprint ownership | `make_fast_runner(lite_ptr<Blueprint>, receiver)` | Retains the shared blueprint handle. |
| No controller-driven cancellation; borrow an existing blueprint | `make_fast_runner_view(blueprint, receiver)` | Caller keeps the blueprint and everything its callbacks reference alive until completion. |

Fast runners can also execute asynchronous nodes. "Fast" does not imply
synchronous completion or a different error policy. The example below is
synchronous because it contains only calculation nodes and an end node.

Do not invoke the same runner concurrently. A fully synchronous/inline run may
reuse its runner after the call returns. After an asynchronous handoff, treat
the original runner as consumed. A borrowed blueprint must survive *all*
continuations; sharing its storage does not make mutable callbacks thread-safe.
See the [runner contracts](../README.md#flow-runner) and
[`test/runner_reuse_contract.cpp`](../test/runner_reuse_contract.cpp).

## Node selection

| Operation | Callback sees | What happens to an upstream error |
| --- | --- | --- |
| `transform(f)` | The successful `T` (no argument for `void`); returns a new value | Callback is skipped; the error is forwarded. Use for nonthrowing value calculations. |
| `then(f)` | `result_t<T, E>&&`; returns another `result_t<U, E>` | Callback is skipped; the error is forwarded. Use to produce a domain failure or adapt a potentially throwing operation. |
| `on_error(f)` | An error-bearing result | Handle/recover the error, including an `std::exception_ptr` error if that is the chosen channel. |
| `catch_exception<Exception>(f)` | A matching exception | Use for typed exception recovery with an `std::exception_ptr` channel and compiler exceptions enabled. |
| A receiver passed to the runner | The final result, success **or** error | Use to observe completion regardless of outcome. |

Receiving a `result` in `then` does **not** mean its callback runs on both
branches. For example, a failed validation in one `then` skips subsequent
`then` and `transform` callbacks. Put final observation in a receiver, or use
the dedicated error operators to recover.

## Complete example: status codes, inline execution, no cancellation

[`examples/synchronous_status.cpp`](examples/synchronous_status.cpp) is a complete
C++14 program. It validates each input in `then`, maps valid values in
`transform`, and delivers success/error to a receiver through a borrowed fast
runner. There is no executor, asynchronous operation, or controller allocation.

From the repository root, with GCC or Clang on Linux:

```sh
mkdir -p build-docs
c++ -std=c++14 -O2 -DNDEBUG -fno-exceptions \
  -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -I. \
  docs/examples/synchronous_status.cpp -pthread -o build-docs/synchronous_status
./build-docs/synchronous_status
```

Expected output:

```text
value=7
error=invalid_input
value=10
```

The enum error channel and `then` in this example deliberately use the
no-exception configuration. With compiler exceptions enabled, `then` includes
an exception adapter that constructs its output error from
`std::current_exception()`, even when the application callback is `noexcept`.
A plain enum cannot represent that exception. Use `std::exception_ptr` (the
default error type) or an error type constructible from it for that path;
alternatively keep this status-code core in a no-exception translation unit.
Success-only `transform` does not add that exception adapter.

`-fno-exceptions` changes compiler exception support.
`FLUX_FOUNDRY_NO_EXCEPTION_STRICT=1` enables the library's strict no-exception
checks when compiler exceptions are disabled; the macro alone does not turn
compiler exceptions off. Callbacks and value/error transport still have to
meet the relevant nonthrowing contracts. `noexcept` is a promise, not an error
conversion mechanism: violating it terminates the process. Disabling exceptions
also does not make allocation failure recoverable.

The example needs no cancellation/async error customizations. Adding a normal
runner or asynchronous nodes with a custom error type may require
`cancel_error<E>`, `awaitable_creating_error<E>`,
`async_submission_failed_error<E>`, or aggregation error specializations.
The corresponding `static_assert` identifies the required hook and signature;
[`test/noexcept_smoke.cpp`](../test/noexcept_smoke.cpp) shows implementations.
Callable storage has its own [nonthrowing construction and move
contract](builder_storage_contract.md), independent of callback invocation.

## Reading performance results

In a replay integration, a large measured gap involved application-side
exception adapters, compiler inlining, runner selection, and build settings.
Changing the adapter/callback shape and then measuring only the compiler flag
are different experiments. Those results do not establish that enabling
exceptions is inherently slow, or that FF has a fixed overhead percentage.

Compare equivalent work, error semantics, output, compiler/version, optimization,
LTO, and exception configuration. Separate blueprint construction, execution,
allocation, serialization, and runtime-boundary costs. Run matched inputs and
inspect generated code when a result is surprising. See the
[benchmark notes](../bench/README.md) for measured scenarios.
