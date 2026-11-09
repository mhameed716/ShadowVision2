// SPDX-License-Identifier: MIT
// sv_g0 — the Gate 0 harness.
//
// Runs the *real* pipeline geometry (crop -> resize -> dilate/feather -> backend
// -> resize back -> blend) against either a synthetic backend or an ONNX/LaMa
// backend, and prints a gate report with PASS/FAIL per criterion from
// docs/decisions-ar.md §1.
//
// On a phone this same binary is built with the NDK and pushed with `adb push`,
// so the numbers come from the device, not from a laptop.
//
// Usage:
//   sv_g0 --image in.ppm --mask in.pgm [--backend synthetic|lama]
//         [--model path.onnx] [--work 512] [--repeat 3] [--budget-mb 1200]
//         [--out out.ppm] [--json] [--fail-run] [--fail-alloc]
//         [--emulate-ns 120] [--extract DIR --gen-test]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sv/backend.h"
#include "sv/common.h"
#include "sv/image_io.h"
#include "sv/inpaint_lama.h"
#include "sv/memory.h"
#include "sv/synthetic_backend.h"

#if SV_WITH_ORT
#include "sv/ort_backend.h"
#endif

namespace {

struct Args {
    std::string image;
    std::string mask;
    std::string model;
    std::string out;
    std::string extract_dir;
    std::string backend = "synthetic";
    int work = 512;
    int repeat = 1;
    int budget_mb = 1200;
    int rss_limit_mb = 700;
    double emulate_ns = 0.0;
    bool json = false;
    bool fail_run = false;
    bool fail_alloc = false;
    bool gen_test = false;
    bool square = false;
};

void usage() {
    fprintf(stderr,
            "sv_g0 — ShadowVision2 Gate 0 harness\n"
            "  --image PATH       input RGB (PPM P6)\n"
            "  --mask PATH        input mask (PGM P5, 255 = hole)\n"
            "  --backend NAME     synthetic | lama   (default: synthetic)\n"
            "  --model PATH       .onnx model for the lama backend\n"
            "  --work N           network long side (default 512)\n"
            "  --repeat N         timed repetitions (default 1)\n"
            "  --budget-mb N      tracked memory budget (default 1200)\n"
            "  --rss-limit-mb N   process peak-RSS limit for the device gate (default 700)\n"
            "  --emulate-ns F     synthetic backend cost ns/pixel\n"
            "  --out PATH         write the result PPM\n"
            "  --square           force W==H==--work (required for the 512 ONNX export)\n"
            "  --json             machine-readable report\n"
            "  --fail-run         inject a backend failure\n"
            "  --fail-alloc       inject a budget failure\n"
            "  --gen-test DIR     write test.ppm/test.pgm into DIR and exit\n");
}

bool arg_eq(const char* a, const char* b) { return strcmp(a, b) == 0; }

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (arg_eq(argv[i], "--image") && i + 1 < argc) a.image = argv[++i];
        else if (arg_eq(argv[i], "--mask") && i + 1 < argc) a.mask = argv[++i];
        else if (arg_eq(argv[i], "--model") && i + 1 < argc) a.model = argv[++i];
        else if (arg_eq(argv[i], "--out") && i + 1 < argc) a.out = argv[++i];
        else if (arg_eq(argv[i], "--backend") && i + 1 < argc) a.backend = argv[++i];
        else if (arg_eq(argv[i], "--work") && i + 1 < argc) a.work = atoi(argv[++i]);
        else if (arg_eq(argv[i], "--repeat") && i + 1 < argc) a.repeat = atoi(argv[++i]);
        else if (arg_eq(argv[i], "--budget-mb") && i + 1 < argc) a.budget_mb = atoi(argv[++i]);
        else if (arg_eq(argv[i], "--rss-limit-mb") && i + 1 < argc) a.rss_limit_mb = atoi(argv[++i]);
        else if (arg_eq(argv[i], "--emulate-ns") && i + 1 < argc) a.emulate_ns = atof(argv[++i]);
        else if (arg_eq(argv[i], "--gen-test") && i + 1 < argc) {
            a.gen_test = true;
            a.extract_dir = argv[++i];
        } else if (arg_eq(argv[i], "--json")) a.json = true;
        else if (arg_eq(argv[i], "--fail-run")) a.fail_run = true;
        else if (arg_eq(argv[i], "--fail-alloc")) a.fail_alloc = true;
        else if (arg_eq(argv[i], "--help") || arg_eq(argv[i], "-h")) {
            usage();
            exit(0);
        }
    }
    return a;
}

struct Gate {
    const char* name;
    double measured;
    double threshold;
    bool lower_is_better;
    bool pass() const { return lower_is_better ? measured <= threshold : measured >= threshold; }
};

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);

    // ---- test data generation mode -------------------------------------
    if (args.gen_test) {
        sv::OwnedImage img;
        sv::MakeTestPattern(&img, 1920, 1440, 3);
        sv::OwnedMask mask;
        // A person-sized hole off-centre, plus a thin power-line stroke.
        sv::MakeRectMask(&mask, 1920, 1440, 780, 420, 1080, 1100);
        for (int x = 200; x < 1700; x += 3) {
            for (int y = 120; y < 126; ++y) mask.data[static_cast<size_t>(y) * 1920 + x] = 255;
        }
        const std::string d = args.extract_dir + "/";
        const bool ok = sv::SavePPM(d + "test.ppm", img.view()) &&
                        sv::SavePGM(d + "test.pgm", mask.view());
        printf("wrote %stest.ppm (%dx%d) and %stest.pgm\n", d.c_str(), img.width, img.height,
               d.c_str());
        return ok ? 0 : 1;
    }

    if (args.image.empty() || args.mask.empty()) {
        usage();
        return 2;
    }

    sv::MemoryGovernor::global().set_budget_kb(static_cast<int64_t>(args.budget_mb) * 1024);

    sv::OwnedImage img;
    sv::OwnedMask mask;
    if (!sv::LoadPPM(args.image, &img)) {
        fprintf(stderr, "cannot read image: %s\n", args.image.c_str());
        return 2;
    }
    if (!sv::LoadPGM(args.mask, &mask)) {
        fprintf(stderr, "cannot read mask: %s\n", args.mask.c_str());
        return 2;
    }
    if (mask.width != img.width || mask.height != img.height) {
        fprintf(stderr, "mask size %dx%d != image %dx%d\n", mask.width, mask.height, img.width,
                img.height);
        return 2;
    }

    // ---- backend selection ---------------------------------------------
    std::unique_ptr<sv::IInpaintBackend> backend;
    sv::BackendOptions opts;
    opts.model_path = args.model;
    opts.intra_op_threads = 2;
    opts.use_npu = false;  // G0 measures the CPU floor first; NPU is a separate run.

    if (args.backend == "synthetic") {
        sv::SyntheticBackend::Config cfg;
        cfg.emulated_ns_per_pixel = args.emulate_ns;
        cfg.fail_run = args.fail_run;
        cfg.fail_alloc = args.fail_alloc;
        backend.reset(new sv::SyntheticBackend(cfg));
    } else if (args.backend == "lama") {
#if SV_WITH_ORT
        if (args.model.empty()) {
            fprintf(stderr, "lama backend requires --model\n");
            return 2;
        }
        backend.reset(sv::CreateOrtBackend());
        a.square = true;  // the exported graph is fixed 512x512
#else
        fprintf(stderr,
                "this build has no ONNX Runtime support (configure with -DSV_WITH_ORT=ON)\n");
        return 2;
#endif
    } else {
        fprintf(stderr, "unknown backend: %s\n", args.backend.c_str());
        return 2;
    }

    sv::LaMaInpainter inpaint(std::move(backend));
    inpaint.set_square_input(args.square);
    if (inpaint.load(opts) != sv::Status::Ok) {
        fprintf(stderr, "backend load failed\n");
        return 3;
    }

    sv::OwnedImage out;
    out.width = img.width;
    out.height = img.height;
    out.data.assign(static_cast<size_t>(img.width) * img.height * 3, 0);

    std::vector<double> totals;
    sv::Stats last;
    sv::CancelToken cancel;
    sv::Status st = sv::Status::Ok;

    for (int i = 0; i < args.repeat; ++i) {
        const double t0 = sv::MemoryGovernor::process_peak_rss_kb();
        (void)t0;
        last = sv::Stats{};
        st = inpaint.inpaint(img.view(), mask.view(), out.mutable_view(), cancel, last, args.work);
        totals.push_back(last.total_ms);
        if (st != sv::Status::Ok) break;
    }

    // ---- report ---------------------------------------------------------
    const double best = totals.empty() ? -1.0 : *std::min_element(totals.begin(), totals.end());
    const int64_t peak_rss = sv::MemoryGovernor::process_peak_rss_kb();
    const double peak_mb = peak_rss > 0 ? static_cast<double>(peak_rss) / 1024.0 : -1.0;

    const double tracked_mb =
        static_cast<double>(sv::MemoryGovernor::global().peak_kb()) / 1024.0;
    const Gate gates[] = {
        // name                       measured              threshold  lower_better
        {"status_ok", st == sv::Status::Ok ? 1.0 : 0.0, 1.0, false},
        {"time_total_ms", best, 4000.0, true},
        {"tracked_peak_mb", tracked_mb, static_cast<double>(args.budget_mb), true},
        {"process_rss_mb", peak_mb, static_cast<double>(args.rss_limit_mb), true},
        {"governor_headroom_mb", static_cast<double>(args.budget_mb) - tracked_mb, 100.0, false},
    };
    bool all_pass = true;
    for (const auto& g : gates) all_pass = all_pass && g.pass();

    if (args.json) {
        printf("{\n");
        printf("  \"backend\": \"%s\",\n", args.backend.c_str());
        printf("  \"image\": \"%dx%d\",\n", img.width, img.height);
        printf("  \"status\": \"%s\",\n", sv::ToString(st));
        printf("  \"time_total_ms\": %.2f,\n", best);
        printf("  \"peak_rss_mb\": %.2f,\n", peak_mb);
        printf("  \"tracked_peak_mb\": %.2f,\n", tracked_mb);
        printf("  \"governor\": \"%s\",\n", sv::MemoryGovernor::global().dump().c_str());
        printf("  \"stages\": [");
        for (int i = 0; i < last.stage_count; ++i) {
            printf("%s{\"name\":\"%s\",\"ms\":%.2f}", i ? "," : "", last.stages[i].name,
                   last.stages[i].ms);
        }
        printf("],\n");
        printf("  \"gate\": \"%s\"\n", all_pass ? "PASS" : "FAIL");
        printf("}\n");
    } else {
        printf("\n=== ShadowVision2 G0 gate report ===\n");
        printf("backend        : %s\n", args.backend.c_str());
        printf("image          : %dx%d  mask bbox:", img.width, img.height);
        int bx0, by0, bx1, by1;
        if (sv::MaskBoundingBox(mask.view(), &bx0, &by0, &bx1, &by1)) {
            printf(" (%d,%d)-(%d,%d)\n", bx0, by0, bx1, by1);
        } else {
            printf(" empty\n");
        }
        printf("status         : %s\n", sv::ToString(st));
        printf("time (best)    : %.1f ms   (repeat=%d)\n", best, args.repeat);
        for (int i = 0; i < last.stage_count; ++i) {
            printf("  - %-20s %8.1f ms\n", last.stages[i].name, last.stages[i].ms);
        }
        printf("governor       : %s\n", sv::MemoryGovernor::global().dump().c_str());
        printf("tracked peak   : %.1f MB\n", tracked_mb);
        printf("peak RSS       : %.1f MB\n", peak_mb);
        printf("npu partition  : %.0f%%\n", inpaint.npu_partition_ratio() * 100.f);
        printf("------------------------------------\n");
        for (const auto& g : gates) {
            printf("  [%s] %-20s %10.2f  (limit %.2f)\n", g.pass() ? "PASS" : "FAIL", g.name,
                   g.measured, g.threshold);
        }
        printf("=== G0: %s ===\n", all_pass ? "PASS" : "FAIL");
    }

    if (!args.out.empty() && st == sv::Status::Ok) {
        sv::SavePPM(args.out, sv::ImageRGB{out.data.data(), out.width, out.height, out.width * 3});
        fprintf(stderr, "wrote %s\n", args.out.c_str());
    }

    return all_pass ? 0 : 1;
}
