# Blueprint builder storage contract

The public node and aggregation factories retain their `noexcept` signatures.
Every callable or executor handle they store must satisfy both of these C++14
requirements, where `Arg` is the factory's deduced forwarding-reference type:

- `std::is_nothrow_constructible<std::decay_t<Arg>, Arg&&>::value`: the actual
  argument can initialize the stored object without throwing.
- `std::is_nothrow_move_constructible<std::decay_t<Arg>>::value`: returning the
  node and moving it into wrappers and blueprint storage cannot throw, including
  when return-value elision is disabled.

This applies to `transform`, `then`, `on_error`, `catch_exception` (when enabled),
`end(F&&)`, `via`, `await(executor)`, and all explicit- and implicit-executor
`await_when_all`, `await_when_all_fast`, `await_when_any`, and
`await_when_any_fast` overloads. Aggregation checks both callbacks and any supplied
executor handle. `await()` and `end()` have no caller-supplied object to check.
`make_blueprint<T, E>()` likewise stores only an internal identity callable.

A callable with a potentially throwing copy constructor and a nonthrowing move
constructor is rejected as an lvalue but accepted as an rvalue. A nonthrowing
initial copy does not make a potentially throwing later move safe. Use
`std::move` only when transferring the object is intended.

Violations produce an explicit compile-time diagnostic, including when return
type deduction is requested in an unevaluated expression. These are hard
contract checks, not a SFINAE-based overload-detection API. Existing callable
invocation and result/error requirements are unchanged, in every exception and
strict/relaxed build mode. This contract does not change `result_t`.

## Where the guarantee starts

Argument expressions are evaluated before entering a factory. For example, a
constructor or an explicit copy in `transform(Callable(source))` may throw at the
call site even if `Callable` then moves safely into storage. Accordingly, the
full call expression need not be `noexcept`. The factory does not catch these
caller-side exceptions. Passing `source` directly instead requests a copy inside
the factory and is rejected when that copy may throw.

This is a construction contract, not a new promise that every blueprint
`operator|` expression is declared `noexcept`, nor a broader execution-time
exception guarantee. Direct use of implementation types under `flow_impl` is
not a substitute for the checked public factories.

## Regression tests

`builder_storage_contract` checks static traits, safe lvalues, move-only and
throwing-copy rvalues, storage/composition paths, and caller-side construction
exceptions. GNU/Clang targets disable elision and repeat the valid checks in
strict and relaxed no-exception modes. `builder_storage_rejection_*` checks
27 factory/argument positions (26 without `catch_exception`) for throwing
lvalue copies, safe lvalue copies followed by throwing moves, and throwing
rvalue moves. The tests match the intended diagnostic rather than accepting
an unrelated compilation failure.
