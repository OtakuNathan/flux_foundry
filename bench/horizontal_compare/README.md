# horizontal_compare

`flux_foundry` 与 standalone Asio 的同机同编译器微基准入口。
输出分为两层：

- `pure scheduling overhead`: 尽量只看调度路径开销（`via` vs `dispatch/post`）
- `full semantics overhead`: 保留 `result/await/when_all/when_any` 的完整语义路径

## 运行

```bash
bench/horizontal_compare/run.sh
```

脚本会在首次运行时自动拉取 Asio 到 `/tmp/flux_foundry_asio`，不会把第三方仓库塞进当前项目目录。

## 当前覆盖

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
- `full.asio.post.when_all2`
- `full.flux.runner.when_any2.post`
- `full.flux.fast_runner.when_any2.post`
- `full.asio.post.when_any2`

## 注意

这是“同语义近似”的微基准，不是完整功能对等评测。
如需发布级横评，建议进一步统一取消语义、错误传播语义、调度模型和 alloc 统计口径。
