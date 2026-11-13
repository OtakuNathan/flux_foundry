# Move only the received packet prefix

`dpdk_packet_batch` now copies only `[0, size())` on move and leaves unused
pointer slots uninitialized. Previously, every receive zeroed the whole array
and every move copied its full capacity. Packet ownership, partial TX and
cancellation semantics are unchanged. Existing API `static_assert` checks
continue to enforce the capacity, packet type, signatures and `noexcept` calls.

Both comparison versions include the preceding runner-factory move fix.
Capacity remains 32, while the software ring contains 1, 4, 16 or 32 packets.
These are native DPDK net_ring PMD measurements on the 3700X, pinned to CPU 3;
they do not involve a physical NIC. Each compiler runs seven alternating
before/after trials, each of 1,048,576 bursts following warmup.

Ordinary public awaitable + fast runner, median ns/burst:

| Packets / capacity 32 | GCC 16 before → after | Change | Clang 23 before → after | Change |
|---|---:|---:|---:|---:|
| 1 | 71.864 → 58.369 | −18.78% | 75.172 → 61.322 | −18.42% |
| 4 | 79.078 → 70.614 | −10.70% | 79.348 → 71.969 | −9.30% |
| 16 | 164.083 → 139.441 | −15.02% | 132.462 → 122.273 | −7.69% |
| 32 | 314.321 → 303.455 | −3.46% | 256.329 → 245.757 | −4.12% |

The strongest repeatable effect is on sparse bursts. Full-burst ranges overlap,
and even the raw baseline shifts by about 1.6–2.4% there; the full-burst median
change should not be read as a guaranteed speedup. Natural code layout is
retained. All five lanes, including raw, batch, context and normal runner, are
reported in [the full summary](summary.md). Clang normal-runner occupancy 16
is essentially unchanged (+0.42%); this is not a uniform gain across workloads.

## Validation

The fake-API contract now exercises empty, partial and full batch move chains,
assignment over existing ownership, self-move and moving the unsent TX tail.
Every allocated packet must be either freed or accepted exactly once. GCC C++14
normal and strict no-exception/no-RTTI builds pass, as do Clang UBSan and the
native DPDK standalone-header compile. Clang MSan with origin tracking passes
the strict no-exception contract.

The initial exception-enabled MSan run with system libstdc++ fails in
`std::exception_ptr` destruction, on both the old and new packet headers. A
[six-line FF-free reproduction](tests/msan-exception-minimal.cpp) fails in the
same place. In this libstdc++ C++14 header, `make_exception_ptr` calls the
out-of-line `exception_ptr(void*)` constructor; the uninstrumented runtime
writes the pointer without updating MSan shadow memory. This is a test-runtime
boundary, not evidence of an uninitialized FF packet slot. See the
[minimal report](tests/msan-exception-minimal.log) and complete compressed
[before](tests/msan-before.log.gz) / [after](tests/msan.log.gz) reports.

MSan requires instrumented dependencies; Clang documents the use of a
self-built instrumented libc++ in its
[external-code guidance](https://clang.llvm.org/docs/MemorySanitizer.html#handling-external-code).
No sanitizer suppression or production `unpoison` workaround is added.

Rechecking with MSan-built LLVM 20.1.8 libc++ and libc++abi resolves the report:
the minimal program and both before/after exception-enabled FF contracts exit
0. A deliberate heap-uninitialized-read control still fails with the expected
MSan diagnostic (exit 1). This confirms the runtime-boundary diagnosis for this
report. [Runtime build and recheck commands](tests/msan-runtime.md) and
[the passing after-contract log](tests/msan-libcxx-after.log) are retained.

## Reproduction

[Raw timings](results.csv) contain 560 rows. [Environment metadata](environment.json)
records all four compile commands and hashes of the compared packet headers
and common runner header. The original DPDK dependency/compiler environment
is described in the sibling benchmark snapshots.

```sh
python3 bench/io_adapters/compare_batch_prefix.py \
  --before /path/to/before-dpdk_awaitable.h \
  --after extension/dpdk_awaitable.h --runner flow/flow_runner.h \
  --references build/io-perf-dpdk build/io-perf-dpdk-gcc \
  --out build/batch-prefix-final --count 1048576 --repeats 7
```

Preserve the original extracted DPDK library environment. The reference
directories supply native compile recipes for the same host and checkout.
The desktop comparison and saved baseline headers remain under
`/tmp/ff-io-perf-vFZ6HVwW/`.

Contract builds use `-std=c++14 -O1 -g -pthread -Wall -Wextra -Werror`, the
after-header overlay and the DPDK pkg-config include flags. Strict builds add
`-fno-exceptions -fno-rtti -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1`; UBSan adds
`-fsanitize=undefined -fno-sanitize-recover=all`; MSan adds
`-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer
-fPIE -pie`. Fake-API tests do not call or link the native DPDK runtime.
Logs are saved under [tests](tests/). The local Pi's optional-adapter build
was unavailable after a reboot removed its temporary dependencies, so these
DPDK validation results are from the desktop.
