# Models — G0

No weights are committed. This directory only holds the manifest of what G0 needs
and how each file is verified. **A model without a SHA-256 and a licence field
does not enter the pipeline** (docs/decisions-ar.md D10).

## What G0 needs

| file | size (approx) | licence | status |
|---|---|---|---|
| `lama_fp32.onnx` | ~200 MB | code Apache-2.0, **weights unconfirmed** | ⚠️ see below |
| `lama_fp16.onnx` | ~100 MB | same | optional, CPU/GPU |

> **Licence risk R1 (blocking for a commercial release).**
> The LaMa *code* is Apache-2.0 (Samsung Research), but the `big-lama` *weights*
> have no explicit licence statement upstream, and the training data (Places2)
> carries research-only terms. Three mitigations are tracked in
> `docs/decisions-ar.md` §5. Until one lands, LaMa is a **development-only**
> default; the shipping default must be a model with a confirmed permissive
> licence (MI-GAN, or SD-1.5 inpainting under OpenRAIL-M).

## Manifest format (the only thing the app trusts)

`models/manifest.json` — signed with Ed25519 in production; in G0 the signature is
checked but the key is a development key.

```json
{
  "version": 1,
  "signed_by": "dev-key-0001",
  "models": [
    {
      "id": "lama.fp32.512",
      "file": "lama_fp32.onnx",
      "sha256": "<fill after export>",
      "bytes": 0,
      "license": "Apache-2.0 (code) / weights: unconfirmed",
      "commercial_use": false,
      "input": {"width": 512, "height": 512, "layout": "NCHW", "range": "0..1"},
      "params": {"type": "none", "note": "deterministic: no seed, no CFG, no steps"},
      "requires": {"abi": 1}
    }
  ]
}
```

`params.type` is the schema hook from D4/§3.3: diffusion models declare
`{"type": "lcm", "steps": [4], "cfg": [1.5], "seed": true}` and the UI renders
only what the loaded model declares. There is no free-form CFG field anywhere.

## Producing `lama_fp32.onnx`

```bash
pip install torch onnx onnxruntime onnxconverter-common
pip install git+https://github.com/advimman/lama.git
python tools/export_lama_onnx.py --ckpt big-lama.pt --out models/lama_fp32.onnx
sha256sum models/lama_fp32.onnx   # paste into manifest.json
```

Then, on the host (no phone needed):

```bash
cmake -S native -B native/build-ort -DSV_WITH_ORT=ON -DSV_ORT_ROOT=/path/to/onnxruntime
cmake --build native/build-ort -j
native/build-ort/sv_g0 --backend lama --model models/lama_fp32.onnx \
                       --image native/build/test.ppm --mask native/build/test.pgm --repeat 3
```

That command is the *real* G0 measurement. The synthetic backend only proves the
plumbing; this proves the product.
