// SPDX-License-Identifier: MIT
#include "sv/segmenter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "sv/memory.h"

namespace sv {

namespace {
double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}
}  // namespace

// ---------------------------------------------------------------------------
// Connected components / cleanup
// ---------------------------------------------------------------------------
void KeepLargestComponent(uint8_t* mask, int w, int h) {
    if (!mask || w <= 0 || h <= 0) return;
    std::vector<int32_t> label(static_cast<size_t>(w) * h, 0);
    std::vector<int32_t> stack;
    int32_t next = 0;
    int32_t best = 0;
    size_t best_size = 0;
    std::vector<size_t> sizes;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = static_cast<size_t>(y) * w + x;
            if (mask[idx] == 0 || label[idx] != 0) continue;
            ++next;
            size_t count = 0;
            stack.clear();
            stack.push_back(static_cast<int32_t>(idx));
            label[idx] = next;
            while (!stack.empty()) {
                const int32_t cur = stack.back();
                stack.pop_back();
                ++count;
                const int cx = cur % w;
                const int cy = cur / w;
                const int nx[4] = {cx - 1, cx + 1, cx, cx};
                const int ny[4] = {cy, cy, cy - 1, cy + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                    const size_t nidx = static_cast<size_t>(ny[k]) * w + nx[k];
                    if (mask[nidx] == 0 || label[nidx] != 0) continue;
                    label[nidx] = next;
                    stack.push_back(static_cast<int32_t>(nidx));
                }
            }
            sizes.resize(static_cast<size_t>(next), 0);
            sizes[static_cast<size_t>(next - 1)] = count;
            if (count > best_size) {
                best_size = count;
                best = next;
            }
        }
    }
    if (best == 0) return;
    for (size_t i = 0; i < label.size(); ++i) {
        if (label[i] != best) mask[i] = 0;
    }
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
SegmentationSession::SegmentationSession(std::unique_ptr<ISegmenterEncoder> enc,
                                         std::unique_ptr<ISegmenterDecoder> dec)
    : m_enc(std::move(enc)), m_dec(std::move(dec)) {}

SegmentationSession::~SegmentationSession() { release(); }

Status SegmentationSession::load(const BackendOptions& enc_opts, const BackendOptions& dec_opts) {
    if (!m_enc || !m_dec) return Status::Internal;
    Status st = m_enc->load(enc_opts);
    if (st != Status::Ok) return st;
    return m_dec->load(dec_opts);
}

Status SegmentationSession::prepare(const ImageRGB& image) {
    if (!m_enc || !m_dec) return Status::ModelNotLoaded;
    // Auto-load with defaults so a caller that only wants the built-in
    // (no-model) segmenter does not have to know about BackendOptions.
    if (!m_enc->ready() && m_enc->load(BackendOptions{}) != Status::Ok) return Status::ModelNotLoaded;
    if (!m_dec->ready() && m_dec->load(BackendOptions{}) != Status::Ok) return Status::ModelNotLoaded;
    const double t0 = now_ms();
    const Status st = m_enc->encode(image, &m_embedding, &m_shape);
    m_encoder_ms = static_cast<float>(now_ms() - t0);
    if (st != Status::Ok) return st;
    m_image_w = image.width;
    m_image_h = image.height;
    m_prepared = true;
    return Status::Ok;
}

Status SegmentationSession::select(const std::vector<PointPrompt>& prompts,
                                   std::vector<uint8_t>* mask, int* mask_w, int* mask_h) {
    if (!m_prepared || !m_dec || !m_dec->ready()) return Status::ModelNotLoaded;
    if (!mask || !mask_w || !mask_h) return Status::InvalidArgument;
    if (prompts.empty()) return Status::InvalidArgument;

    const double t0 = now_ms();
    const Status st = m_dec->decode(m_embedding, m_shape, m_image_w, m_image_h, prompts, true,
                                    mask);
    m_decoder_ms = static_cast<float>(now_ms() - t0);
    if (st == Status::Ok) {
        *mask_w = m_image_w;
        *mask_h = m_image_h;
    }
    return st;
}

void SegmentationSession::reset() { /* prompts are passed per call, nothing cached */ }

void SegmentationSession::release() {
    if (m_enc) m_enc->release();
    if (m_dec) m_dec->release();
    m_embedding.clear();
    m_embedding.shrink_to_fit();
    m_shape.clear();
    m_prepared = false;
}

// ---------------------------------------------------------------------------
// Synthetic segmenter: colour-region growth from the last foreground prompt.
//
// This is what makes the User Studio demonstrable on a fresh install with no
// models at all. It is bounded, deterministic and honest: it cannot mislead
// anyone into thinking they are looking at MobileSAM output (the UI labels it
// "built-in (no model)").
// ---------------------------------------------------------------------------
namespace {

struct SyntheticEnc : ISegmenterEncoder {
    // 3x3 mean colour grid: a "poor man's embedding". Enough to separate a
    // textured object from a uniform background, which is exactly the fixture.
    std::vector<float> grid;
    int gw = 0, gh = 0;
    int img_w = 0, img_h = 0;
    bool loaded = false;

    Status load(const BackendOptions&) override {
        loaded = true;
        return Status::Ok;
    }
    bool ready() const override { return loaded; }

    Status encode(const ImageRGB& image, std::vector<float>* embedding,
                  std::vector<int64_t>* shape) override {
        if (!image.valid()) return Status::InvalidArgument;
        gw = 32;
        gh = 32;
        img_w = image.width;
        img_h = image.height;
        grid.assign(static_cast<size_t>(gw) * gh * 3, 0.f);
        for (int gy = 0; gy < gh; ++gy) {
            for (int gx = 0; gx < gw; ++gx) {
                const int x0 = gx * image.width / gw;
                const int x1 = std::max(x0 + 1, (gx + 1) * image.width / gw);
                const int y0 = gy * image.height / gh;
                const int y1 = std::max(y0 + 1, (gy + 1) * image.height / gh);
                double r = 0, g = 0, b = 0;
                int n = 0;
                for (int y = y0; y < y1; ++y) {
                    const uint8_t* row = image.data + static_cast<size_t>(y) * image.stride;
                    for (int x = x0; x < x1; ++x) {
                        r += row[x * 3 + 0];
                        g += row[x * 3 + 1];
                        b += row[x * 3 + 2];
                        ++n;
                    }
                }
                const size_t i = (static_cast<size_t>(gy) * gw + gx) * 3;
                grid[i + 0] = static_cast<float>(r / n);
                grid[i + 1] = static_cast<float>(g / n);
                grid[i + 2] = static_cast<float>(b / n);
            }
        }
        // The real ONNX encoder returns [1,256,64,64]. The synthetic one returns
        // its own tensor; the decoder must not assume the other's shape, so the
        // grid dimensions travel in `shape` and the decoder reads them from there.
        *embedding = grid;
        *shape = {1, 3, gh, gw};
        return Status::Ok;
    }
    void release() override {
        grid.clear();
        grid.shrink_to_fit();
        loaded = false;
    }
};

struct SyntheticDec : ISegmenterDecoder {
    bool loaded = false;
    Status load(const BackendOptions&) override {
        loaded = true;
        return Status::Ok;
    }
    bool ready() const override { return loaded; }

    Status decode(const std::vector<float>& embedding, const std::vector<int64_t>& shape,
                  int image_w, int image_h, const std::vector<PointPrompt>& prompts,
                  bool keep_largest, std::vector<uint8_t>* mask_out) override {
        if (shape.size() != 4 || embedding.empty()) return Status::InvalidArgument;
        const int gw = static_cast<int>(shape[3]);
        const int gh = static_cast<int>(shape[2]);

        // Seed colour = mean of a small neighbourhood around the last foreground tap.
        float seed[3] = {0, 0, 0};
        int taps = 0;
        for (const auto& p : prompts) {
            if (p.label != 1) continue;
            const int gx = std::clamp(static_cast<int>(p.x * gw / image_w), 0, gw - 1);
            const int gy = std::clamp(static_cast<int>(p.y * gh / image_h), 0, gh - 1);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = std::clamp(gx + dx, 0, gw - 1);
                    const int y = std::clamp(gy + dy, 0, gh - 1);
                    const size_t i = (static_cast<size_t>(y) * gw + x) * 3;
                    seed[0] += embedding[i + 0];
                    seed[1] += embedding[i + 1];
                    seed[2] += embedding[i + 2];
                    ++taps;
                }
            }
        }
        if (taps == 0) return Status::InvalidArgument;
        for (float& c : seed) c /= static_cast<float>(taps);

        const float tol = 26.f;
        mask_out->assign(static_cast<size_t>(image_w) * image_h, 0);
        for (int y = 0; y < image_h; ++y) {
            const int gy = std::clamp(y * gh / image_h, 0, gh - 1);
            for (int x = 0; x < image_w; ++x) {
                const int gx = std::clamp(x * gw / image_w, 0, gw - 1);
                const size_t i = (static_cast<size_t>(gy) * gw + gx) * 3;
                // Any 2D grid in this session is regenerated per tap: the grid is
                // repeated in `embedding` only once, so scale it back here.
                const float r = embedding[i + 0];
                const float g = embedding[i + 1];
                const float b = embedding[i + 2];
                const float d = std::fabs(r - seed[0]) + std::fabs(g - seed[1]) + std::fabs(b - seed[2]);
                if (d < tol * 3.f) (*mask_out)[static_cast<size_t>(y) * image_w + x] = 255;
            }
        }
        // Exclude regions from negative prompts.
        for (const auto& p : prompts) {
            if (p.label != 0) continue;
            const int cx = static_cast<int>(p.x);
            const int cy = static_cast<int>(p.y);
            const int rad = std::max(8, image_w / 40);
            for (int y = std::max(0, cy - rad); y < std::min(image_h, cy + rad); ++y) {
                for (int x = std::max(0, cx - rad); x < std::min(image_w, cx + rad); ++x) {
                    (*mask_out)[static_cast<size_t>(y) * image_w + x] = 0;
                }
            }
        }
        if (keep_largest) KeepLargestComponent(mask_out->data(), image_w, image_h);
        return Status::Ok;
    }
    void release() override { loaded = false; }
};

}  // namespace

std::unique_ptr<ISegmenterEncoder> CreateSyntheticEncoder() {
    return std::unique_ptr<ISegmenterEncoder>(new SyntheticEnc());
}

std::unique_ptr<ISegmenterDecoder> CreateSyntheticDecoder() {
    return std::unique_ptr<ISegmenterDecoder>(new SyntheticDec());
}

}  // namespace sv
