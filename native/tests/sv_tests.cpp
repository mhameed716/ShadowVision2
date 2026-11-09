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

    size_t touched_outside = 0;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            if (mask.data[static_cast<size_t>(y) * img.width + x] != 0) continue;
            const size_t i = (static_cast<size_t>(y) * img.width + x) * 3;
            for (int c = 0; c < 3; ++c) {
                if (out.data[i + c] != img.data[i + c]) ++touched_outside;
            }
        }
    }
    // The blend only writes inside the crop, and only where mask>0, so this is
    // an exact contract — allow zero.
    CHECK(touched_outside == 0);
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
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
