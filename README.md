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
| [`docs/product-ar.md`](docs/product-ar.md) | the two-interface product build: pipeline, privacy enforcement, what is still stubbed |

## Verify in 30 seconds (no NDK, no JDK, no weights)

```bash
./tools/verify_g0.sh          # build + 76 unit checks + gate report
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
native/     C++17 core: MemoryGovernor, kernels, LaMa pipeline, segmentation
            session, SHA-256 + Ed25519 catalog verifier, ONNX backend, stable
            C ABI (v2), CLI harness, 52 unit checks. Builds on host and Android.
android/    Product app: Studio (dark canvas editor) + Engine Control console,
            single inference thread, RegisterNatives JNI, 16 KB arm64 packaging.
            Zero INTERNET permission — enforced by tools/check_manifest.sh in CI.
tools/      verify_g0.sh · sign_catalog.py (Ed25519, dependency-free) ·
            export_lama_onnx.py · export_mobilesam_onnx.py · privacy guard ·
            CI type-check shims.
models/     signed catalog + licence rules. No weights are committed, ever.
```

## The two interfaces

**Studio** — import → brush or smart tap → remove → before/after → export.
The canvas is a custom View, not Compose Canvas: a 12 MP image redrawn by Compose
is GC stutter followed by OOM. The mask is stored at half resolution (~4× less
memory) and upscaled with smoothing at export.

**Engine Control** — process RSS (the number that actually gets you killed; VRAM
does not exist on Android), thermal status, signed-catalog verification per model,
and a live log ring. Accelerator choices are **CPU** and **QNN Auto** only: GPU is
absent because ONNX Runtime has no Vulkan EP on Android and NNAPI is deprecated.

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
| G0 host harness (build, 76 tests, gate report, ASan/UBSan) | ✅ green |
| ONNX translation units compile against ORT 1.23 headers | ✅ in CI |
| MobileSAM weights exported + pushed to a device | ⬜ needs desktop export |
| Signed catalog Ed25519 (Python sign ↔ C++ verify, tamper-tested) | ✅ green |
| Privacy guardrails (no INTERNET/GPS, no network APIs) | ✅ enforced in CI |
| Product UI: Studio + Engine Control | ✅ built (needs device run) |
| Real LaMa weights on a device | ⬜ export + push + measure |
| ONNX MobileSAM encoder+decoder (`ort_segmenter.cpp`) | ✅ implemented, type-checked against ORT 1.23 headers |
| Real MobileSAM weights on a device | ⬜ export + push + parity run |
