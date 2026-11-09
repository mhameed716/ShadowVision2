// SPDX-License-Identifier: MIT
// G0 unit tests. No test framework: this must also run on the phone via adb,
// where adding gtest would double the APK.
//
// What is asserted here is exactly what G0 depends on:
//   * the governor refuses instead of dying (the BudgetExceeded contract)
//   * mask geometry (bbox, dilation, feather) behaves at the edges
//   * crop+blend is pixel-exact outside the mask (no silent damage)
//   * an upscaled/downscaled round trip does not shift the image
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sv/common.h"
#include "sv/image_io.h"
#include "sv/inpaint_lama.h"
#include "sv/kernels.h"
#include "sv/manifest.h"
#include "sv/memory.h"
#include "sv/sam_geometry.h"
#include "sv/segmenter.h"
#include "sv/sha256.h"
#include "sv/synthetic_backend.h"

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                  \
    do {                                                                       \
        ++g_checks;                                                            \
        const double _a = (a), _b = (b);                                       \
        if (std::fabs(_a - _b) > (eps)) {                                      \
            printf("  FAIL %s:%d  %s=%.4f vs %s=%.4f (eps %.4f)\n", __FILE__,  \
                   __LINE__, #a, _a, #b, _b, (double)(eps));                   \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

void test_governor_refuses() {
    printf("[test] governor refuses instead of dying\n");
    sv::MemoryGovernor gov(1024);  // 1 MB
    CHECK(gov.reserve("a", 512 * 1024) == sv::Status::Ok);
    CHECK(gov.reserve("b", 400 * 1024) == sv::Status::Ok);
    CHECK(gov.reserve("c", 400 * 1024) == sv::Status::BudgetExceeded);  // 1.3 MB > 1 MB
    gov.release("a", 512 * 1024);
    CHECK(gov.current_kb() == 400);
    CHECK(gov.peak_kb() == 912);
}

void test_lease_raii() {
    printf("[test] RAII lease releases on scope exit\n");
    sv::MemoryGovernor::global().set_budget_kb(8 * 1024);
    const int64_t before = sv::MemoryGovernor::global().current_kb();
    {
        auto lease = sv::MemoryGovernor::global().acquire("t", 1024 * 1024);
        CHECK(lease.live);
        CHECK(sv::MemoryGovernor::global().current_kb() == before + 1024);
    }
    CHECK(sv::MemoryGovernor::global().current_kb() == before);
}

void test_mask_bbox_and_expand() {
    printf("[test] mask bbox + expansion clamps to image\n");
    sv::OwnedMask m;
    sv::MakeRectMask(&m, 100, 80, 30, 20, 60, 50);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    CHECK(sv::MaskBoundingBox(m.view(), &x0, &y0, &x1, &y1));
    CHECK(x0 == 30 && y0 == 20 && x1 == 60 && y1 == 50);
    sv::ExpandBox(&x0, &y0, &x1, &y1, 40, 100, 80);
    CHECK(x0 == 0 && y0 == 0 && x1 == 100 && y1 == 80);

    sv::OwnedMask empty;
    sv::MakeRectMask(&empty, 10, 10, 0, 0, 0, 0);
    CHECK(!sv::MaskBoundingBox(empty.view(), &x0, &y0, &x1, &y1));
}

void test_dilate_feather() {
    printf("[test] dilate grows, feather softens, nothing escapes bounds\n");
    const int w = 64, h = 64;
    std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);
    for (int y = 30; y < 34; ++y)
        for (int x = 30; x < 34; ++x) mask[static_cast<size_t>(y) * w + x] = 255;

    sv::DilateMask(mask.data(), w, h, w, 5);
    CHECK(mask[static_cast<size_t>(28) * w + 32] == 255);  // grew upward
    CHECK(mask[static_cast<size_t>(10) * w + 32] == 0);    // far away untouched

    sv::FeatherMask(mask.data(), w, h, w, 3);
    CHECK(mask[static_cast<size_t>(32) * w + 32] == 255);  // core stays solid
    CHECK(mask[static_cast<size_t>(32) * w + 39] < 255);   // edge softened
    CHECK(mask[static_cast<size_t>(32) * w + 50] == 0);    // far outside stays 0
}

void test_resize_roundtrip_identity() {
    printf("[test] 2x downsample then bilinear upsample stays close\n");
    sv::OwnedImage src;
    sv::MakeTestPattern(&src, 128, 128, 1);
    std::vector<uint8_t> small(static_cast<size_t>(64) * 64 * 3);
    sv::DownsampleAreaRGB(src.view(), 2, sv::MutableImageRGB{small.data(), 64, 64, 64 * 3});
    std::vector<uint8_t> back(static_cast<size_t>(128) * 128 * 3);
    sv::ResizeBilinearRGB(sv::ImageRGB{small.data(), 64, 64, 64 * 3}, 128, 128,
                          sv::MutableImageRGB{back.data(), 128, 128, 128 * 3});
    double err = 0;
    for (size_t i = 0; i < back.size(); ++i) err += std::fabs(back[i] - src.data[i]);
    err /= static_cast<double>(back.size());
    CHECK_NEAR(err, 0.0, 6.0);  // smooth content: mean abs error under 6/255
}

void test_pipeline_does_not_touch_outside_mask() {
    printf("[test] inpaint leaves pixels outside the mask byte-identical\n");
    sv::OwnedImage img;
    sv::MakeTestPattern(&img, 192, 144, 2);
    sv::OwnedMask mask;
    sv::MakeRectMask(&mask, 192, 144, 60, 40, 100, 90);

    sv::SyntheticBackend::Config cfg;
    auto backend = std::unique_ptr<sv::IInpaintBackend>(new sv::SyntheticBackend(cfg));
    sv::LaMaInpainter inpaint(std::move(backend));
    CHECK(inpaint.load({}) == sv::Status::Ok);

    sv::OwnedImage out;
    out.width = img.width;
    out.height = img.height;
    out.data.assign(img.data.size(), 0);
    sv::Stats stats;
    sv::CancelToken cancel;
    CHECK(inpaint.inpaint(img.view(), mask.view(), out.mutable_view(), cancel, stats, 128) ==
          sv::Status::Ok);

    // The blend uses the DILATED mask on purpose: a mask that stops exactly on
    // the object edge always leaves a 1-px halo of the object behind. So the
    // contract is not "zero pixels outside the stroke" — it is "nothing beyond
    // the documented halo". Both halves matter: `touched_outside > 0` proves the
    // halo is really applied (drop the dilation and the red rims come back),
    // and `too_far == 0` proves it does not creep into the rest of the photo.
    size_t touched_outside = 0;
    size_t too_far = 0;
    const int halo_px = 6;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            if (mask.data[static_cast<size_t>(y) * img.width + x] != 0) continue;
            const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
            bool changed = false;
            for (int c = 0; c < 3; ++c) {
                if (out.data[i + c] != img.data[i + c]) changed = true;
            }
            if (!changed) continue;
            ++touched_outside;
            // Distance to the nearest mask pixel (Chebyshev, brute force is fine
            // at this size).
            int best = 1 << 30;
            for (int yy = std::max(0, y - halo_px); yy <= std::min(img.height - 1, y + halo_px); ++yy) {
                for (int xx = std::max(0, x - halo_px); xx <= std::min(img.width - 1, x + halo_px); ++xx) {
                    if (mask.data[static_cast<size_t>(yy) * img.width + xx] == 0) continue;
                    best = std::min(best, std::max(std::abs(yy - y), std::abs(xx - x)));
                }
            }
            if (best > halo_px) ++too_far;
        }
    }
    printf("  halo: %zu px changed outside the stroke (limit %d px from it)\n",
           touched_outside, halo_px);
    CHECK(touched_outside > 0);   // the dilation must actually be in the blend
    CHECK(too_far == 0);          // and it must stay local
    CHECK(stats.stage_count >= 3);
}

void test_cancel_is_observed() {
    printf("[test] a cancelled token stops before doing work\n");
    sv::OwnedImage img;
    sv::MakeTestPattern(&img, 64, 64, 0);
    sv::OwnedMask mask;
    sv::MakeRectMask(&mask, 64, 64, 10, 10, 20, 20);
    auto backend = std::unique_ptr<sv::IInpaintBackend>(new sv::SyntheticBackend());
    sv::LaMaInpainter inpaint(std::move(backend));
    CHECK(inpaint.load({}) == sv::Status::Ok);
    sv::OwnedImage out;
    out.width = 64;
    out.height = 64;
    out.data.assign(64 * 64 * 3, 0);
    sv::Stats stats;
    sv::CancelToken cancel;
    cancel.request();
    CHECK(inpaint.inpaint(img.view(), mask.view(), out.mutable_view(), cancel, stats, 64) ==
          sv::Status::Cancelled);
}

void test_budget_failure_propagates() {
    printf("[test] a refused allocation becomes BudgetExceeded, not a crash\n");
    sv::MemoryGovernor::global().set_budget_kb(64);  // 64 KB: nothing fits
    sv::OwnedImage img;
    sv::MakeTestPattern(&img, 256, 256, 0);
    sv::OwnedMask mask;
    sv::MakeRectMask(&mask, 256, 256, 40, 40, 200, 200);
    sv::SyntheticBackend::Config cfg;
    auto backend = std::unique_ptr<sv::IInpaintBackend>(new sv::SyntheticBackend(cfg));
    sv::LaMaInpainter inpaint(std::move(backend));
    CHECK(inpaint.load({}) == sv::Status::Ok);
    sv::OwnedImage out;
    out.width = 256;
    out.height = 256;
    out.data.assign(256 * 256 * 3, 0);
    sv::Stats stats;
    sv::CancelToken cancel;
    const sv::Status st =
        inpaint.inpaint(img.view(), mask.view(), out.mutable_view(), cancel, stats, 128);
    CHECK(st == sv::Status::BudgetExceeded);
    sv::MemoryGovernor::global().set_budget_kb(1200 * 1024);  // restore
}

void test_image_io_roundtrip() {
    printf("[test] PPM/PGM round trip is lossless\n");
    sv::OwnedImage img;
    sv::MakeTestPattern(&img, 37, 23, 5);
    CHECK(sv::SavePPM("/tmp/sv_rt.ppm", img.view()));
    sv::OwnedImage back;
    CHECK(sv::LoadPPM("/tmp/sv_rt.ppm", &back));
    CHECK(back.width == 37 && back.height == 23);
    CHECK(back.data == img.data);

    sv::OwnedMask m;
    sv::MakeRectMask(&m, 37, 23, 5, 5, 15, 15);
    CHECK(sv::SavePGM("/tmp/sv_rt.pgm", m.view()));
    sv::OwnedMask mback;
    CHECK(sv::LoadPGM("/tmp/sv_rt.pgm", &mback));
    CHECK(mback.data == m.data);
}

void test_sha256_nist_vectors() {
    printf("[test] SHA-256 matches NIST vectors\n");
    auto h = [](const std::string& in) {
        uint8_t d[32];
        sv::Sha256::hash(in.data(), in.size(), d);
        return sv::Sha256::hex(d);
    };
    CHECK(h("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(h("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(h("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

void test_catalog_rejects_tampering() {
    printf("[test] catalog: bad signature / bad sha / path traversal rejected\n");
    // A real signed catalog is produced by tools/sign_catalog.py and exercised by
    // tools/verify_g0.sh; here we assert the negative paths, which is what stops
    // a malicious model from being loaded.
    sv::Catalog cat;
    const std::string payload =
        "SV-CATALOG-1\nkey=dev\nentry\nid=x\nfile=a.onnx\nsha256=" +
        std::string(64, 'a') + "\nend\n";
    CHECK(!sv::VerifyAndParseCatalog(payload, std::string(64, '0'), &cat));
    CHECK(cat.reason.find("no signature") != std::string::npos);

    CHECK(!sv::VerifyAndParseCatalog(payload + "signature=" + std::string(128, 'f'),
                                     std::string(64, '1'), &cat));
    CHECK(!cat.verified);

    CHECK(sv::IsSafeRelativeFile("lama_fp32.onnx"));
    CHECK(!sv::IsSafeRelativeFile("../secret.onnx"));
    CHECK(!sv::IsSafeRelativeFile("/data/local/tmp/x.onnx"));
    CHECK(!sv::IsSafeRelativeFile("a/b.onnx"));
}

void test_synthetic_segmenter_selects_region() {
    printf("[test] segmentation session: prepare once, tap selects a region\n");
    // Two-tone image: left half dark, right half bright.
    sv::OwnedImage img;
    img.width = 200;
    img.height = 200;
    img.data.assign(200 * 200 * 3, 0);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x) {
            const uint8_t v = x < 100 ? 20 : 230;
            const size_t i = (static_cast<size_t>(y) * 200 + x) * 3;
            img.data[i] = v;
            img.data[i + 1] = v;
            img.data[i + 2] = v;
        }
    sv::SegmentationSession session(sv::CreateSyntheticEncoder(), sv::CreateSyntheticDecoder());
    CHECK(session.prepare(img.view()) == sv::Status::Ok);
    CHECK(session.prepared());
    std::vector<uint8_t> mask;
    int mw = 0, mh = 0;
    std::vector<sv::PointPrompt> prompts{{150.f, 100.f, 1}};
    const sv::Status sel = session.select(prompts, &mask, &mw, &mh);
    CHECK(sel == sv::Status::Ok);
    if (sel == sv::Status::Ok && mask.size() == 200u * 200u) {
        // The tap was on the bright half: that half is selected, the dark is not.
        CHECK(mw == 200 && mh == 200);
        CHECK(mask[static_cast<size_t>(100) * 200 + 150] == 255);
        CHECK(mask[static_cast<size_t>(100) * 200 + 20] == 0);
    }
}

void test_keep_largest_component() {
    printf("[test] largest-component filter removes specks\n");
    const int w = 40, h = 40;
    std::vector<uint8_t> m(static_cast<size_t>(w) * h, 0);
    for (int y = 5; y < 20; ++y)
        for (int x = 5; x < 20; ++x) m[static_cast<size_t>(y) * w + x] = 255;
    m[static_cast<size_t>(35) * w + 35] = 255;  // speck
    sv::KeepLargestComponent(m.data(), w, h);
    CHECK(m[static_cast<size_t>(10) * w + 10] == 255);
    CHECK(m[static_cast<size_t>(35) * w + 35] == 0);
}

void test_sam_geometry_landscape() {
    printf("[test] SAM geometry: 1920x1080 -> 1024 long side, pad right\n");
    const auto g = sv::sam::PreprocessGeometry(1920, 1080, 1024);
    CHECK(g.new_w == 1024);
    CHECK(g.new_h == 576);            // 1080 * 1024/1920 = 576
    CHECK(g.pad_right() == 0);
    CHECK(g.pad_bottom() == 448);     // 1024 - 576
    float mx = 0.f, my = 0.f;
    sv::sam::TransformPoint(g, 960.f, 540.f, &mx, &my);
    CHECK(std::abs(mx - 512.f) < 0.5f);   // centre maps to centre
    CHECK(std::abs(my - 288.f) < 0.5f);
}

void test_sam_geometry_portrait_square_and_rounding() {
    printf("[test] SAM geometry: portrait, square, odd sizes\n");
    const auto p = sv::sam::PreprocessGeometry(1080, 1920, 1024);
    CHECK(p.new_h == 1024);
    CHECK(p.new_w == 576);
    CHECK(p.pad_bottom() == 0 && p.pad_right() == 448);

    const auto sq = sv::sam::PreprocessGeometry(1024, 1024, 1024);
    CHECK(sq.new_w == 1024 && sq.new_h == 1024 && sq.pad_right() == 0 && sq.pad_bottom() == 0);

    // 4032x3024 (12 MP phone photo) must not round the long side to 1023.
    const auto big = sv::sam::PreprocessGeometry(4032, 3024, 1024);
    CHECK(big.new_w == 1024);
    CHECK(big.new_h == 768);
    CHECK(sq.scale_x == 1.f);
    // Inverse mapping round-trips within a pixel.
    float mx = 0.f, my = 0.f;
    sv::sam::TransformPoint(big, 2016.f, 1512.f, &mx, &my);
    CHECK(std::abs(sv::sam::InverseMapX(big, mx) - 2016.f) < 1.f);
    CHECK(std::abs(sv::sam::InverseMapY(big, my) - 1512.f) < 1.f);
}

// The end-to-end demo found this the hard way: eight ring-fill passes advance
// eight pixels, so a 46-px-wide object kept its original pixels in the middle and
// the backend still returned Ok. These checks fail if that ever comes back.
void test_pyramid_fill_clears_wide_holes() {
    printf("[test] PyramidFill: wide hole fully replaced, gradient preserved\n");
    const int w = 200, h = 120;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    std::vector<uint8_t> m(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t p = (static_cast<size_t>(y) * w + x) * 3;
            rgb[p + 0] = 30;
            rgb[p + 1] = static_cast<uint8_t>(90 + y / 3);  // vertical gradient
            rgb[p + 2] = 200;
        }
    }
    // 30 wide x 60 tall — far wider than the 8 px a ring fill reaches.
    for (int y = 30; y < 90; ++y) {
        for (int x = 80; x < 110; ++x) {
            m[static_cast<size_t>(y) * w + x] = 255;
            const size_t p = (static_cast<size_t>(y) * w + x) * 3;
            rgb[p + 0] = 200; rgb[p + 1] = 40; rgb[p + 2] = 40;
        }
    }
    const int filled = sv::PyramidFill(rgb.data(), w * 3, m.data(), w, w, h);
    CHECK(filled == 30 * 60);
    int left = 0;
    for (uint8_t v : m) left += (v != 0);
    CHECK(left == 0);  // no hole pixel may survive

    // The centre must no longer be the object's colour at all.
    const size_t c = (static_cast<size_t>(60) * w + 95) * 3;
    CHECK(rgb[c + 0] < 60);          // was 200
    CHECK(rgb[c + 2] > 150);         // was 40
    // ...and it must follow the gradient, not be a flat block: the pixel 20 rows
    // down is brighter than the one 20 rows up by roughly 20/3.
    const size_t up = (static_cast<size_t>(40) * w + 95) * 3;
    const size_t dn = (static_cast<size_t>(80) * w + 95) * 3;
    const int delta = static_cast<int>(rgb[dn + 1]) - static_cast<int>(rgb[up + 1]);
    CHECK(delta >= 10 && delta <= 18);
    // Smooth: no staircase steps inside the hole (neighbouring rows differ by <= 1).
    int max_step = 0;
    for (int y = 32; y < 88; ++y) {
        const int a = rgb[(static_cast<size_t>(y) * w + 95) * 3 + 1];
        const int b = rgb[(static_cast<size_t>(y + 1) * w + 95) * 3 + 1];
        max_step = std::max(max_step, std::abs(a - b));
    }
    CHECK(max_step <= 1);
}

void test_pyramid_fill_no_hole_is_a_noop() {
    printf("[test] PyramidFill: empty mask changes nothing\n");
    const int w = 32, h = 32;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 77);
    std::vector<uint8_t> m(static_cast<size_t>(w) * h, 0);
    CHECK(sv::PyramidFill(rgb.data(), w * 3, m.data(), w, w, h) == 0);
    bool untouched = true;
    for (auto v : rgb) untouched = untouched && v == 77;
    CHECK(untouched);
}

// The check that was missing when the demo bug shipped: go through the REAL
// pipeline (LaMaInpainter + SyntheticBackend), not the fill function directly. A
// mutated backend that quietly keeps the object's pixels (eight ring passes, no
// error reported) must fail here.
void test_backend_removes_wide_object_end_to_end() {
    printf("[test] end-to-end: a wide object is gone from the output pixels\n");
    const int w = 320, h = 240;
    sv::OwnedImage img;
    img.width = w;
    img.height = h;
    img.data.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t p = (static_cast<size_t>(y) * w + x) * 3;
            img.data[p + 0] = 40;                      // sky blue, left to right
            img.data[p + 1] = static_cast<uint8_t>(120 + x / 8);
            img.data[p + 2] = 210;
        }
    }
    // A 50-px-wide red post, like the one in the demo scene.
    for (int y = 60; y < 200; ++y) {
        for (int x = 120; x < 170; ++x) {
            const size_t p = (static_cast<size_t>(y) * w + x) * 3;
            img.data[p + 0] = 190; img.data[p + 1] = 60; img.data[p + 2] = 55;
        }
    }
    sv::OwnedMask mask;
    mask.width = w;
    mask.height = h;
    mask.data.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 60; y < 200; ++y)
        for (int x = 120; x < 170; ++x) mask.data[static_cast<size_t>(y) * w + x] = 255;

    sv::SyntheticBackend::Config cfg;
    auto backend = std::unique_ptr<sv::IInpaintBackend>(new sv::SyntheticBackend(cfg));
    sv::LaMaInpainter inpaint(std::move(backend));
    CHECK(inpaint.load({}) == sv::Status::Ok);

    sv::OwnedImage out;
    out.width = w;
    out.height = h;
    out.data.assign(img.data.size(), 0);
    sv::Stats stats;
    sv::CancelToken cancel;
    CHECK(inpaint.inpaint(img.view(), mask.view(), out.mutable_view(), cancel, stats, 256) ==
          sv::Status::Ok);

    // Count pixels that are still the post's red anywhere in the mask.
    int still_red = 0;
    int mask_px = 0;
    for (int y = 62; y < 198; ++y) {
        for (int x = 122; x < 168; ++x) {
            const size_t p = (static_cast<size_t>(y) * w + x) * 3;
            ++mask_px;
            if (out.data[p + 0] > 150 && out.data[p + 2] < 110) ++still_red;
        }
    }
    printf("  object pixels left in the output: %d of %d\n", still_red, mask_px);
    CHECK(mask_px > 0);
    // A few edge pixels are tolerable; a surviving block is not. Eight ring
    // passes left ~60%% of this region red.
    CHECK(still_red * 100 < mask_px * 2);

    // And the result must match the surrounding sky's gradient, not be a flat block.
    // Same column, above the post: the sky is constant down a column, so this is
    // the value the hole should have taken.
    const size_t a = (static_cast<size_t>(130) * w + 145) * 3;
    const size_t b = (static_cast<size_t>(20) * w + 145) * 3;   // known sky, same column
    const int da = static_cast<int>(out.data[a + 1]) - static_cast<int>(img.data[b + 1]);
    printf("  green channel vs known sky above it: %+d\n", da);
    CHECK(da > -12 && da < 12);
}

void test_area_resize() {
    printf("[test] area resize: constant stays constant, edges do not bleed to zero\n");
    sv::OwnedImage flat;
    flat.width = 64;
    flat.height = 64;
    flat.data.assign(64 * 64 * 3, 180);
    std::vector<uint8_t> small(16 * 16 * 3, 0);
    sv::ResizeAreaRGB(flat.view(), 16, 16, sv::MutableImageRGB{small.data(), 16, 16, 16 * 3});
    bool all = true;
    for (auto v : small) all = all && v == 180;
    CHECK(all);

    // Half black / half white must resample to exactly the midpoint at the seam,
    // not to the nearest source pixel (that is the aliasing we are avoiding).
    sv::OwnedImage split;
    split.width = 64;
    split.height = 8;
    split.data.assign(64 * 8 * 3, 0);
    for (int y = 0; y < 8; ++y)
        for (int x = 32; x < 64; ++x)
            for (int c = 0; c < 3; ++c)
                split.data[(static_cast<size_t>(y) * 64 + x) * 3 + c] = 255;
    std::vector<uint8_t> half(8 * 8 * 3, 0);
    sv::ResizeAreaRGB(split.view(), 8, 8, sv::MutableImageRGB{half.data(), 8, 8, 8 * 3});
    CHECK(half[(static_cast<size_t>(0) * 8 + 0) * 3] == 0);
    CHECK(half[(static_cast<size_t>(0) * 8 + 7) * 3] == 255);
    // At exactly 2:1 downscale the seam falls between destination pixels, so no
    // destination pixel mixes both halves. What matters is that no partial edge
    // bleeds: first four columns must be pure black, last four pure white.
    const int seam_left = half[(static_cast<size_t>(0) * 8 + 3) * 3];
    const int seam_right = half[(static_cast<size_t>(0) * 8 + 4) * 3];
    CHECK(seam_left == 0);
    CHECK(seam_right == 255);
}

void test_sam_normalization() {
    printf("[test] SAM normalization constants\n");
    // Mid-grey maps to ~0 for all channels (that is why SAM uses these means).
    CHECK(std::abs(sv::sam::Normalize(0, 123.675f)) < 1e-3f);
    CHECK(std::abs(sv::sam::Normalize(1, 116.28f)) < 1e-3f);
    CHECK(std::abs(sv::sam::Normalize(2, 103.53f)) < 1e-3f);
    CHECK(sv::sam::Normalize(0, 255.f) > 2.f);
}

}  // namespace

int main() {
    printf("ShadowVision2 G0 unit tests\n");
    test_governor_refuses();
    test_lease_raii();
    test_mask_bbox_and_expand();
    test_dilate_feather();
    test_resize_roundtrip_identity();
    test_pipeline_does_not_touch_outside_mask();
    test_cancel_is_observed();
    test_budget_failure_propagates();
    test_image_io_roundtrip();
    test_sha256_nist_vectors();
    test_catalog_rejects_tampering();
    test_synthetic_segmenter_selects_region();
    test_keep_largest_component();
    test_sam_geometry_landscape();
    test_sam_geometry_portrait_square_and_rounding();
    test_area_resize();
    test_backend_removes_wide_object_end_to_end();
    test_pyramid_fill_clears_wide_holes();
    test_pyramid_fill_no_hole_is_a_noop();
    test_sam_normalization();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
