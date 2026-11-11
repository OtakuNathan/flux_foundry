# Pipeline checkpoints and lifecycle instrumentation

## Implemented: position and vaccine

`flow/flow.h` exposes debug checkpoints; `flow/flow_contract.h` can also be
included directly. A vaccine observes the **whole current result** as a const
reference, including error results, and returns `bool`. It does not consume the
result, change the error channel, or move/copy the payload to inspect it.

```cpp
auto bp = flux_foundry::make_blueprint<int>()
    | flux_foundry::position("decode", flux_foundry::transform(
        [](int x) noexcept { return x + 1; }))
    | FLUX_FOUNDRY_POSITION("decoded response")
    | flux_foundry::vaccine("positive decoded value", [](const auto& result) noexcept {
        return result.has_error() || result.value() > 0;
    })
    | flux_foundry::end();
```

`position("name")` marks a source location for subsequent checkpoints, until the
next position. `position("name", node)` puts that marker immediately before a
builder such as `transform(...)`, `via(...)`, or `await<...>(...)`.
`FLUX_FOUNDRY_POSITION("name")` also records the source file and line. A vaccine
without a position reports `unpositioned`. Positions are captured when composing
the blueprint; they survive node fusion and executor handoffs without thread-local
"current position" state. Branch blueprints have their own positions.

`vaccine("contract name", predicate, hook)` accepts an optional diagnostic hook
with signature `void(const flux_foundry::contract_violation&)`. Names and source
strings must outlive every execution of the blueprint; string literals are a safe
default. The hook must not throw. A false predicate or, with exceptions enabled,
a throwing predicate calls the hook and then aborts even if the hook returns.
The default hook writes the contract, position, file/line and reason to stderr.
`vaccine(predicate)` supplies a default contract name. Predicates must be const
callable and return exactly `bool`; no-exception builds require a noexcept call.
Const references prevent direct mutation/consumption of the result, but do not
provide deep immutability of pointees or synchronize user-captured state.

Checks default on when `NDEBUG` is absent. Define
`FLUX_FOUNDRY_ENABLE_VACCINE=0` or `=1` consistently across translation units to
override this. Disabled markers are removed during blueprint composition before
fusion: no stored predicate, position, per-run allocation, atomics or branch.
Argument expressions are still evaluated by C++ when constructing the blueprint;
put expensive runtime inspection inside the predicate. Tests verify identical
blueprint types/sizes and, on Linux GCC/Clang, byte-identical optimized executables
(stripped symbols, no build ID) with and without disabled markers.

Checks follow normal runner cancellation rules: hard cancellation can skip them
and jump to the terminal node; soft cancellation may present an error result.
Enabling checks can affect scheduling, code layout and fusion boundaries. They
are not a replacement for TSan or a detector for a callback that dereferences an
already freed awaitable. Library/backend lifetime testing remains independent.

## Planned: backend boundary instrumentation

The remaining design below is not an implemented API. It concerns protocol
instrumentation beyond the user-authored result checkpoints above.

Potential integration points for backend protocol checks:

- An external decorator around a runner or awaitable integration.
- Blueprint markers/checkpoints composed with the existing pipe syntax, e.g.
  `make_blueprint() | then(...) | vaccine(...) | poison(...) | end(...)`.

Read-only value/error inspection is implemented above. A separate protocol layer
could connect source markers to independently owned awaitable debug tokens.
Any future poison marker, scoped invariant syntax, or implicit backend wrapping
remains an open design question.

For fast runner/awaitable contracts, markers must instrument the submission and
completion boundaries during blueprint construction. A downstream calculation
node alone cannot safely diagnose a duplicate callback upstream.

Candidate checks:

- Reentrant invocation and reuse after an asynchronous runner handoff. Preserve
  existing legal synchronous reuse; do not invent a blanket one-shot rule.
- Exactly one terminal completion; success and cancel must not both complete.
- No callback on failed submission, including a callback followed by failure.
- Legal inline completion before submit returns; validate an advertised
  completes-inline property without mistaking inline completion for a violation.
- Explicit ownership/lifetime checks for borrowed blueprint views where the
  owner participates in instrumentation.

The first resume can destroy the awaitable. A late/duplicate callback must reach
an independently owned debug token before accessing the real object. A flag
inside a freed awaitable cannot diagnose use-after-free safely. Specify token
retention and teardown so instrumentation does not silently repair the ownership
bug it intends to expose. Missing completion can only be diagnosed at an explicit
quiescence/teardown boundary, not inferred from an arbitrary timeout.

Use a compile-time policy. Disabled instrumentation should select original fast
types and erase markers before node fusion: no extra state, atomics, allocation,
or runtime branch. Enable by default for the chosen debug configuration; allow
optimized builds to opt in. Keep translation-unit configuration consistent.
Report logical source-node positions even when calculation nodes are fused.
A noexcept diagnostic hook may log and terminate; do not require exceptions or
silently swallow a failed runtime contract. Business-error routing, if supported,
needs an explicit compatible error-channel policy.

Validation before shipping:

- Positive cases: synchronous and asynchronous completion, submission rejection,
  inline completion, normal fast-runner handoff, move-only values, error results.
- Deterministic negative cases: duplicate completion, callback after rejection,
  false inline declaration, reentry, consumed-runner reuse, invalid borrowed view.
- ASan/UBSan/TSan, including callbacks after public wrapper destruction. The
  checker is not a replacement for memory-publication correctness or sanitizers.
- Compare disabled size/code generation and allocations with the unwrapped path.
- Verify markers preserve executor, cancellation and completion semantics.

## Composition scenario: an RPC flow inside a business flow

An outer blueprint can await Host A, await Host B, then calculate or aggregate
results. Each await can wrap an inner local blueprint that encodes a Portal
request, submits asynchronous I/O, awaits its response, and decodes the result.
The remote host is an RPC destination, never a remote FF executor. Each layer
owns its operation state and completes its parent exactly once. Cancellation or
a local timeout does not prove the remote operation stopped; preserve explicit
unknown-outcome results. Instrumentation should retain both logical flow/node
identities so a violation can be attributed to the inner operation and its outer
await boundary. Do not assume nested blueprints are automatically flattened or
allocation-free; measure the actual implementation before making such claims.
