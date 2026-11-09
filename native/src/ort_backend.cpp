// SPDX-License-Identifier: MIT
// ONNX Runtime backend for LaMa (dilated) inpainting.
//
// Model contract (export it yourself from the LaMa weights — see tools/export_lama_onnx.py):
//   image : float32 NCHW [1,3,H,W], RGB in 0..1
//   mask  : float32 NCHW [1,1,H,W], 1 = hole        (LaMa expects the *image*
//           already multiplied by (1-mask) in some exports; we support both and
//           decide from the session's input count, not from a guess)
//   output: float32 NCHW [1,3,H,W], 0..1
//
// Shape policy: one session per (W,H), cache capped at 2. Every extra session
// maps the whole weight file again, which is exactly the RSS we are guarding.
#include "sv/ort_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "sv/memory.h"

namespace sv {
namespace {

constexpr const char* kImageName = "image";
constexpr const char* kMaskName = "mask";
constexpr const char* kOutputName = "output";

struct CachedSession {
    Ort::Session session{nullptr};
    int w = 0;
    int h = 0;
    size_t input_count = 0;
    std::vector<int64_t> input_shape;  // [N,C,H,W]; -1 means dynamic
    MemoryGovernor::Lease lease;  // accounts for the weights this session maps
    bool valid() const { return session != nullptr; }
};

class OrtBackend final : public IInpaintBackend {
public:
    OrtBackend() : m_env(ORT_LOGGING_LEVEL_WARNING, "shadowvision") {}
    ~OrtBackend() override { release(); }

    Status load(const BackendOptions& opts) override {
        std::lock_guard<std::mutex> lock(m_mu);
        release_locked();
        m_opts = opts;
        if (opts.model_path.empty()) return Status::InvalidArgument;
        m_model_bytes = FileSize(opts.model_path);
        if (m_model_bytes == 0) return Status::ModelNotLoaded;

        try {
            m_session_opts = Ort::SessionOptions{};
            m_session_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            // Low thread count on purpose: mobile inference is bandwidth-bound
            // and extra threads buy heat (throttling) faster than throughput.
            m_session_opts.SetIntraOpNumThreads(opts.intra_op_threads);
            m_session_opts.SetInterOpNumThreads(1);
            m_session_opts.AddConfigEntry("session.intra_op.allow_spinning", "0");

            if (opts.use_npu) {
                // The QNN provider is only present in builds linked against the
                // QNN AAR. If it is absent ORT throws here and we degrade to CPU,
                // which is the contract: never crash over an accelerator.
                std::map<std::string, std::string> qnn;
                qnn["backend_type"] = "htp";
                qnn["htp_performance_mode"] = "burst";
                if (!opts.qnn_context_bin.empty()) {
                    qnn["ep.context_file_path"] = opts.qnn_context_bin;
                }
                m_session_opts.AppendExecutionProvider("QNN", qnn);
            }
            m_ready = true;
            return Status::Ok;
        } catch (const std::exception& e) {
            m_last_error = e.what();
            return opts.use_npu ? degrade_to_cpu() : Status::Internal;
        }
    }

    Status run(const ImageRGB& image, const Mask& mask, MutableImageRGB out) override {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_ready) return Status::ModelNotLoaded;
        if (!image.valid() || !mask.valid() || !out.valid()) return Status::InvalidArgument;

        CachedSession* entry = session_for(image.width, image.height);
        if (!entry) return Status::BudgetExceeded;

        const size_t px = static_cast<size_t>(image.width) * image.height;
        auto lease = MemoryGovernor::global().acquire("ort.io", px * 3 * sizeof(float) * 2);
        if (!lease.live) return Status::BudgetExceeded;

        std::vector<float> img_planar(px * 3);
        std::vector<float> mask_planar(px);
        to_planar_rgb(image, img_planar.data());
        to_planar_mask(mask, mask_planar.data());

        try {
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const int64_t shape[4] = {1, 3, image.height, image.width};
            const int64_t mshape[4] = {1, 1, image.height, image.width};

            // Two-input exports get the mask explicitly; single-input exports
            // (mask pre-multiplied by the caller) get the image only.
            std::vector<Ort::Value> inputs;
            std::vector<const char*> names;
            inputs.push_back(Ort::Value::CreateTensor<float>(mem, img_planar.data(), img_planar.size(),
                                                            shape, 4));
            names.push_back(kImageName);
            if (entry->input_count >= 2) {
                inputs.push_back(Ort::Value::CreateTensor<float>(
                    mem, mask_planar.data(), mask_planar.size(), mshape, 4));
                names.push_back(kMaskName);
            } else {
                for (size_t i = 0; i < px; ++i) {
                    const float keep = 1.f - mask_planar[i];
                    img_planar[i] *= keep;
                    img_planar[px + i] *= keep;
                    img_planar[px * 2 + i] *= keep;
                }
            }

            const char* out_names[] = {kOutputName};
            auto outputs = entry->session.Run(Ort::RunOptions{nullptr}, names.data(), inputs.data(),
                                              inputs.size(), out_names, 1);
            if (outputs.empty() || !outputs[0].IsTensor()) return Status::Internal;
            from_planar_rgb(outputs[0].GetTensorData<float>(), out);
            return Status::Ok;
        } catch (const std::exception& e) {
            m_last_error = e.what();
            return Status::Internal;
        }
    }

    const char* name() const override { return "onnxruntime"; }
    bool ready() const override { return m_ready; }
    float npu_partition_ratio() const override { return m_npu_ratio; }

    void release() override {
        std::lock_guard<std::mutex> lock(m_mu);
        release_locked();
    }

    std::string diagnostics() const override { return m_last_error; }

private:
    static size_t FileSize(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return 0;
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fclose(f);
        return sz > 0 ? static_cast<size_t>(sz) : 0;
    }

    Status degrade_to_cpu() {
        m_last_error += " | QNN unavailable, fell back to CPU";
        try {
            m_session_opts = Ort::SessionOptions{};
            m_session_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            m_session_opts.SetIntraOpNumThreads(m_opts.intra_op_threads);
            m_session_opts.SetInterOpNumThreads(1);
            m_session_opts.AddConfigEntry("session.intra_op.allow_spinning", "0");
            m_ready = true;
            m_npu_ratio = 0.f;  // honest: the NPU is not in play
            return Status::Ok;
        } catch (const std::exception& e) {
            m_last_error = e.what();
            return Status::BackendUnavailable;
        }
    }

    void release_locked() {
        m_sessions.clear();
        m_ready = false;
        m_npu_ratio = -1.f;
    }

    CachedSession* session_for(int w, int h) {
        const int64_t key = (static_cast<int64_t>(w) << 32) | static_cast<uint32_t>(h);
        auto it = m_sessions.find(key);
        if (it != m_sessions.end()) return &it->second;
        if (m_sessions.size() >= 2) m_sessions.erase(m_sessions.begin());

        CachedSession entry;
        entry.w = w;
        entry.h = h;
        entry.lease = MemoryGovernor::global().acquire("ort.weights", m_model_bytes);
        if (!entry.lease.live) return nullptr;
        try {
            entry.session = Ort::Session(m_env, m_opts.model_path.c_str(), m_session_opts);
            entry.input_count = entry.session.GetInputCount();
            Ort::AllocatorWithDefaultOptions alloc;
            if (entry.input_count > 0) {
                auto info = entry.session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
                entry.input_shape = info.GetShape();
            }
        } catch (const std::exception& e) {
            m_last_error = e.what();
            return nullptr;
        }
        // Node-assignment ratio: left unknown on purpose until the QNN optrace
        // parser lands. A fabricated 100% is worse than an honest -1.
        m_npu_ratio = m_opts.use_npu ? -1.f : 0.f;
        if (!entry.input_shape.empty()) {
            fprintf(stderr, "[sv][ort] session %dx%d, model expects %lldx%lld\n", w, h,
                    static_cast<long long>(entry.input_shape.size() == 4 ? entry.input_shape[2] : -1),
                    static_cast<long long>(entry.input_shape.size() == 4 ? entry.input_shape[3] : -1));
        }
        auto res = m_sessions.emplace(key, std::move(entry));
        return &res.first->second;
    }

    static void to_planar_rgb(const ImageRGB& img, float* dst) {
        const size_t px = static_cast<size_t>(img.width) * img.height;
        for (int y = 0; y < img.height; ++y) {
            const uint8_t* row = img.data + static_cast<size_t>(y) * img.stride;
            for (int x = 0; x < img.width; ++x) {
                const size_t i = static_cast<size_t>(y) * img.width + x;
                dst[i] = row[x * 3 + 0] * (1.f / 255.f);
                dst[px + i] = row[x * 3 + 1] * (1.f / 255.f);
                dst[px * 2 + i] = row[x * 3 + 2] * (1.f / 255.f);
            }
        }
    }

    static void to_planar_mask(const Mask& m, float* dst) {
        for (int y = 0; y < m.height; ++y) {
            const uint8_t* row = m.data + static_cast<size_t>(y) * m.stride;
            float* d = dst + static_cast<size_t>(y) * m.width;
            for (int x = 0; x < m.width; ++x) d[x] = row[x] * (1.f / 255.f);
        }
    }

    static void from_planar_rgb(const float* src, MutableImageRGB& out) {
        const size_t px = static_cast<size_t>(out.width) * out.height;
        for (int y = 0; y < out.height; ++y) {
            uint8_t* row = out.data + static_cast<size_t>(y) * out.stride;
            for (int x = 0; x < out.width; ++x) {
                const size_t i = static_cast<size_t>(y) * out.width + x;
                for (int c = 0; c < 3; ++c) {
                    const float v = std::clamp(src[px * c + i], 0.f, 1.f) * 255.f;
                    row[x * 3 + c] = static_cast<uint8_t>(v + 0.5f);
                }
            }
        }
    }

    Ort::Env m_env;
    Ort::SessionOptions m_session_opts{nullptr};
    BackendOptions m_opts;
    std::map<int64_t, CachedSession> m_sessions;
    std::mutex m_mu;
    bool m_ready = false;
    size_t m_model_bytes = 0;
    float m_npu_ratio = -1.f;
    std::string m_last_error;
};

}  // namespace

std::unique_ptr<IInpaintBackend> CreateOrtBackend() {
    return std::unique_ptr<IInpaintBackend>(new OrtBackend());
}

}  // namespace sv
