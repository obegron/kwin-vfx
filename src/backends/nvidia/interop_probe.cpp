// SPDX-License-Identifier: GPL-2.0-or-later
// Independent ownership/format/synchronization gate. No VFX or KWin dependency.
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cuda_runtime_api.h>
#include <cuda_gl_interop.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static void check(cudaError_t error, const char *stage)
{
    if (error != cudaSuccess) throw std::runtime_error(std::string(stage) + ": " + cudaGetErrorName(error) + ": " + cudaGetErrorString(error));
}

class Probe : protected QOpenGLExtraFunctions
{
public:
    QJsonObject device;
    explicit Probe(bool gles)
    {
        QSurfaceFormat format;
        format.setRenderableType(gles ? QSurfaceFormat::OpenGLES : QSurfaceFormat::OpenGL);
        format.setVersion(3, gles ? 2 : 3);
        if (!gles) format.setProfile(QSurfaceFormat::CoreProfile);
        context.setFormat(format);
        require(context.create(), "Cannot create GL context");
        surface.setFormat(context.format());
        surface.create();
        require(context.makeCurrent(&surface), "Cannot make GL context current");
        initializeOpenGLFunctions();
        device = {{"gl_vendor", reinterpret_cast<const char *>(glGetString(GL_VENDOR))},
            {"gl_renderer", reinterpret_cast<const char *>(glGetString(GL_RENDERER))},
            {"gl_version", reinterpret_cast<const char *>(glGetString(GL_VERSION))},
            {"gles", context.isOpenGLES()}};
    }

    void matchDevice()
    {
        unsigned count = 0;
        int devices[8] = {};
        check(cudaGLGetDevices(&count, devices, 8, cudaGLDeviceListAll), "cudaGLGetDevices");
        require(count == 1, "Expected one CUDA GPU associated with this GL context");
        check(cudaSetDevice(devices[0]), "cudaSetDevice");
        cudaDeviceProp properties{};
        check(cudaGetDeviceProperties(&properties, devices[0]), "cudaGetDeviceProperties");
        int runtime = 0, driver = 0;
        check(cudaRuntimeGetVersion(&runtime), "cudaRuntimeGetVersion");
        check(cudaDriverGetVersion(&driver), "cudaDriverGetVersion");
        device.insert("cuda_device", properties.name);
        device.insert("cuda_ordinal", devices[0]);
        device.insert("compute_capability", QString::number(properties.major) + "." + QString::number(properties.minor));
        device.insert("runtime_version", runtime);
        device.insert("driver_api_version", driver);
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreate");
    }

    ~Probe()
    {
        release();
        if (stream) (void)cudaStreamDestroy(stream);
    }

    QJsonObject run(QSize size)
    {
        glGenTextures(2, textures.data());
        glGenFramebuffers(2, framebuffers.data());
        for (size_t i = 0; i < textures.size(); ++i) {
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
            require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Incomplete FBO");
        }
        check(cudaGraphicsGLRegisterImage(&resources[0], textures[0], GL_TEXTURE_2D, cudaGraphicsRegisterFlagsReadOnly), "register input RGBA8 texture");
        check(cudaGraphicsGLRegisterImage(&resources[1], textures[1], GL_TEXTURE_2D, cudaGraphicsRegisterFlagsWriteDiscard), "register output RGBA8 texture");
        check(cudaMallocPitch(&linear, &pitch, size_t(size.width()) * 4, size.height()), "allocate pitch-linear GPU image");
        check(cudaEventCreate(&beginEvent), "cudaEventCreate begin");
        check(cudaEventCreate(&endEvent), "cudaEventCreate end");
        QJsonArray samples;
        constexpr int cycles = 60;
        for (int frame = 0; frame < cycles; ++frame) {
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[0]);
            glViewport(0, 0, size.width(), size.height());
            glDisable(GL_BLEND);
            glDisable(GL_DITHER);
            glEnable(GL_SCISSOR_TEST);
            const auto colors = pattern(frame);
            for (int q = 0; q < 4; ++q) {
                glScissor((q & 1) * size.width() / 2, (q >> 1) * size.height() / 2, size.width() / 2, size.height() / 2);
                const auto &c = colors[size_t(q)];
                glClearColor(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            glFlush(); // Submit GL production; map provides the GL -> CUDA ordering.
            auto started = Clock::now();
            auto call = Clock::now();
            check(cudaGraphicsMapResources(2, resources.data(), stream), "map GL textures");
            mapped = true;
            const double mapMs = elapsed(call);
            cudaArray_t input = nullptr, output = nullptr;
            check(cudaGraphicsSubResourceGetMappedArray(&input, resources[0], 0, 0), "get input array");
            check(cudaGraphicsSubResourceGetMappedArray(&output, resources[1], 0, 0), "get output array");
            check(cudaEventRecord(beginEvent, stream), "record CUDA begin");
            check(cudaMemcpy2DFromArrayAsync(linear, pitch, input, 0, 0, size_t(size.width()) * 4, size.height(), cudaMemcpyDeviceToDevice, stream), "array to linear GPU copy");
            // A future VFX effect consumes/writes pitch-linear GPU buffers here.
            check(cudaMemcpy2DToArrayAsync(output, 0, 0, linear, pitch, size_t(size.width()) * 4, size.height(), cudaMemcpyDeviceToDevice, stream), "linear to array GPU copy");
            check(cudaEventRecord(endEvent, stream), "record CUDA end");
            call = Clock::now();
            check(cudaGraphicsUnmapResources(2, resources.data(), stream), "unmap GL textures");
            mapped = false;
            const double unmapMs = elapsed(call);
            cudaError_t ready;
            while ((ready = cudaStreamQuery(stream)) == cudaErrorNotReady) {
                require(elapsed(started) < 5000, "GPU completion timeout");
                QThread::usleep(50);
            }
            check(ready, "CUDA stream completion");
            const double readyMs = elapsed(started);
            float copyMs = 0;
            check(cudaEventElapsedTime(&copyMs, beginEvent, endEvent), "CUDA copy timing");
            samples.append(QJsonObject{{"map_cpu_ms", mapMs}, {"unmap_cpu_ms", unmapMs},
                {"cuda_copy_ms", copyMs}, {"ready_wall_ms", readyMs}});
            if (frame == 0 || frame == cycles - 1) {
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[1]);
                std::vector<unsigned char> bytes(size_t(size.width()) * size.height() * 4);
                glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
                for (int y = 0; y < size.height(); ++y) {
                    for (int x = 0; x < size.width(); ++x) {
                        const auto &expected = colors[size_t((x >= size.width() / 2) + 2 * (y >= size.height() / 2))];
                        for (int c = 0; c < 4; ++c) {
                            require(bytes[(size_t(y) * size.width() + x) * 4 + c] == expected[size_t(c)], "RGBA channel/alpha/orientation mismatch");
                        }
                    }
                }
            }
            require(glGetError() == GL_NO_ERROR, "GL error after CUDA interop");
        }
        QJsonObject result{{"size", QJsonArray{size.width(), size.height()}}, {"pitch", double(pitch)},
            {"cycles", cycles}, {"byte_exact", true}, {"samples", samples}};
        release();
        return result;
    }

private:
    static std::array<std::array<unsigned char, 4>, 4> pattern(int frame)
    {
        const auto d = static_cast<unsigned char>(frame % 32);
        return {{{static_cast<unsigned char>(32 + d), 0, 0, 64}, {0, static_cast<unsigned char>(50 + d), 0, 128},
            {0, 0, static_cast<unsigned char>(80 + d), 192}, {static_cast<unsigned char>(120 + d), 80, 30, 255}}};
    }
    void release()
    {
        if (mapped) { (void)cudaGraphicsUnmapResources(2, resources.data(), stream); mapped = false; }
        if (stream) (void)cudaStreamSynchronize(stream); // Cleanup only in this standalone probe.
        for (auto &resource : resources) { if (resource) (void)cudaGraphicsUnregisterResource(resource); resource = nullptr; }
        if (linear) { (void)cudaFree(linear); linear = nullptr; }
        if (beginEvent) { (void)cudaEventDestroy(beginEvent); beginEvent = nullptr; }
        if (endEvent) { (void)cudaEventDestroy(endEvent); endEvent = nullptr; }
        if (framebuffers[0]) glDeleteFramebuffers(2, framebuffers.data());
        if (textures[0]) glDeleteTextures(2, textures.data());
        textures = {}; framebuffers = {};
    }
    QOpenGLContext context;
    QOffscreenSurface surface;
    cudaStream_t stream = nullptr;
    cudaEvent_t beginEvent = nullptr, endEvent = nullptr;
    std::array<GLuint, 2> textures{}, framebuffers{};
    std::array<cudaGraphicsResource_t, 2> resources{};
    void *linear = nullptr;
    size_t pitch = 0;
    bool mapped = false;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QJsonObject result;
    int status = 0;
    try {
        Probe probe(app.arguments().contains(QStringLiteral("--gles")));
        result.insert("device", probe.device);
        probe.matchDevice();
        result.insert("device", probe.device);
        QJsonArray cases;
        for (QSize size : {QSize(1280, 720), QSize(1920, 1080), QSize(3840, 2160), QSize(640, 480)}) cases.append(probe.run(size));
        result.insert("cases", cases);
        result.insert("passed", true);
        result.insert("ai_inference", false);
        result.insert("scope", "Owned RGBA8 GL textures -> CUDA arrays -> pitch-linear device buffer -> GL; two device copies; not zero-copy; no VFX call");
    } catch (const std::exception &error) {
        result.insert("passed", false);
        result.insert("error", error.what());
        status = 1;
    }
    const QByteArray json = QJsonDocument(result).toJson();
    const int output = app.arguments().indexOf(QStringLiteral("--output"));
    if (output >= 0 && output + 1 < app.arguments().size()) {
        QFile file(app.arguments()[output + 1]);
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) return 2;
    } else std::cout << json.constData();
    return status;
}
