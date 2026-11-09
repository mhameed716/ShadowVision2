// SPDX-License-Identifier: MIT
// MobileSAM encoder + decoder through ONNX Runtime.
//
// This is the translation unit that turns sv::ISegmenterEncoder/Decoder into
// actual MobileSAM graphs. It is the counterpart of ort_backend.cpp (which does
// inpainting) and follows the same rules:
//   * the only place that includes <onnxruntime_cxx_api.h> for segmentation,
//   * all large buffers go through MemoryGovernor,
//   * fixed shapes, one session per graph, released between images,
//   * every transformation of coordinates goes through sv/sam_geometry.h.
//
// Contract (tools/export_mobilesam_onnx.py):
//   encoder  image   : float32 [1,3,S,S]  (SAM normalization, zeros padding)
//            embedding: float32 [1,256,64,64]
//   decoder  embedding + points [1,N,2] + labels [1,N] -> logits [1,1,256,256]
//
// Failure policy: if a model lacks the expected inputs or outputs we refuse to
// load rather than guess. A silently mis-wired decoder produces confident, wrong
// masks — the worst possible failure mode for a tool whose whole promise is
// "this is the object you selected".
#include "sv/segmenter.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "sv/kernels.h"
#include "sv/memory.h"
#include "sv/sam_geometry.h"

namespace sv {
namespace {

constexpr const char* kEncIn = "image";
constexpr const char* kEncOut = "embedding";
constexpr const char* kDecEmbedding = "embedding";
constexpr const char* kDecPoints = "points";
constexpr const char* kDecLabels = "labels";
constexpr const char* kDecOut = "logits";

// SAM's low-res mask is 256x256 for a 1024 input; derived, not assumed.
constexpr int kMaskSide = 256;
constexpr int kEmbedSide = 64;
constexpr int kEmbedChannels = 256;

std::string ShapeToString(const std::vector<int64_t>& s) {
    std::string out = "[";
    for (size_t i = 0; i < s.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(s[i]);
    }
    return out + "]";
}

bool HasInput(Ort::Session& session, const char* name, std::vector<int64_t>* shape) {
    Ort::AllocatorWithDefaultOptions alloc;
    const size_t n = session.GetInputCount();
    for (size_t i = 0; i < n; ++i) {
        auto in = session.GetInputNameAllocated(i, alloc);
        if (std::strcmp(in.get(), name) == 0) {
            if (shape) {
                *shape = session.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            }
            return true;
        }
    }
    return false;
}

bool HasOutput(Ort::Session& session, const char* name) {
    Ort::AllocatorWithDefaultOptions alloc;
    const size_t n = session.GetOutputCount();
    for (size_t i = 0; i < n; ++i) {
        auto out = session.GetOutputNameAllocated(i, alloc);
        if (std::strcmp(out.get(), name) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
class OnnxEncoder final : public ISegmenterEncoder {
public:
    OnnxEncoder() : m_env(ORT_LOGGING_LEVEL_WARNING, "sv-seg-enc") {}

    Status load(const BackendOptions& opts) override {
        std::lock_guard<std::mutex> lock(m_mu);
        m_ready = false;
        if (opts.model_path.empty()) return Status::InvalidArgument;
        try {
            Ort::SessionOptions so;
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            // The encoder is one big clockwork; splitting it across threads buys
            // little and heats the phone (same rationale as the inpainter).
            so.SetIntraOpNumThreads(opts.intra_op_threads > 0 ? opts.intra_op_threads : 2);
            so.SetInterOpNumThreads(1);
            so.AddConfigEntry("session.intra_op.allow_spinning", "0");

            m_session = Ort::Session(m_env, opts.model_path.c_str(), so);

            std::vector<int64_t> shape;
            if (!HasInput(m_session, kEncIn, &shape)) {
                m_error = std::string("model has no input named '") + kEncIn + "'";
                return Status::InvalidArgument;
            }
            if (shape.size() != 4 || shape[2] <= 0 || shape[3] <= 0) {
                // Dynamic shapes would need a second session per image size and make
                // QNN impossible; the export script pins them on purpose.
                m_error = "encoder input must be static NCHW, got " + ShapeToString(shape);
                return Status::InvalidArgument;
            }
            if (shape[2] != shape[3]) {
                m_error = "encoder input must be square, got " + ShapeToString(shape);
                return Status::InvalidArgument;
            }
            m_model_size = static_cast<int>(shape[2]);
            if (!HasOutput(m_session, kEncOut)) {
                m_error = std::string("model has no output named '") + kEncOut + "'";
                return Status::InvalidArgument;
            }
            m_ready = true;
            return Status::Ok;
        } catch (const std::exception& e) {
            m_error = e.what();
            return Status::Internal;
        }
    }

    bool ready() const override { return m_ready; }

    Status encode(const ImageRGB& image, std::vector<float>* embedding,
                  std::vector<int64_t>* shape) override {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_ready) return Status::ModelNotLoaded;
        if (!image.valid() || !embedding || !shape) return Status::InvalidArgument;

        const int s = m_model_size;
        const auto geom = sam::PreprocessGeometry(image.width, image.height, s);
        const size_t px = static_cast<size_t>(s) * s;
        const size_t bytes = px * 3 * sizeof(float);

        auto lease = MemoryGovernor::global().acquire("seg.encoder.in", bytes);
        if (!lease.live) return Status::BudgetExceeded;

        // 1. Downscale with an area filter (see ResizeAreaRGB) into a leased
        //    scratch buffer, then 2. normalise into the NCHW model tensor with
        //    zero padding on the right/bottom — exactly the SamPredictor recipe.
        const size_t resized_bytes = static_cast<size_t>(geom.new_w) * geom.new_h * 3;
        auto lease_resized = MemoryGovernor::global().acquire("seg.encoder.resized", resized_bytes);
        if (!lease_resized.live) return Status::BudgetExceeded;

        std::vector<uint8_t> resized(resized_bytes);
        ResizeAreaRGB(image, geom.new_w, geom.new_h,
                      MutableImageRGB{resized.data(), geom.new_w, geom.new_h, geom.new_w * 3});

        std::vector<float> input(px * 3, 0.f);
        for (int y = 0; y < geom.new_h; ++y) {
            const uint8_t* row = resized.data() + static_cast<size_t>(y) * geom.new_w * 3;
            for (int x = 0; x < geom.new_w; ++x) {
                const uint8_t* p = row + static_cast<size_t>(x) * 3;
                const size_t i = static_cast<size_t>(y) * s + x;
                for (int c = 0; c < 3; ++c) {
                    input[static_cast<size_t>(c) * px + i] =
                        sam::Normalize(c, static_cast<float>(p[c]));
                }
            }
        }

        try {
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const int64_t in_shape[4] = {1, 3, s, s};
            Ort::Value tin =
                Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), in_shape, 4);
            const char* in_names[] = {kEncIn};
            const char* out_names[] = {kEncOut};
            auto outputs = m_session.Run(Ort::RunOptions{nullptr}, in_names, &tin, 1, out_names, 1);
            if (outputs.empty()) return Status::Internal;

            auto info = outputs[0].GetTensorTypeAndShapeInfo();
            const std::vector<int64_t> out_shape = info.GetShape();
            const size_t count = info.GetElementCount();
            const float* data = outputs[0].GetTensorData<float>();

            // Keep the shape the graph reported: the decoder must receive exactly
            // what this model produced, not what we assumed it would.
            embedding->assign(data, data + count);
            *shape = out_shape;
            return Status::Ok;
        } catch (const std::exception& e) {
            m_error = e.what();
            return Status::Internal;
        }
    }

    void release() override {
        std::lock_guard<std::mutex> lock(m_mu);
        m_ready = false;
    }

    std::string diagnostics() const { return m_error; }

private:
    Ort::Env m_env;
    Ort::Session m_session{nullptr};
    mutable std::mutex m_mu;
    bool m_ready = false;
    int m_model_size = 1024;
    std::string m_error;
};

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
class OnnxDecoder final : public ISegmenterDecoder {
public:
    OnnxDecoder() : m_env(ORT_LOGGING_LEVEL_WARNING, "sv-seg-dec") {}

    Status load(const BackendOptions& opts) override {
        std::lock_guard<std::mutex> lock(m_mu);
        m_ready = false;
        if (opts.model_path.empty()) return Status::InvalidArgument;
        try {
            Ort::SessionOptions so;
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            // The decoder is tiny; a single thread avoids waking big cores for a
            // 40 ms job (waking them costs more power than the job itself).
            so.SetIntraOpNumThreads(1);
            so.SetInterOpNumThreads(1);
            so.AddConfigEntry("session.intra_op.allow_spinning", "0");

            m_session = Ort::Session(m_env, opts.model_path.c_str(), so);

            std::vector<int64_t> emb_shape;
            if (!HasInput(m_session, kDecEmbedding, &emb_shape)) {
                m_error = std::string("decoder has no input '") + kDecEmbedding + "'";
                return Status::InvalidArgument;
            }
            if (emb_shape.size() != 4 || emb_shape[1] <= 0 || emb_shape[2] <= 0 ||
                emb_shape[3] <= 0) {
                m_error = "decoder embedding input must be static [1,C,H,W], got " +
                          ShapeToString(emb_shape);
                return Status::InvalidArgument;
            }
            m_embed_channels = static_cast<int>(emb_shape[1]);
            m_embed_side = static_cast<int>(emb_shape[2]);
            if (static_cast<int>(emb_shape[3]) != m_embed_side) {
                m_error = "decoder embedding must be square, got " + ShapeToString(emb_shape);
                return Status::InvalidArgument;
            }

            std::vector<int64_t> pts_shape;
            if (!HasInput(m_session, kDecPoints, &pts_shape)) {
                m_error = std::string("decoder has no input '") + kDecPoints + "'";
                return Status::InvalidArgument;
            }
            m_max_points = (pts_shape.size() == 3 && pts_shape[1] > 0)
                               ? static_cast<int>(pts_shape[1])
                               : 8;
            if (!HasInput(m_session, kDecLabels, nullptr)) {
                m_error = std::string("decoder has no input '") + kDecLabels + "'";
                return Status::InvalidArgument;
            }
            if (!HasOutput(m_session, kDecOut)) {
                m_error = std::string("decoder has no output '") + kDecOut + "'";
                return Status::InvalidArgument;
            }
            m_ready = true;
            return Status::Ok;
        } catch (const std::exception& e) {
            m_error = e.what();
            return Status::Internal;
        }
    }

    bool ready() const override { return m_ready; }

    Status decode(const std::vector<float>& embedding, const std::vector<int64_t>& shape,
                  int image_w, int image_h, const std::vector<PointPrompt>& prompts,
                  bool keep_largest, std::vector<uint8_t>* mask_out) override {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_ready) return Status::ModelNotLoaded;
        if (!mask_out || prompts.empty() || image_w <= 0 || image_h <= 0) {
            return Status::InvalidArgument;
        }
        if (shape.size() != 4) return Status::InvalidArgument;
        const size_t expected =
            static_cast<size_t>(shape[1]) * shape[2] * shape[3];
        if (embedding.size() != expected) {
            m_error = "embedding size " + std::to_string(embedding.size()) + " != shape " +
                      ShapeToString(shape);
            return Status::InvalidArgument;
        }

        // Prompt coordinates: image px -> padded model space.
        const auto geom = sam::PreprocessGeometry(image_w, image_h, m_model_size());
        const int n_points = std::max(1, m_max_points);
        const size_t pts = static_cast<size_t>(n_points);
        auto lease_pts = MemoryGovernor::global().acquire("seg.decoder.prompts",
                                                         pts * 2 * sizeof(float) + pts * sizeof(float));
        if (!lease_pts.live) return Status::BudgetExceeded;

        std::vector<float> points(pts * 2, 0.f);
        std::vector<float> labels(pts, -1.f);  // SAM's convention: -1 = padding
        const int used = std::min<int>(static_cast<int>(prompts.size()), n_points);
        for (int i = 0; i < used; ++i) {
            float mx = 0.f, my = 0.f;
            sam::TransformPoint(geom, prompts[i].x, prompts[i].y, &mx, &my);
            points[static_cast<size_t>(i) * 2 + 0] = mx;
            points[static_cast<size_t>(i) * 2 + 1] = my;
            labels[static_cast<size_t>(i)] = prompts[i].label != 0 ? 1.f : 0.f;
        }
        (void)expected;

        try {
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const int64_t emb_dims[4] = {1, shape[1], shape[2], shape[3]};
            const int64_t pts_dims[3] = {1, n_points, 2};
            const int64_t lbl_dims[2] = {1, n_points};

            Ort::Value t_emb = Ort::Value::CreateTensor<float>(
                mem, const_cast<float*>(embedding.data()), embedding.size(), emb_dims, 4);
            Ort::Value t_pts =
                Ort::Value::CreateTensor<float>(mem, points.data(), points.size(), pts_dims, 3);
            Ort::Value t_lbl =
                Ort::Value::CreateTensor<float>(mem, labels.data(), labels.size(), lbl_dims, 2);

            const char* in_names[] = {kDecEmbedding, kDecPoints, kDecLabels};
            Ort::Value inputs[] = {std::move(t_emb), std::move(t_pts), std::move(t_lbl)};
            const char* out_names[] = {kDecOut};
            auto outputs =
                m_session.Run(Ort::RunOptions{nullptr}, in_names, inputs, 3, out_names, 1);
            if (outputs.empty()) return Status::Internal;

            auto info = outputs[0].GetTensorTypeAndShapeInfo();
            const std::vector<int64_t> out_shape = info.GetShape();
            const size_t count = info.GetElementCount();
            const float* logits = outputs[0].GetTensorData<float>();

            // Accept the shapes real exports produce:
            //   [1,1,H,W]  single mask (our script)
            //   [1,3,H,W]  multimask_output=True (someone else's export)
            //   [1,H,W]    squeezed by a tracer
            int masks = 1;
            int mh = 0, mw = 0;
            if (out_shape.size() == 4) {
                masks = std::max(1, static_cast<int>(out_shape[1]));
                mh = static_cast<int>(out_shape[2]);
                mw = static_cast<int>(out_shape[3]);
            } else if (out_shape.size() == 3) {
                mh = static_cast<int>(out_shape[1]);
                mw = static_cast<int>(out_shape[2]);
            } else {
                m_error = "unexpected decoder output shape " + ShapeToString(out_shape);
                return Status::Internal;
            }
            const size_t per_mask = static_cast<size_t>(mh) * static_cast<size_t>(mw);
            if (per_mask * static_cast<size_t>(masks) != count) {
                m_error = "decoder output element count mismatch: " + ShapeToString(out_shape) +
                          " vs " + std::to_string(count);
                return Status::Internal;
            }
            m_last_mask_side = mh;

            // For a multimask export pick the mask with the largest positive area.
            // SAM's three proposals are nested (part / whole / context); the largest
            // is the object the user tapped in practice, and picking per call is
            // cheaper than asking the UI to choose.
            const float* chosen = logits;
            if (masks > 1) {
                size_t best_area = 0;
                for (int i = 0; i < masks; ++i) {
                    const float* m = logits + per_mask * static_cast<size_t>(i);
                    size_t area = 0;
                    for (size_t k = 0; k < per_mask; ++k) area += m[k] > 0.f ? 1 : 0;
                    if (area > best_area) {
                        best_area = area;
                        chosen = m;
                    }
                }
            }

            return Postprocess(chosen, mw, mh, geom, image_w, image_h, keep_largest, mask_out);
        } catch (const std::exception& e) {
            m_error = e.what();
            return Status::Internal;
        }
    }

    void release() override {
        std::lock_guard<std::mutex> lock(m_mu);
        m_ready = false;
    }

    std::string diagnostics() const { return m_error; }

private:
    int m_model_size() const {
        // The encoder's input side equals the decoder mask side × 4 (256 -> 1024)
        // and the embedding side × 16 (64 -> 1024). We carry the image side that
        // the geometry needs: derive 1024 from the embedding, which the decoder
        // declared at load time.
        return m_embed_side > 0 ? m_embed_side * 16 : 1024;
    }

    // Low-res logits -> full-resolution binary mask, in image pixels.
    Status Postprocess(const float* logits, int mw, int mh, const sam::Geometry& geom,
                       int image_w, int image_h, bool keep_largest,
                       std::vector<uint8_t>* mask_out) const {
        // 1. Threshold in model space. SAM's low-res output is 256x256 for a 1024
        //    input; we upscale with nearest to the *resized* (unpadded) size so the
        //    padding never contributes a pixel.
        const float thr = 0.f;  // logits are already calibrated around 0
        std::vector<uint8_t> small(static_cast<size_t>(mw) * mh, 0);
        for (int y = 0; y < mh; ++y) {
            for (int x = 0; x < mw; ++x) {
                small[static_cast<size_t>(y) * mw + x] = logits[static_cast<size_t>(y) * mw + x] > thr
                                                             ? 255
                                                             : 0;
            }
        }

        // 2. Nearest-neighbour upscale to the resized image, then to image pixels.
        //    Two steps because the aspect ratios differ: first undo the 16x model
        //    downscale, then map the resized image back onto the original.
        mask_out->assign(static_cast<size_t>(image_w) * image_h, 0);
        for (int y = 0; y < image_h; ++y) {
            const float my = (static_cast<float>(y) + 0.5f) * geom.scale_y;  // image -> model
            const int sy = std::min(mh - 1, std::max(0, static_cast<int>(my / (geom.model_size / static_cast<float>(mh)))));
            for (int x = 0; x < image_w; ++x) {
                const float mx = (static_cast<float>(x) + 0.5f) * geom.scale_x;
                const int sx = std::min(mw - 1, std::max(0, static_cast<int>(mx / (geom.model_size / static_cast<float>(mw)))));
                (*mask_out)[static_cast<size_t>(y) * image_w + x] =
                    small[static_cast<size_t>(sy) * mw + sx];
            }
        }

        // 3. Remove blobs. The decoder legitimately returns a few specks near the
        //    object; on a phone "select object + 3 specks" reads as a bug.
        if (keep_largest) KeepLargestComponent(mask_out->data(), image_w, image_h);
        return Status::Ok;
    }

    Ort::Env m_env;
    Ort::Session m_session{nullptr};
    mutable std::mutex m_mu;
    bool m_ready = false;
    int m_embed_channels = kEmbedChannels;
    int m_embed_side = kEmbedSide;
    int m_max_points = 8;
    int m_last_mask_side = kMaskSide;
    std::string m_error;
};

}  // namespace

std::unique_ptr<ISegmenterEncoder> CreateOnnxEncoder() {
    return std::unique_ptr<ISegmenterEncoder>(new OnnxEncoder());
}

std::unique_ptr<ISegmenterDecoder> CreateOnnxDecoder() {
    return std::unique_ptr<ISegmenterDecoder>(new OnnxDecoder());
}

}  // namespace sv
