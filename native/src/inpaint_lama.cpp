// SPDX-License-Identifier: MIT
#include "sv/inpaint_lama.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "sv/kernels.h"
#include "sv/memory.h"

namespace sv {

namespace {
inline double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}
}  // namespace

bool MaskBoundingBox(const Mask& mask, int* x0, int* y0, int* x1, int* y1) {
    if (!mask.valid()) return false;
    int minx = mask.width, miny = mask.height, maxx = -1, maxy = -1;
    for (int y = 0; y < mask.height; ++y) {
        const uint8_t* row = mask.data + static_cast<size_t>(y) * mask.stride;
        for (int x = 0; x < mask.width; ++x) {
            if (row[x] > 8) {
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }
    if (maxx < 0) return false;
    *x0 = minx;
    *y0 = miny;
    *x1 = maxx + 1;
    *y1 = maxy + 1;
    return true;
}

void ExpandBox(int* x0, int* y0, int* x1, int* y1, int margin, int w, int h) {
    *x0 = std::max(0, *x0 - margin);
    *y0 = std::max(0, *y0 - margin);
    *x1 = std::min(w, *x1 + margin);
    *y1 = std::min(h, *y1 + margin);
}

struct LaMaInpainter::Impl {
    std::unique_ptr<IInpaintBackend> backend;
    std::vector<uint8_t> crop_rgb;
    std::vector<uint8_t> crop_mask;
    std::vector<uint8_t> patch_rgb;
    int work_res = 512;
    bool square = false;  // set for fixed-shape exports (ONNX 512x512, QNN HPT)

    explicit Impl(std::unique_ptr<IInpaintBackend> b) : backend(std::move(b)) {}
};

LaMaInpainter::LaMaInpainter(std::unique_ptr<IInpaintBackend> backend)
    : m_impl(new Impl(std::move(backend))) {}

LaMaInpainter::~LaMaInpainter() = default;

Status LaMaInpainter::load(const BackendOptions& opts) {
    if (!m_impl->backend) return Status::Internal;
    return m_impl->backend->load(opts);
}

void LaMaInpainter::release() {
    if (m_impl->backend) m_impl->backend->release();
    m_impl->crop_rgb.clear();
    m_impl->crop_rgb.shrink_to_fit();
    m_impl->crop_mask.clear();
    m_impl->crop_mask.shrink_to_fit();
    m_impl->patch_rgb.clear();
    m_impl->patch_rgb.shrink_to_fit();
}

void LaMaInpainter::set_square_input(bool square) { m_impl->square = square; }

float LaMaInpainter::npu_partition_ratio() const {
    return m_impl->backend ? m_impl->backend->npu_partition_ratio() : -1.f;
}

Status LaMaInpainter::inpaintFixed(const ImageRGB& image, const Mask& mask, MutableImageRGB out,
                                   const CancelToken& cancel, Stats& stats) {
    if (!image.valid() || !mask.valid() || !out.valid()) return Status::InvalidArgument;
    if (!m_impl->backend || !m_impl->backend->ready()) return Status::ModelNotLoaded;
    if (cancel.cancelled()) return Status::Cancelled;
    return m_impl->backend->run(image, mask, out);
}

Status LaMaInpainter::inpaint(const ImageRGB& image, const Mask& mask, MutableImageRGB out,
                              const CancelToken& cancel, Stats& stats, int work_resolution) {
    if (!image.valid() || !mask.valid() || !out.valid()) return Status::InvalidArgument;
    if (!m_impl->backend || !m_impl->backend->ready()) return Status::ModelNotLoaded;
    if (cancel.cancelled()) return Status::Cancelled;

    const double t_crop_start = now_ms();

    // 1. Crop to mask bbox + margin. This is the single largest speed win in the
    //    whole pipeline: cost scales with area, and the bbox is often 5-20% of
    //    the frame. Never run the UNet/LaMa on the full 12MP image.
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (!MaskBoundingBox(mask, &bx0, &by0, &bx1, &by1)) {
        // Nothing to do: copy through.
        for (int y = 0; y < out.height; ++y) {
            memcpy(out.data + static_cast<size_t>(y) * out.stride,
                   image.data + static_cast<size_t>(y) * image.stride,
                   static_cast<size_t>(out.width) * 3);
        }
        return Status::Ok;
    }
    const int margin = std::max(16, work_resolution / 8);
    ExpandBox(&bx0, &by0, &bx1, &by1, margin, image.width, image.height);
    const int crop_w = bx1 - bx0;
    const int crop_h = by1 - by0;

    // 2. Target network resolution: preserve aspect, long side = work_res, and
    //    round to a multiple of 8 (LaMa downsamples 3 times).
    //
    //    `square` forces W == H == work_res. Needed for fixed-shape exports (the
    //    ONNX contract is 512x512) and for QNN HTP, which does not accept a
    //    dynamic shape at all. Cropping to a square before inference is also
    //    cheaper than padding a wide crop into a square later.
    const int long_side = std::max(crop_w, crop_h);
    const float scale = static_cast<float>(work_resolution) / static_cast<float>(long_side);
    int net_w;
    int net_h;
    if (m_impl->square) {
        net_w = net_h = work_resolution;
    } else {
        net_w = std::max(64, static_cast<int>(crop_w * std::min(1.f, scale)));
        net_h = std::max(64, static_cast<int>(crop_h * std::min(1.f, scale)));
    }
    net_w = (net_w + 7) & ~7;
    net_h = (net_h + 7) & ~7;
    m_impl->work_res = std::max(net_w, net_h);

    // 3. Allocate under the governor. peak here is 3 buffers at net resolution
    //    plus the patch at crop resolution.
    const size_t net_bytes = static_cast<size_t>(net_w) * net_h * 3;
    const size_t crop_bytes = static_cast<size_t>(crop_w) * crop_h * 3;
    auto lease_in = MemoryGovernor::global().acquire("lama.crop_in", net_bytes);
    auto lease_mask = MemoryGovernor::global().acquire("lama.crop_mask",
                                                       static_cast<size_t>(net_w) * net_h);
    auto lease_out = MemoryGovernor::global().acquire("lama.crop_out", net_bytes);
    auto lease_patch = MemoryGovernor::global().acquire("lama.patch", crop_bytes);
    if (!lease_in.live || !lease_mask.live || !lease_out.live || !lease_patch.live) {
        return Status::BudgetExceeded;
    }
    m_impl->crop_rgb.assign(net_bytes, 0);
    m_impl->crop_mask.assign(static_cast<size_t>(net_w) * net_h, 0);
    m_impl->patch_rgb.assign(crop_bytes, 0);

    // 4. Views into the source crop, then resize into the network buffer.
    ImageRGB src_crop{image.data + static_cast<size_t>(by0) * image.stride + static_cast<size_t>(bx0) * 3,
                      crop_w, crop_h, image.stride};
    Mask src_mask{mask.data + static_cast<size_t>(by0) * mask.stride + bx0, crop_w, crop_h,
                  mask.stride};

    MutableImageRGB net_in{m_impl->crop_rgb.data(), net_w, net_h, net_w * 3};
    MutableImageRGB net_out{m_impl->crop_rgb.data(), net_w, net_h, net_w * 3};

    ResizeBilinearRGB(src_crop, net_w, net_h, net_in);
    ResizeBilinearMask(src_mask, net_w, net_h, m_impl->crop_mask.data(), net_w);

    // Dilate + feather at network resolution: cheaper and equivalent.
    DilateMask(m_impl->crop_mask.data(), net_w, net_h, net_w, 3);
    // The network needs a FEATHERED mask so its own boundary is soft; the blend
    // needs the DILATED mask so nothing of the object survives at the edge. Two
    // masks, both derived from the same dilation, kept in separate buffers.
    std::vector<uint8_t> dilated_mask(static_cast<size_t>(net_w) * net_h);
    std::memcpy(dilated_mask.data(), m_impl->crop_mask.data(), dilated_mask.size());
    FeatherMask(m_impl->crop_mask.data(), net_w, net_h, net_w, 2);

    // Crop-resolution version of the dilated mask for blending.
    std::vector<uint8_t> blend_mask(static_cast<size_t>(crop_w) * crop_h, 0);
    for (int y = 0; y < crop_h; ++y) {
        const int sy = std::min(net_h - 1, y * net_h / crop_h);
        for (int x = 0; x < crop_w; ++x) {
            const int sx = std::min(net_w - 1, x * net_w / crop_w);
            blend_mask[static_cast<size_t>(y) * crop_w + x] =
                dilated_mask[static_cast<size_t>(sy) * net_w + sx];
        }
    }

    stats.add("lama.crop_prepare", now_ms() - t_crop_start);
    if (cancel.cancelled()) return Status::Cancelled;

    // 5. Run the network (this is where the inference engine is called).
    const double t_run = now_ms();
    Mask net_mask{m_impl->crop_mask.data(), net_w, net_h, net_w};
    std::vector<uint8_t> net_result(net_bytes, 0);
    MutableImageRGB net_result_view{net_result.data(), net_w, net_h, net_w * 3};
    const Status st = m_impl->backend->run(ImageRGB{m_impl->crop_rgb.data(), net_w, net_h, net_w * 3},
                                          net_mask, net_result_view);
    stats.add("lama.run", now_ms() - t_run);
    if (st != Status::Ok) return st;
    (void)net_out;
    if (cancel.cancelled()) return Status::Cancelled;

    // 6. Upsample the patch back to crop resolution, blend, write out.
    const double t_blend = now_ms();
    MutableImageRGB patch_view{m_impl->patch_rgb.data(), crop_w, crop_h, crop_w * 3};
    ResizeBilinearRGB(ImageRGB{net_result.data(), net_w, net_h, net_w * 3}, crop_w, crop_h,
                      patch_view);

    // Copy the original into out first, then blend only the crop region.
    for (int y = 0; y < out.height; ++y) {
        memcpy(out.data + static_cast<size_t>(y) * out.stride,
               image.data + static_cast<size_t>(y) * image.stride,
               static_cast<size_t>(out.width) * 3);
    }
    MutableImageRGB out_crop{out.data + static_cast<size_t>(by0) * out.stride + static_cast<size_t>(bx0) * 3,
                             crop_w, crop_h, out.stride};
    // Blend with the DILATED mask, not the raw stroke. The dilation exists because
    // a mask that stops exactly on the object edge always leaves a one-pixel halo
    // of it behind; but blending with the undilated mask re-introduces exactly
    // that halo — the red rims left around the post in the end-to-end run. The
    // dilation and the blend must use the same mask or the dilation is decoration.
    BlendMaskedRGB(out_crop, sv::ImageRGB{patch_view.data, crop_w, crop_h, crop_w * 3},
                   Mask{blend_mask.data(), crop_w, crop_h, crop_w}, nullptr, 0);
    stats.add("lama.blend", now_ms() - t_blend);
    stats.peak_rss_kb = MemoryGovernor::process_peak_rss_kb();
    return Status::Ok;
}

}  // namespace sv
