# ShadowVision2

On-device image inpainting for Android — no inference servers, no upload, no
account. Segmentation (MobileSAM) → removal (LaMa) → optional generative fill
(SD-1.5 + LCM) entirely on the phone.

**Current milestone: G0 — feasibility gate.** No product UI is written until the
numbers on a real device justify it.

| document | what it is |
|---|---|
| [`docs/design-review-ar.md`](docs/design-review-ar.md) | the design review that killed the original plan's two fatal assumptions |
| [`docs/decisions-ar.md`](docs/decisions-ar.md) | the 12 binding decisions, with the target metrics they serve |
| [`docs/G0-runbook-ar.md`](docs/G0-runbook-ar.md) | how to build, run and close the G0 gate |

## Verify in 30 seconds (no NDK, no JDK, no weights)

```bash
./tools/verify_g0.sh          # build + 33 unit checks + gate report
./tools/verify_g0.sh --asan   # under AddressSanitizer/UBSan
```

The harness reports PASS/FAIL per criterion instead of a number you have to
interpret:

```
time (best)    : 38.9 ms        [PASS] time_total_ms      38.90  (limit 4000)
tracked peak   : 6.9 MB         [PASS] tracked_peak_mb     6.87  (limit 1200)
peak RSS       : 29.2 MB        [PASS] process_rss_mb     29.21  (limit 700)
=== G0: PASS ===
```

> The numbers above come from the **synthetic** backend: they measure the
> geometry (crop → resize → dilate/feather → blend) and the memory governor, not
> the network. Never quote them as device performance.

## Layout

```
native/     C++17 core: MemoryGovernor, kernels, LaMa pipeline, ONNX backend,
            stable C ABI, CLI harness, unit tests. Builds on host and Android.
android/    Gradle/NDK shell: single inference thread, RegisterNatives JNI,
            G0 runner UI, 16 KB-aligned arm64-only packaging.
tools/      verify_g0.sh, LaMa→ONNX exporter, CI type-check shims.
models/     manifest + licence rules. No weights are committed, ever.
```

## Non-negotiables

1. **LaMa is the default path**, diffusion is an optional layer. Deterministic
   beats technically-impressive-but-slow.
2. **CPU is the floor; NPU is a per-SoC artifact**, never a checkbox. No fake
   "GPU delegate" in the UI, no invented VRAM counter.
3. **Failures are refused, not absorbed**: `MemoryGovernor` returns
   `BudgetExceeded` before the kernel has to kill the process.
4. **Every claim is measurable** or it does not ship.

## Status

| gate | state |
|---|---|
| G0 host harness (build, tests, gate report, ASan) | ✅ green |
| G0 device measurement with real LaMa weights | ⬜ not yet run |
| G1 product MVP ("remove") | ⬜ blocked on G0 |
