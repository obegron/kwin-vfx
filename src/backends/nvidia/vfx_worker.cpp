// SPDX-License-Identifier: GPL-2.0-or-later
#include "vfx_worker.h"
#include "vfx_runtime.h"
#include <cudaGL.h>
#include <QThread>
#include <array>
#include <chrono>

namespace VfxNvidia {
namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now()-t).count(); }
void check(CUresult status, const char *stage)
{
    if (status == CUDA_SUCCESS) return;
    const char *name = "unknown CUDA error";
    cuGetErrorName(status, &name);
    throw std::runtime_error(std::string(stage) + ": " + name);
}
// This object is created, used and destroyed exclusively on the worker thread.
struct Resources {
    Runtime sdk;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext egl = EGL_NO_CONTEXT;
    CUdevice device = 0;
    CUcontext cuda = nullptr;
    bool pushed = false, mapped = false;
    CUstream stream = nullptr;
    CUevent done = nullptr;
    CUevent sdkBegin = nullptr, sdkEnd = nullptr;
    std::array<CUgraphicsResource, 3> graphics{};
    std::array<CUgraphicsResource, 2> mappedGraphics{};
    CUdeviceptr input = 0, output = 0;
    CUdeviceptr intermediate = 0, displayOutput = 0;
    size_t inputPitch = 0, outputPitch = 0;
    size_t intermediatePitch = 0, displayPitch = 0;
    NvCVImage *src = nullptr, *dst = nullptr;
    NvCVImage *middle = nullptr, *displayImage = nullptr;
    Effect effect = nullptr;
    Effect hdrEffect = nullptr;
    ~Resources()
    {
        if (qEnvironmentVariableIsSet("KWIN_VFX_TEST_NVIDIA_TEARDOWN_DELAY")) QThread::msleep(250);
        if (mapped) cuGraphicsUnmapResources(2, mappedGraphics.data(), stream);
        if (cuda) cuStreamSynchronize(stream); // Worker teardown only; nullptr denotes default stream.
        if (hdrEffect) sdk.destroy(hdrEffect);
        if (effect) sdk.destroy(effect);
        if (src) sdk.imageDestroy(src);
        if (dst) sdk.imageDestroy(dst);
        if (middle) sdk.imageDestroy(middle);
        if (displayImage) sdk.imageDestroy(displayImage);
        for (auto resource : graphics) if (resource) cuGraphicsUnregisterResource(resource);
        if (input) cuMemFree(input);
        if (output) cuMemFree(output);
        if (intermediate) cuMemFree(intermediate);
        if (displayOutput) cuMemFree(displayOutput);
        if (done) cuEventDestroy(done);
        if (sdkBegin) cuEventDestroy(sdkBegin);
        if (sdkEnd) cuEventDestroy(sdkEnd);
        if (pushed) { CUcontext previous; cuCtxPopCurrent(&previous); }
        if (cuda) cuDevicePrimaryCtxRelease(device);
        if (egl != EGL_NO_CONTEXT) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, egl);
        }
    }
};
}

Worker::Worker(QString root, QSize size, GLuint input, GLuint output, unsigned mode, float strength,
    GLuint secondOutput, bool trueHdr, bool restoration, unsigned peakNits)
    : m_root(std::move(root)), m_size(size), m_input(input), m_output(output), m_secondOutput(secondOutput),
      m_mode(mode), m_strength(strength), m_trueHdr(trueHdr), m_restoration(restoration), m_peakNits(peakNits)
{
    m_gpuTiming = qEnvironmentVariableIsSet("KWIN_VFX_PROFILE");
    m_display = eglGetCurrentDisplay();
    m_share = eglGetCurrentContext();
    require(m_display != EGL_NO_DISPLAY && m_share != EGL_NO_CONTEXT, "NVIDIA worker requires current EGL context");
    EGLint api = 0, id = 0;
    require(eglQueryContext(m_display, m_share, EGL_CONTEXT_CLIENT_TYPE, &api), "Cannot query EGL API");
    m_api = EGLenum(api);
    require(eglQueryContext(m_display, m_share, EGL_CONTEXT_CLIENT_VERSION, &m_major), "Cannot query EGL version");
    // KWin/Qt GLES and desktop GL both support the EGL 1.5 minor-version query.
    if (!eglQueryContext(m_display, m_share, EGL_CONTEXT_MINOR_VERSION, &m_minor)) m_minor = 0;
    require(eglQueryContext(m_display, m_share, EGL_CONFIG_ID, &id), "Cannot query EGL config");
    if (id) {
        const EGLint attributes[] = {EGL_CONFIG_ID, id, EGL_NONE};
        EGLint count = 0;
        require(eglChooseConfig(m_display, attributes, &m_config, 1, &count) && count == 1, "Cannot find shared EGL config");
    }
    // EGL sharing requires matching reset-notification behavior. KWin creates
    // robust contexts (including NVIDIA's reset-on-purge option), unlike the
    // default Qt context used by the independent probe.
    GLboolean robust = GL_FALSE;
    GLint reset = GL_NO_RESET_NOTIFICATION;
    glGetBooleanv(GL_CONTEXT_ROBUST_ACCESS, &robust);
    glGetIntegerv(GL_RESET_NOTIFICATION_STRATEGY, &reset);
    m_robust = robust;
    m_loseOnReset = reset == GL_LOSE_CONTEXT_ON_RESET;
    if (epoxy_has_egl_extension(m_display, "EGL_NV_robustness_video_memory_purge")) {
        EGLint purge = EGL_FALSE;
        if (eglQueryContext(m_display, m_share, EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV, &purge)) m_resetOnPurge = purge;
    }
    require(!size.isEmpty() && size.width() <= 3840 && size.height() <= 2160, "NVIDIA slot dimensions exceed audited range");
    require(mode == 0 || (mode >= 8 && mode <= 15), "Worker requires a same-size denoise/deblur or identity mode");
    require(m_restoration || m_trueHdr, "Worker pipeline is empty");
    require(!m_trueHdr || (peakNits >= 400 && peakNits <= 2000), "TrueHDR peak outside SDK range");
    m_thread = std::thread([this] { run(); });
}

Worker::~Worker()
{
    requestStop();
    if (m_thread.joinable()) m_thread.join(); // Explicit destruction only, never per frame.
    if (m_fence) glDeleteSync(m_fence); // Caller keeps its sharing context current.
}

void Worker::requestStop()
{
    m_stop.store(true, std::memory_order_release);
    auto state = m_state.load(std::memory_order_acquire);
    while (state != State::Failed && state != State::Finished && state != State::Stopping
        && !m_state.compare_exchange_weak(state, State::Stopping, std::memory_order_acq_rel)) {}
    m_state.notify_all();
}

void Worker::submit(GLsync fence, uint64_t sequence, unsigned outputIndex)
{
    require(accessible() && fence, "Invalid worker slot submission");
    require(outputIndex <= (m_secondOutput ? 1u : 0u), "Invalid worker output index");
    m_fence = fence;
    m_sequence = sequence;
    m_outputIndex = outputIndex;
    m_state.store(State::Queued, std::memory_order_release);
    m_state.notify_one();
}

void Worker::run()
{
    try {
        Resources r;
        r.display = m_display;
        require(eglBindAPI(m_api), "Worker cannot bind EGL API");
        std::vector<EGLint> attributes{EGL_CONTEXT_MAJOR_VERSION, m_major, EGL_CONTEXT_MINOR_VERSION, m_minor};
        if (m_robust || m_loseOnReset) {
            attributes.insert(attributes.end(), {EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT, m_robust ? EGL_TRUE : EGL_FALSE,
                m_api == EGL_OPENGL_ES_API ? EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT : EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR,
                m_loseOnReset ? EGL_LOSE_CONTEXT_ON_RESET_EXT : EGL_NO_RESET_NOTIFICATION_EXT});
        }
        if (m_resetOnPurge) attributes.insert(attributes.end(), {EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV, EGL_TRUE});
        attributes.push_back(EGL_NONE);
        r.egl = eglCreateContext(m_display, m_config, m_share, attributes.data());
        if (r.egl == EGL_NO_CONTEXT) throw std::runtime_error("Cannot create shared worker EGL context: " + std::to_string(eglGetError()));
        require(eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, r.egl), "Cannot activate shared worker EGL context");
        check(cuInit(0), "initialize CUDA");
        unsigned count = 0;
        check(cuGLGetDevices(&count, &r.device, 1, CU_GL_DEVICE_LIST_ALL), "match GL/CUDA device");
        require(count == 1, "Worker requires one matching CUDA device");
        check(cuDevicePrimaryCtxRetain(&r.cuda, r.device), "retain CUDA primary context");
        check(cuCtxPushCurrent(r.cuda), "activate CUDA context");
        r.pushed = true;
        // Stream 0 is the verified SDK ordering path. It is used only here on
        // the worker; no CUDA synchronization runs on the rendering thread.
        check(cuEventCreate(&r.done, CU_EVENT_DISABLE_TIMING), "create worker completion event");
        if (m_gpuTiming) {
            check(cuEventCreate(&r.sdkBegin, CU_EVENT_DEFAULT), "create SDK timing begin");
            check(cuEventCreate(&r.sdkEnd, CU_EVENT_DEFAULT), "create SDK timing end");
        }
        r.sdk.open(m_root, m_trueHdr);
        check(cuGraphicsGLRegisterImage(&r.graphics[0], m_input, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY), "register worker input");
        check(cuGraphicsGLRegisterImage(&r.graphics[1], m_output, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD), "register worker output");
        if (m_secondOutput) check(cuGraphicsGLRegisterImage(&r.graphics[2], m_secondOutput, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD), "register second worker output");
        check(cuMemAllocPitch(&r.input, &r.inputPitch, size_t(m_size.width())*4, m_size.height(), 4), "allocate worker input");
        check(cuMemAllocPitch(&r.output, &r.outputPitch, size_t(m_size.width())*4, m_size.height(), 4), "allocate worker output");
        r.sdk.wrap(&r.src, m_size, r.input, r.inputPitch);
        // RGB10A2=13 / P32=11 are audited against this wheel's exported
        // conversion templates and byte-exact primary-color transfer probe.
        r.sdk.wrap(&r.dst, m_size, r.output, r.outputPitch, m_trueHdr ? 13 : 6, m_trueHdr ? 11 : 1);
        if (m_trueHdr) {
            check(cuMemAllocPitch(&r.displayOutput, &r.displayPitch, size_t(m_size.width())*8, m_size.height(), 8), "allocate HDR float output");
            r.sdk.wrap(&r.displayImage, m_size, r.displayOutput, r.displayPitch, 6, 4); // RGBA F16
            if (m_restoration) {
                check(cuMemAllocPitch(&r.intermediate, &r.intermediatePitch, size_t(m_size.width())*4, m_size.height(), 4), "allocate restoration intermediate");
                r.sdk.wrap(&r.middle, m_size, r.intermediate, r.intermediatePitch);
            }
        }
        if (m_restoration) {
            r.sdk.check(r.sdk.create("VideoSuperRes", &r.effect), "create worker restoration");
            r.sdk.check(r.sdk.setImage(r.effect, "SrcImage0", r.src), "set worker source");
            r.sdk.check(r.sdk.setImage(r.effect, "DstImage0", m_trueHdr ? r.middle : r.dst), "set worker destination");
            r.sdk.check(r.sdk.setStream(r.effect, "CudaStream", r.stream), "set worker stream");
            r.sdk.check(r.sdk.setU32(r.effect, "QualityLevel", m_mode), "set worker mode");
            r.sdk.check(r.sdk.setF32(r.effect, "Strength", m_strength), "set worker strength");
            r.sdk.check(r.sdk.load(r.effect), "load worker restoration");
        }
        if (m_trueHdr) {
            r.sdk.check(r.sdk.create("TrueHDR", &r.hdrEffect), "create TrueHDR");
            r.sdk.check(r.sdk.setImage(r.hdrEffect, "SrcImage0", m_restoration ? r.middle : r.src), "set TrueHDR source");
            r.sdk.check(r.sdk.setImage(r.hdrEffect, "DstImage0", r.dst), "set TrueHDR destination");
            r.sdk.check(r.sdk.setStream(r.hdrEffect, "CudaStream", r.stream), "set TrueHDR stream");
            for (const auto &[name, value] : std::array<std::pair<const char *, unsigned>, 5>{{
                     {"Contrast", 100}, {"Saturation", 100}, {"MiddleGray", 50}, {"Luminance", m_peakNits}, {"DebandingOff", 0}}})
                r.sdk.check(r.sdk.setU32(r.hdrEffect, name, value), "set TrueHDR parameter");
            r.sdk.check(r.sdk.load(r.hdrEffect), "load TrueHDR");
        }
        if (!m_stop.load()) m_state.store(State::Idle, std::memory_order_release);
        while (!m_stop.load(std::memory_order_acquire)) {
            auto state = m_state.load(std::memory_order_acquire);
            if (state != State::Queued) {
                if (state == State::Stopping || m_stop.load(std::memory_order_acquire)) break;
                m_state.wait(state, std::memory_order_acquire);
                continue;
            }
            m_state.store(State::Running, std::memory_order_release);
            const auto started = Clock::now();
            // Fence belongs to the submitting GL context. Never wait for it on
            // the rendering thread. Both contexts share the sync object.
            GLenum ready;
            while ((ready = glClientWaitSync(m_fence, 0, 1000000)) == GL_TIMEOUT_EXPIRED) {
                require(ms(started) < 10000, "Worker GL fence timeout");
            }
            require(ready == GL_ALREADY_SIGNALED || ready == GL_CONDITION_SATISFIED, "Worker GL fence failed");
            glDeleteSync(m_fence);
            m_fence = nullptr;
            r.mappedGraphics = {r.graphics[0], r.graphics[1 + m_outputIndex]};
            check(cuGraphicsMapResources(2, r.mappedGraphics.data(), r.stream), "map worker textures");
            r.mapped = true;
            CUarray input = nullptr, output = nullptr;
            check(cuGraphicsSubResourceGetMappedArray(&input, r.graphics[0], 0, 0), "get input array");
            check(cuGraphicsSubResourceGetMappedArray(&output, r.mappedGraphics[1], 0, 0), "get output array");
            CUDA_MEMCPY2D copy{};
            copy.srcMemoryType = CU_MEMORYTYPE_ARRAY; copy.srcArray = input;
            copy.dstMemoryType = CU_MEMORYTYPE_DEVICE; copy.dstDevice = r.input; copy.dstPitch = r.inputPitch;
            copy.WidthInBytes = size_t(m_size.width())*4; copy.Height = m_size.height();
            check(cuMemcpy2DAsync(&copy, r.stream), "copy worker source to linear GPU image");
            if (qEnvironmentVariableIsSet("KWIN_VFX_TEST_NVIDIA_FAILURE")) throw std::runtime_error("Injected worker failure while mapped");
            if (qEnvironmentVariableIsSet("KWIN_VFX_TEST_NVIDIA_DELAY")) QThread::msleep(100);
            if (m_gpuTiming) check(cuEventRecord(r.sdkBegin, r.stream), "record SDK GPU begin");
            const auto sdkStarted = Clock::now();
            if (r.effect) r.sdk.check(r.sdk.run(r.effect, 0), "run worker restoration");
            if (r.hdrEffect) r.sdk.check(r.sdk.run(r.hdrEffect, 0), "run TrueHDR");
            const double sdkCpuMs = ms(sdkStarted);
            if (m_gpuTiming) check(cuEventRecord(r.sdkEnd, r.stream), "record SDK GPU end");
            if (m_trueHdr) r.sdk.check(r.sdk.imageTransfer(r.dst, r.displayImage, 1.0f, r.stream, nullptr), "convert packed HDR to RGBA16F");
            copy = {};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE; copy.srcDevice = m_trueHdr ? r.displayOutput : r.output;
            copy.srcPitch = m_trueHdr ? r.displayPitch : r.outputPitch;
            copy.dstMemoryType = CU_MEMORYTYPE_ARRAY; copy.dstArray = output;
            copy.WidthInBytes = size_t(m_size.width())*(m_trueHdr ? 8 : 4); copy.Height = m_size.height();
            check(cuMemcpy2DAsync(&copy, r.stream), "copy worker output to GL");
            check(cuGraphicsUnmapResources(2, r.mappedGraphics.data(), r.stream), "unmap worker textures");
            r.mapped = false;
            check(cuEventRecord(r.done, r.stream), "record worker completion");
            CUresult done;
            while ((done = cuEventQuery(r.done)) == CUDA_ERROR_NOT_READY) {
                require(ms(started) < 10000, "Worker CUDA completion timeout");
                QThread::usleep(100);
            }
            check(done, "complete worker stream");
            float sdkGpuMs = -1;
            if (m_gpuTiming) check(cuEventElapsedTime(&sdkGpuMs, r.sdkBegin, r.sdkEnd), "read completed SDK GPU time");
            m_timing = {sdkCpuMs, ms(started), sdkGpuMs};
            if (!m_stop.load()) m_state.store(State::Ready, std::memory_order_release);
        }
        if (m_fence) { glDeleteSync(m_fence); m_fence = nullptr; }
    } catch (const std::exception &error) {
        m_error = QString::fromUtf8(error.what());
        m_state.store(State::Failed, std::memory_order_release);
    }
    // Keep Failed observable until the main thread consumes its diagnostic.
    if (m_state.load() != State::Failed) m_state.store(State::Finished, std::memory_order_release);
    m_finished.store(true, std::memory_order_release);
}
} // namespace VfxNvidia
