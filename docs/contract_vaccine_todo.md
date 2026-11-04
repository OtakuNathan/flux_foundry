# TODO: opt-in runtime contract instrumentation

Status: design discussion, not an implemented API. Names and scope syntax are
provisional. This work is independent of the no-exception/libuv changes.

Provide two ways to enable the same contract checker:

- An external decorator around a runner or awaitable integration.
- Blueprint markers/checkpoints composed with the existing pipe syntax, e.g.
  `make_blueprint() | then(...) | vaccine(...) | poison(...) | end(...)`.

A user checkpoint can inspect a borrowed result and validate a domain-specific
precondition or invariant without consuming, copying, or modifying the payload.
Make handling of value/error results explicit; a plain success-only transform
cannot represent every contract check. The precise responsibilities of vaccine
and poison, and whether markers apply to one node or a scoped region, remain open.

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
results. Each await can wrap an inner local blueprint that encodes a Dynabridge
request, submits asynchronous I/O, awaits its response, and decodes the result.
The remote host is an RPC destination, never a remote FF executor. Each layer
owns its operation state and completes its parent exactly once. Cancellation or
a local timeout does not prove the remote operation stopped; preserve explicit
unknown-outcome results. Instrumentation should retain both logical flow/node
identities so a violation can be attributed to the inner operation and its outer
await boundary. Do not assume nested blueprints are automatically flattened or
allocation-free; measure the actual implementation before making such claims.
