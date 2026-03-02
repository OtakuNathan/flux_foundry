# horizontal_compare

Microbenchmark entry point for `flux_foundry` vs standalone Asio on the same host and toolchain.
Results are split into two layers:

- `pure scheduling overhead`: isolates scheduling-path cost as much as possible (`via` vs `dispatch/post`)
- `full semantics overhead`: keeps full `result/await/when_all/when_any` semantics in the path

## Run

```bash
bench/horizontal_compare/run.sh
```

On first run, the script auto-fetches Asio into `/tmp/flux_foundry_asio` and does not place third-party code inside this repository.

`noexcept` variant:

```bash
bench/horizontal_compare/run_noexcept.sh
```

3-run consolidated collection (recommended for README snapshots):

```bash
bash bench/horizontal_compare/run_collect_3x.sh
```

This writes one timestamped result file under `bench/horizontal_compare/results/` containing:

- raw output of all 6 runs (`run.sh` x3 + `run_noexcept.sh` x3)
- per-case `run1/run2/run3/median/mean` summary tables

## Current Coverage

- `baseline.direct.loop20`

Pure scheduling overhead:

- `sched.flux.via20.dispatch`
- `sched.asio.dispatch20`
- `sched.flux.via20.post`
- `sched.asio.post20`

Full semantics overhead:

- `full.flux.runner.sync20`
- `full.flux.fast_runner.sync20`
- `full.flux.runner.async4.post`
- `full.flux.fast_runner.async4.post`
- `full.asio.post.async4`
- `full.flux.runner.when_all2.post`
- `full.flux.fast_runner.when_all2.post`
- `full.flux.fast_runner.when_all2.fastagg.post`
- `full.asio.post.when_all2`
- `full.flux.runner.when_any2.post`
- `full.flux.fast_runner.when_any2.post`
- `full.flux.fast_runner.when_any2.fastagg.post`
- `full.asio.post.when_any2`

Real backend async overhead (noexcept matrix):

- `real.flux.fast_runner.fast_awaitable.async4.backend`
- `real.asio.raw.async4.backend`
- `real.flux.fast_runner.fast_awaitable.async1.backend`
- `real.asio.raw.async1.backend`

## Notes

This is a "semantics-near" microbenchmark, not a full feature-equivalence evaluation.
For publication-grade comparisons, further align cancel semantics, error propagation semantics, scheduling model, and allocation accounting methodology.
