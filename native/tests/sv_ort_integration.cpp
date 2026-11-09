// SPDX-License-Identifier: MIT
// Integration test: the *real* ONNX segmenter, running on synthetic graphs.
//
// This executable exists because `-fsyntax-only` proves nothing. Everything that
// can go wrong in this path goes wrong at a seam:
//   * a tensor bound to a buffer whose size disagrees with its declared shape,
//   * inputs passed in a different order than the graph expects,
//   * the mask indexed before the Status is checked,
//   * a coordinate transform that is off by the pad or by the resize scale.
// All of those compile. None of them are caught by unit tests on pure geometry.
//
// The graphs come from tools/make_test_graphs.py: shaped exactly like MobileSAM
// (encoder [1,3,S,S] -> [1,256,h,w], decoder -> [1,1,4h,4w]) but semantically an
// oracle — the decoder emits a disc of fixed radius around the prompt point in
// model space. That lets us assert the strong property:
//
//     a tap at image pixel (x, y) yields a mask centred at (x, y)
//
// which is the single most common real-world SAM integration bug (taps landing
// on the wrong object because of the 1024 padding).
//
// Build: -DSV_WITH_ORT=ON with -DSV_ORT_ROOT=<dir containing include/ and lib/>.
// Exit code 0 = all checks passed; failures are printed line by line.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sv/kernels.h"
#include "sv/memory.h"
#include "sv/sam_geometry.h"
#include "sv/segmenter.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// Deterministic non-uniform test image: a smooth gradient plus a checker texture
// so the area resizer has real work to do (a constant image would pass even if
// the resizer were a no-op).
std::vector<uint8_t> MakeImage(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = px.data() + (static_cast<size_t>(y) * w + x) * 3;
            const bool checker = ((x / 7) + (y / 7)) % 2 == 0;
            p[0] = static_cast<uint8_t>(x * 255 / (w > 1 ? w - 1 : 1));
            p[1] = static_cast<uint8_t>(y * 255 / (h > 1 ? h - 1 : 1));
            p[2] = checker ? 220 : 30;
        }
    }
    return px;
}

// Centroid of a binary mask, plus its area (in pixels).
struct Blob {
    double cx = 0, cy = 0;
    size_t area = 0;
};

Blob Measure(const std::vector<uint8_t>& mask, int w) {
    Blob b;
    for (size_t i = 0; i < mask.size(); ++i) {
        if (mask[i] == 0) continue;
        const int x = static_cast<int>(i % static_cast<size_t>(w));
        const int y = static_cast<int>(i / static_cast<size_t>(w));
        b.cx += x;
        b.cy += y;
        ++b.area;
    }
    if (b.area) {
        b.cx /= static_cast<double>(b.area);
        b.cy /= static_cast<double>(b.area);
    }
    return b;
}

// Largest 4-connected blob area, to prove KeepLargestComponent actually ran.
size_t LargestComponent(const std::vector<uint8_t>& mask, int w, int h) {
    std::vector<uint8_t> seen(mask.size(), 0);
    std::vector<int> stack;
    size_t best = 0;
    for (int start = 0; start < w * h; ++start) {
        if (!mask[static_cast<size_t>(start)] || seen[static_cast<size_t>(start)]) continue;
        size_t area = 0;
        stack.clear();
        stack.push_back(start);
        seen[static_cast<size_t>(start)] = 1;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            ++area;
            const int x = i % w, y = i / w;
            const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& n : nb) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h) continue;
                const int j = n[1] * w + n[0];
                if (mask[static_cast<size_t>(j)] && !seen[static_cast<size_t>(j)]) {
                    seen[static_cast<size_t>(j)] = 1;
                    stack.push_back(j);
                }
            }
        }
        if (area > best) best = area;
    }
    return best;
}

// Decode through the public session API and return the mask for one tap.
bool TapAt(sv::SegmentationSession& session, float x, float y, int w, int h,
           std::vector<uint8_t>* mask) {
    sv::PointPrompt p;
    p.x = x;
    p.y = y;
    p.label = 1;
    int mw = 0, mh = 0;
    session.reset();
    const sv::Status st = session.select({p}, mask, &mw, &mh);
    return st == sv::Status::Ok && mw == w && mh == h;
}

}  // namespace

// Absolute reference for the ENCODER. A tap-round-trip cannot see an encoder-side
// scaling error: the encoder misplaces the point, the decoder maps it back with
// its own (correct) geometry, and the mask still lands on the finger. The error
// cancels. Only an absolute observation catches it.
//
// The test encoder is a block-mean pooling encoder, so a bright patch at known
// image coordinates produces a hot block at a predictable embedding index. If the
// transform in front of it is wrong (padded vs stretched, wrong scale), the hot
// block moves — and that is measurable without the decoder at all.
static void CheckEncoderGeometry(const std::string& enc_path, const std::string& dec_path,
                                 bool expect_normalised, const char* label) {
    sv::BackendOptions eo, dopts;
    eo.model_path = enc_path;
    dopts.model_path = dec_path;
    auto enc = sv::CreateOnnxEncoder();
    auto dec = sv::CreateOnnxDecoder();
    if (enc->load(eo) != sv::Status::Ok || dec->load(dopts) != sv::Status::Ok) {
        Check(false, std::string(label) + ": encoder/decoder load for the geometry probe");
        return;
    }

    // Black 400x300 image with a white square covering whole embedding blocks.
    // 400 image px -> 256 model px (0.64), and one embedding block is 16 model px
    // = 25 image px. Use a 50x50 image-px square aligned to that grid so the
    // pooled value is not diluted by the black border.
    const int w = 400, h = 300;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3, 0);
    for (int y = 75; y < 125; ++y)
        for (int x = 300; x < 350; ++x)
            for (int c = 0; c < 3; ++c)
                px[(static_cast<size_t>(y) * w + x) * 3 + c] = 255;
    sv::ImageRGB img{px.data(), w, h, w * 3};

    std::vector<float> emb;
    std::vector<int64_t> shape;
    if (enc->encode(img, &emb, &shape) != sv::Status::Ok || shape.size() != 4) {
        Check(false, std::string(label) + ": encode for the geometry probe");
        return;
    }
    const int side = static_cast<int>(shape[2]);
    // channel 0 only: the test encoder carries R in channel 0.
    size_t best = 0;
    float best_v = -1e30f;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const float v = emb[static_cast<size_t>(y) * side + x];
            if (v > best_v) { best_v = v; best = static_cast<size_t>(y) * side + x; }
        }
    }
    const double fx = static_cast<double>(best % side) / (side - 1);
    const double fy = static_cast<double>(best / side) / (side - 1);
    // Expected normalised position of the patch centre (324, 84) in the frame:
    //   NCHW: content is 256x192 in a 256 frame -> x 324/400=0.81, y 84/300=0.28
    //   NHWC (square stretch): the frame spans the image, same fractions.
    // Either way the hot block must be in the lower-right quadrant, well away
    // from the centre: that is what a padded/stretched confusion breaks.
    Check(fx > 0.65 && fy > 0.15,
          std::string(label) + ": hot embedding block sits where the patch is "
          "(x=" + std::to_string(fx) + " y=" + std::to_string(fy) + ")");
    Check(fx > 0.65 && fy < 0.55,
          std::string(label) + ": hot block is low but not at the very bottom");

    // Absolute scale: NCHW graphs are fed pre-normalised values, so a fully lit
    // block is ~ (255-123.675)/58.395 = 2.25. NHWC graphs are fed raw 0..255.
    // A double-normalisation bug shows up here and nowhere else.
    // Absolute scale, with generous slack for the pooled edge blocks: NCHW graphs
    // are fed pre-normalised floats (a lit, unpooled pixel is ~2.25), NHWC graphs
    // are fed raw 0..255. Double normalisation would put the NCHW value near 0.85
    // and the NHWC value near 173 — both are outside this window.
    const double expected = expect_normalised ? (255.0 - 123.675) / 58.395 : 255.0;
    // Frame invariant: an NHWC encoder consumes a full s x s frame, so the
    // encoder must have resized to exactly s x s. A padded (content-height)
    // frame would leave the bottom rows zero and the square promise broken.
    {
        std::vector<uint8_t> probe_px(static_cast<size_t>(w) * h * 3, 200);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    probe_px[(static_cast<size_t>(y) * w + x) * 3 + c] =
                        static_cast<uint8_t>(y * 255 / (h - 1));
        sv::ImageRGB probe{probe_px.data(), w, h, w * 3};
        std::vector<float> emb2;
        std::vector<int64_t> shape2;
        Check(enc->encode(probe, &emb2, &shape2) == sv::Status::Ok,
              std::string(label) + ": gradient probe encodes");
        // Every embedding block must carry signal: a vertical gradient over a
        // fully covered frame leaves no all-black block. With a padded frame the
        // bottom third of the NHWC input would be zeros.
        int zero_blocks = 0;
        const int rows = static_cast<int>(shape2[2]);
        for (int y = 0; y < rows; ++y) {
            bool all_zero = true;
            for (int x = 0; x < rows; ++x) {
                if (emb2[static_cast<size_t>(y) * rows + x] > 0.f) { all_zero = false; break; }
            }
            if (all_zero) ++zero_blocks;
        }
        if (expect_normalised) {
            // The padded convention pads: rows below the content are zero by
            // design (192 of 256 here), and that is correct.
            Check(zero_blocks > 0,
                  std::string(label) + ": padded convention leaves the pad rows empty "
                  "(got " + std::to_string(zero_blocks) + ")");
        } else {
            // The square-frame convention must not pad: every row carries signal.
            Check(zero_blocks == 0,
                  std::string(label) + ": square frame is fully covered, no empty rows (got " +
                      std::to_string(zero_blocks) + ")");
        }
    }

    Check(best_v > expected * 0.8 && best_v <= expected * 1.02,
          std::string(label) + ": embedding scale matches the convention "
          "(max=" + std::to_string(best_v) + ", expected ~" + std::to_string(expected) + ")");
}

// Runs the same tap-accuracy assertions against a graph pair, so the two
// supported interfaces are held to an identical standard.
static void CheckTapAccuracy(const std::string& enc_path, const std::string& dec_path,
                             const char* label) {
    sv::BackendOptions eo, dopts;
    eo.model_path = enc_path;
    dopts.model_path = dec_path;
    sv::SegmentationSession session(sv::CreateOnnxEncoder(), sv::CreateOnnxDecoder());
    const sv::Status ls = session.load(eo, dopts);
    Check(ls == sv::Status::Ok, std::string(label) + ": loads");
    if (ls != sv::Status::Ok) return;

    const int w = 400, h = 300;
    std::vector<uint8_t> px = MakeImage(w, h);
    sv::ImageRGB img{px.data(), w, h, w * 3};
    Check(session.prepare(img) == sv::Status::Ok, std::string(label) + ": prepare");

    // The two preprocessing conventions must produce different geometry for a
    // non-square image, and the tap transform must follow the session's own
    // convention. Asserting the *difference* is what kills a mutant that swaps
    // the square frame for a padded one.
    {
        const sv::sam::Geometry padded = sv::sam::PreprocessGeometry(w, h, 256);
        float px_padded = 0.f, py_padded = 0.f, px_square = 0.f, py_square = 0.f;
        sv::sam::TransformPoint(padded, 100.f, 80.f, &px_padded, &py_padded);
        const sv::sam::Geometry square{256, 256, 256, 256.f / w, 256.f / h};
        sv::sam::TransformPoint(square, 100.f, 80.f, &px_square, &py_square);
        Check(std::fabs(py_padded - py_square) > 1.f,
              std::string(label) + ": padded and square conventions differ in y "
              "(" + std::to_string(py_padded) + " vs " + std::to_string(py_square) + ")");
        Check(padded.new_h != square.new_h,
              std::string(label) + ": padded and square conventions differ in height");
    }

    struct Probe { float x, y; } probes[] = {{100.f, 80.f}, {300.f, 220.f}};
    for (const auto& p : probes) {
        std::vector<uint8_t> mask;
        const bool ok = TapAt(session, p.x, p.y, w, h, &mask);
        Check(ok, std::string(label) + ": select returns an image-sized mask");
        if (!ok) continue;
        Check(mask.size() == static_cast<size_t>(w) * h,
              std::string(label) + ": mask length is w*h");
        const Blob b = Measure(mask, w);
        const double dx = b.cx - p.x, dy = b.cy - p.y;
        Check(std::fabs(dx) <= 14.0 && std::fabs(dy) <= 14.0,
              std::string(label) + ": centroid within 14 px of the tap (" +
                  std::to_string(dx) + "," + std::to_string(dy) + ")");
        // The oracle also emits a tiny second disc near the top-left corner. With
        // keep_largest (the session's behaviour) the result is one component.
        Check(LargestComponent(mask, w, h) == b.area,
              std::string(label) + ": keep_largest left a single component (" +
                  std::to_string(LargestComponent(mask, w, h)) + " of " +
                  std::to_string(b.area) + " px)");
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <encoder.onnx> <decoder.onnx>\n"
                     "       (generate them with tools/make_test_graphs.py)\n",
                     argv[0]);
        return 2;
    }
    const std::string enc_path = argv[1];
    const std::string dec_path = argv[2];

    sv::BackendOptions eo, dopts;
    eo.model_path = enc_path;
    dopts.model_path = dec_path;

    auto enc = sv::CreateOnnxEncoder();
    auto dec = sv::CreateOnnxDecoder();
    Check(enc != nullptr && dec != nullptr, "factory returns both halves");

    // --- 1. load rejects garbage before it runs anything ---------------------
    {
        sv::BackendOptions bad;
        bad.model_path = "/nonexistent/definitely_not_a_model.onnx";
        auto tmp = sv::CreateOnnxEncoder();
        const sv::Status st = tmp->load(bad);
        Check(st != sv::Status::Ok, "missing model file is an error, not a crash");
        Check(!tmp->ready(), "failed load leaves the encoder not ready");
    }
    {
        sv::BackendOptions empty;
        auto tmp = sv::CreateOnnxDecoder();
        Check(tmp->load(empty) == sv::Status::InvalidArgument,
              "empty model path is InvalidArgument");
    }

    // --- 2. load the contract-shaped graphs ---------------------------------
    const sv::Status es = enc->load(eo);
    const sv::Status ds = dec->load(dopts);
    Check(es == sv::Status::Ok, "encoder loads a contract-shaped graph");
    Check(ds == sv::Status::Ok, "decoder loads a contract-shaped graph");
    if (es != sv::Status::Ok || ds != sv::Status::Ok) {
        std::printf("cannot continue: load failed (enc=%s dec=%s)\n", sv::ToString(es),
                    sv::ToString(ds));
        return 1;
    }
    Check(enc->ready() && dec->ready(), "both halves report ready");

    // --- 3. encode: shapes and non-degeneracy -------------------------------
    // 4:3 landscape, the aspect ratio that produces right/bottom padding.
    const int iw = 400, ih = 300;
    std::vector<uint8_t> px = MakeImage(iw, ih);
    sv::ImageRGB img{px.data(), iw, ih, iw * 3};

    std::vector<float> emb;
    std::vector<int64_t> shape;
    const sv::Status est = enc->encode(img, &emb, &shape);
    Check(est == sv::Status::Ok, "encode returns Ok on a 4:3 landscape image");
    Check(shape.size() == 4 && shape[0] == 1 && shape[1] == 256,
          "embedding is [1,256,h,w], got " + std::to_string(shape.size()) + " dims");
    if (shape.size() == 4) {
        const size_t expect = static_cast<size_t>(shape[1] * shape[2] * shape[3]);
        Check(emb.size() == expect, "embedding element count matches its declared shape");
        Check(static_cast<int>(shape[2]) * 16 == 256,
              "embedding side x16 equals the encoder input side (16*16=256)");
    }
    bool finite = true, varied = false;
    float lo = 1e30f, hi = -1e30f;
    for (float v : emb) {
        if (!std::isfinite(v)) finite = false;
        lo = std::fmin(lo, v);
        hi = std::fmax(hi, v);
    }
    varied = (hi - lo) > 1e-3f;
    Check(finite, "embedding contains no NaN/Inf");
    Check(varied, "embedding is not constant (the resizer did work)");

    // Determinism: the same image must produce bit-identical embeddings, or the
    // studio's live mask would flicker between identical taps.
    std::vector<float> emb2;
    std::vector<int64_t> shape2;
    Check(enc->encode(img, &emb2, &shape2) == sv::Status::Ok, "second encode succeeds");
    Check(emb == emb2, "encoder is deterministic for the same input");

    // --- 4. the segmented point lands where the user tapped ------------------
    // Session-level: the real public API the studio calls.
    {
        sv::SegmentationSession session(sv::CreateOnnxEncoder(), sv::CreateOnnxDecoder());
        Check(session.load(eo, dopts) == sv::Status::Ok, "SegmentationSession::load");
        Check(session.prepare(img) == sv::Status::Ok, "SegmentationSession::prepare");
        Check(session.prepared(), "session reports prepared after prepare()");
        // The graph declares 256 (16*16); the session must report what the graph
        // said, not a hard-coded 1024. This is the assertion that catches "the
        // model size is duplicated in two places and one of them is stale".
        Check(session.model_size() == 256,
              "session model_size follows the graph (got " +
                  std::to_string(session.model_size()) + ")");
        // And the geometry the decoder will use must agree with it: a 400x300
        // image is padded 0 on the right and 44 rows on the bottom at side 256.
        const sv::sam::Geometry g = sv::sam::PreprocessGeometry(iw, ih, session.model_size());
        Check(g.new_w == 256 && g.new_h == 192, "geometry: 400x300 -> 256x192 in the model");
        Check(g.pad_right() == 0 && g.pad_bottom() == 64,
              "geometry: padding is on the bottom only (right=" + std::to_string(g.pad_right()) +
                  " bottom=" + std::to_string(g.pad_bottom()) + ")");
        // A tap outside the resized area (i.e. in the padding strip) must not be
        // silently clamped into the image: transform it and check it lands in the
        // padded region, which is what makes the decoder return "nothing".
        // The bottom-right image corner maps to (new_w, new_h) = (256,192); the
        // strip from 192 to 256 is padding, and nothing the user taps may land
        // there. This is the arithmetic a wrong pad silently breaks.
        float mx = 0.f, my = 0.f;
        sv::sam::TransformPoint(g, static_cast<float>(iw), static_cast<float>(ih), &mx, &my);
        Check(std::fabs(mx - 256.f) < 0.5f && std::fabs(my - 192.f) < 0.5f,
              "geometry: the image corner maps to (new_w,new_h) in model space");
        Check(my + g.pad_bottom() == session.model_size(),
              "geometry: content height + bottom pad == model side");
        Check(g.pad_right() + g.new_w == session.model_size() &&
                  g.pad_bottom() + g.new_h == session.model_size(),
              "geometry: content + pad accounts for the whole model side on both axes");

        // The decoder's oracle puts a hot pixel at row mask_side/8, col 5*mask_side/8
        // in *model* space — i.e. inside the padding strip for a 4:3 image. It must
        // never appear in the returned mask, and with keep_largest it must be gone:
        // that is the proof both that the pad is honoured and that the filter runs.
        std::vector<uint8_t> hot;
        int hw = 0, hh = 0;
        sv::PointPrompt hp{static_cast<float>(iw) / 2.f, static_cast<float>(ih) / 2.f, 1};
        Check(session.select({hp}, &hot, &hw, &hh) == sv::Status::Ok, "speck probe: select");
        // Model-space (32,40) at mask side 64 -> model px (128,160) -> image px
        // (128/0.64, 160/0.64) ... but row 160 is in the bottom padding (content is
        // 192 tall, so rows >= 192 are padding; 160 is content). Choose instead the
        // speck the graph adds at (mask_side/8, 5*mask_side/8) = (8, 40):
        // model px = ((40+0.5)*4, (8+0.5)*4) = (162, 34) -> image px (253, 53).
        const int sx = 253, sy = 53;
        size_t specks = 0;
        for (size_t i = 0; i < hot.size(); ++i)
            if (hot[i]) ++specks;
        Check(hot[static_cast<size_t>(sy) * iw + sx] == 0,
              "the stray positive pixel is filtered out (keep_largest)");
        Check(LargestComponent(hot, iw, ih) == specks,
              "the mask is a single connected component");

        struct Probe { float x, y; } probes[] = {{100.f, 80.f}, {300.f, 220.f}, {200.f, 150.f}};
        for (const auto& p : probes) {
            std::vector<uint8_t> mask;
            const bool ok = TapAt(session, p.x, p.y, iw, ih, &mask);
            Check(ok, "select() returns Ok with image-sized mask");
            Check(mask.size() == static_cast<size_t>(iw) * ih,
                  "mask length is exactly w*h (" + std::to_string(mask.size()) + ")");
            if (!ok) continue;
            const Blob b = Measure(mask, iw);
            Check(b.area > 0, "mask is non-empty");
            // The oracle disc has radius 40 model px; on a 400x300 image the
            // model is 256 px on the long side, so image-space radius is
            // 40 * (400/256) ~= 62 px. Allow generous slack for the coarse
            // 64-px mask grid; the point is centroid alignment, not exactness.
            const double dx = b.cx - p.x, dy = b.cy - p.y;
            Check(std::fabs(dx) <= 12.0 && std::fabs(dy) <= 12.0,
                  "mask centroid is within 12 px of the tap (" + std::to_string(dx) + "," +
                      std::to_string(dy) + ")");
        }

        // --- 5. portrait image: padding is on the other side -----------------
        const int pw = 300, ph = 400;
        std::vector<uint8_t> ppx = MakeImage(pw, ph);
        sv::ImageRGB pimg{ppx.data(), pw, ph, pw * 3};
        sv::SegmentationSession portrait(sv::CreateOnnxEncoder(), sv::CreateOnnxDecoder());
        Check(portrait.load(eo, dopts) == sv::Status::Ok, "portrait: load");
        Check(portrait.prepare(pimg) == sv::Status::Ok, "portrait: prepare");
        // Portrait: the *right* pad is non-zero (300x400 -> 192x256 at side 256),
        // so this is the case that a broken pad_right() cannot survive.
        const sv::sam::Geometry pg =
            sv::sam::PreprocessGeometry(pw, ph, portrait.model_size());
        Check(pg.new_w == 192 && pg.new_h == 256, "portrait geometry: 300x400 -> 192x256");
        Check(pg.pad_right() == 64 && pg.pad_bottom() == 0,
              "portrait: padding is on the right only (right=" + std::to_string(pg.pad_right()) +
                  " bottom=" + std::to_string(pg.pad_bottom()) + ")");

        // A wrong scale_x is INVISIBLE to a round-trip test: forward and inverse
        // both use it, so the error cancels and the centroid still lands on the
        // tap. The only way to see it is an absolute reference — the image corner
        // must land on (new_w, new_h), which for this portrait is (192, 256) and
        // not (256, 256).
        float pmx = 0.f, pmy = 0.f;
        sv::sam::TransformPoint(pg, static_cast<float>(pw), static_cast<float>(ph), &pmx, &pmy);
        Check(std::fabs(pmx - 192.f) < 0.5f && std::fabs(pmy - 256.f) < 0.5f,
              "portrait: image corner maps to (192,256) in model space, got (" +
                  std::to_string(pmx) + "," + std::to_string(pmy) + ")");

        std::vector<uint8_t> pmask;
        const bool pok = TapAt(portrait, 150.f, 120.f, pw, ph, &pmask);
        Check(pok, "portrait: select");
        if (pok) {
            const Blob b = Measure(pmask, pw);
            Check(std::fabs(b.cx - 150.0) <= 12.0 && std::fabs(b.cy - 120.0) <= 12.0,
                  "portrait: centroid tracks the tap (padding on the bottom)");
        }
    }

    // --- 5b. the `keep_largest` flag is honoured in both directions ----------
    {
        // Decode directly with the flag off: the stray pixel must survive, which
        // proves the previous test passes because the filter ran, not because the
        // graph never produced a speck.
        std::vector<uint8_t> loose;
        sv::PointPrompt p{200.f, 150.f, 1};
        const sv::Status st = dec->decode(emb, shape, iw, ih, {p}, /*keep_largest=*/false, &loose);
        Check(st == sv::Status::Ok, "decode with keep_largest=false returns Ok");
        if (st == sv::Status::Ok) {
            std::vector<uint8_t> tight;
            dec->decode(emb, shape, iw, ih, {p}, /*keep_largest=*/true, &tight);
            size_t a = 0, b = 0;
            for (size_t i = 0; i < loose.size(); ++i) {
                if (loose[i]) ++a;
                if (tight[i]) ++b;
            }
            Check(a > b, "keep_largest=true removes pixels that keep_largest=false keeps (" +
                             std::to_string(a) + " vs " + std::to_string(b) + ")");
        }
    }

    // --- 6. multi-tap: a second prompt must not corrupt the first ------------
    {
        sv::SegmentationSession session(sv::CreateOnnxEncoder(), sv::CreateOnnxDecoder());
        session.load(eo, dopts);
        session.prepare(img);
        std::vector<uint8_t> m1;
        int mw = 0, mh = 0;
        sv::PointPrompt a{120.f, 100.f, 1};
        Check(session.select({a}, &m1, &mw, &mh) == sv::Status::Ok, "two-tap: first select");
        sv::PointPrompt b{280.f, 200.f, 1};
        std::vector<uint8_t> m2;
        Check(session.select({a, b}, &m2, &mw, &mh) == sv::Status::Ok, "two-tap: second select");
        Check(m2.size() == m1.size(), "two-tap: mask size stable");
        // The synthetic oracle keys on the first point, so an accumulated prompt
        // must keep the first object selected rather than jump to the second.
        const Blob b1 = Measure(m1, iw), b2 = Measure(m2, iw);
        Check(std::fabs(b1.cx - b2.cx) <= 12.0,
              "two-tap: adding a prompt does not move the mask off the first object");
    }

    // --- 7. failure paths must not produce a mask ---------------------------
    {
        sv::SegmentationSession session(sv::CreateOnnxEncoder(), sv::CreateOnnxDecoder());
        // Not loaded: every call must refuse, and must not write into the mask.
        std::vector<uint8_t> mask(16, 7);
        int mw = 0, mh = 0;
        sv::PointPrompt p{1.f, 1.f, 1};
        const sv::Status st = session.select({p}, &mask, &mw, &mh);
        Check(st != sv::Status::Ok, "select before load fails");
        Check(mask.size() == 16 && mask[0] == 7, "failed select leaves the mask untouched");
    }
    {
        // Embedding/shape mismatch: the decoder must reject rather than read OOB.
        std::vector<uint8_t> mask;
        std::vector<float> wrong(4, 0.f);
        std::vector<int64_t> wrong_shape{1, 256, 16, 16};
        sv::PointPrompt p{1.f, 1.f, 1};
        const sv::Status st = dec->decode(wrong, wrong_shape, 32, 32, {p}, true, &mask);
        Check(st == sv::Status::InvalidArgument, "decoder rejects an undersized embedding");
    }

    // --- 8. largest-component filter is wired in ----------------------------
    {
        // A mask with one big blob and one speck: after filtering, the speck is
        // gone. Exercised directly so it is independent of the model.
        const int w = 32, h = 32;
        std::vector<uint8_t> m(static_cast<size_t>(w) * h, 0);
        for (int y = 4; y < 14; ++y)
            for (int x = 4; x < 14; ++x) m[static_cast<size_t>(y) * w + x] = 255;
        m[static_cast<size_t>(28) * w + 28] = 255;  // single-pixel speck
        sv::KeepLargestComponent(m.data(), w, h);
        Check(m[static_cast<size_t>(28) * w + 28] == 0, "speck removed by KeepLargestComponent");
        Check(m[static_cast<size_t>(9) * w + 9] == 255, "main blob survives");
    }

    enc->release();
    dec->release();
    Check(!enc->ready() && !dec->ready(), "release() clears readiness");

    // --- 9. the second supported interface (Acly/MobileSAM export) -----------
    // NHWC encoder input, point_coords/point_labels/mask_input/has_mask_input/
    // orig_im_size decoder, masks returned at original resolution. Optional:
    // pass the pair as argv[3], argv[4].
    if (argc >= 5) {
        CheckTapAccuracy(argv[3], argv[4], "acly-interface");
        // Encoder-side absolute checks, one per convention.
        CheckEncoderGeometry(argv[1], argv[2], /*normalised=*/true, "nchw-encoder");
        CheckEncoderGeometry(argv[3], argv[4], /*normalised=*/false, "acly-encoder");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
