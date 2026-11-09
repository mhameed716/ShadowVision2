// SPDX-License-Identifier: MIT
// Segmentation: MobileSAM split into encoder + decoder (docs/decisions-ar.md D5).
//
// The split is the single highest-leverage decision in the product:
//   * encoder runs once per image (~0.4-1.2 s, background, shows a spinner),
//   * decoder runs per tap (~30-80 ms), so "tap the object" feels instant.
// Running the monolithic model per tap instead would mean 1-2 s per tap and a
// product nobody uses twice.
//
// Model contract (see tools/export_mobilesam_onnx.py):
//   encoder: image float32 NCHW [1,3,1024,1024] -> embedding [1,256,64,64]
//   decoder: embedding + points [1,N,2] + labels [1,N] -> logits [1,1,256,256]
//
// Coordinates are supplied in *image* pixels and converted here to the model's
// padded 1024 space (sv/sam_geometry.h), so the UI never has to know about it.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "sv/backend.h"
#include "sv/common.h"

namespace sv {

struct PointPrompt {
    float x = 0.f;   // image pixels
    float y = 0.f;
    int label = 1;   // 1 = foreground, 0 = background
};

class ISegmenterEncoder {
public:
    virtual ~ISegmenterEncoder() = default;
    virtual Status load(const BackendOptions& opts) = 0;
    virtual bool ready() const = 0;
    // Produces a compact embedding for one image. Deterministic.
    virtual Status encode(const ImageRGB& image, std::vector<float>* embedding,
                          std::vector<int64_t>* shape) = 0;
    virtual void release() = 0;
};

class ISegmenterDecoder {
public:
    virtual ~ISegmenterDecoder() = default;
    virtual Status load(const BackendOptions& opts) = 0;
    virtual bool ready() const = 0;
    // Writes a full-resolution 8-bit mask (0/255) for the given prompts.
    // `keep_largest` removes the small disconnected blobs the decoder sometimes
    // returns around the object; on a phone this is the difference between
    // "select object" and "select object plus three random specks".
    virtual Status decode(const std::vector<float>& embedding, const std::vector<int64_t>& shape,
                          int image_w, int image_h, const std::vector<PointPrompt>& prompts,
                          bool keep_largest, std::vector<uint8_t>* mask_out) = 0;
    virtual void release() = 0;
};

// Session that owns the embedding for one image and answers taps.
class SegmentationSession {
public:
    SegmentationSession(std::unique_ptr<ISegmenterEncoder> enc,
                        std::unique_ptr<ISegmenterDecoder> dec);
    ~SegmentationSession();

    // Loads encoder + decoder (idempotent). Must precede prepare().
    Status load(const BackendOptions& enc_opts, const BackendOptions& dec_opts);

    Status prepare(const ImageRGB& image);   // encoder (slow, once)
    bool prepared() const { return m_prepared; }

    // Prompts accumulate: tap 1 = add foreground, tap with label 0 = exclude.
    Status select(const std::vector<PointPrompt>& prompts, std::vector<uint8_t>* mask,
                  int* mask_w, int* mask_h);

    void reset();      // clears prompts, keeps the embedding
    void release();    // frees everything (called when the image changes)

    float encoder_ms() const { return m_encoder_ms; }
    float decoder_ms() const { return m_decoder_ms; }

private:
    std::unique_ptr<ISegmenterEncoder> m_enc;
    std::unique_ptr<ISegmenterDecoder> m_dec;
    std::vector<float> m_embedding;
    std::vector<int64_t> m_shape;
    int m_image_w = 0;
    int m_image_h = 0;
    bool m_prepared = false;
    float m_encoder_ms = 0.f;
    float m_decoder_ms = 0.f;
};

// Real MobileSAM graphs through ONNX Runtime. Built only when SV_WITH_ORT=1:
//   CreateOnnxEncoder() / CreateOnnxDecoder()
// `BackendOptions.model_path` selects the graph; shapes are read from the model
// so the 1024 default is not hard-coded a second time.
#if SV_WITH_ORT
std::unique_ptr<ISegmenterEncoder> CreateOnnxEncoder();
std::unique_ptr<ISegmenterDecoder> CreateOnnxDecoder();
#endif

// A deterministic stand-in that makes the whole UI testable with no weights:
// selects the connected region around the last prompt by colour similarity.
// It is intentionally mediocre; it exists so that the canvas, the mask plumbing
// and the pipeline can be exercised end to end on CI and on a fresh install.
std::unique_ptr<ISegmenterEncoder> CreateSyntheticEncoder();
std::unique_ptr<ISegmenterDecoder> CreateSyntheticDecoder();

// Keeps only the largest 4-connected component. Exposed for tests.
void KeepLargestComponent(uint8_t* mask, int w, int h);

}  // namespace sv
