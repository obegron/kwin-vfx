// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone Linux wheel-runtime experiment, NEVER compiled into the effect.
// Function signatures: NVIDIA VFX API reference and NvCVImage API Guide.
// NvCVImage remains opaque: the SDK allocates/deletes the descriptor. No guessed
// structure layout, SDK headers, Python, Torch or proprietary source is embedded.
#include "vfx_runtime.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cuda.h>
#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#if !defined(__linux__) || !defined(__x86_64__)
#error This experimental loader audits the Linux x86_64 wheel only
#endif

namespace {
using Clock = std::chrono::steady_clock;
using VfxNvidia::Runtime;
using VfxNvidia::NvCVImage;
using VfxNvidia::Effect;
double elapsed(Clock::time_point started) { return std::chrono::duration<double, std::milli>(Clock::now() - started).count(); }
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void cuda(CUresult status, const char *stage)
{
    if (status == CUDA_SUCCESS) return;
    const char *name = "unknown CUDA error";
    cuGetErrorName(status, &name);
    throw std::runtime_error(std::string(stage) + ": " + name);
}


class Probe
{
public:
    Runtime sdk;
    CUcontext context = nullptr;
    bool contextPushed = false;
    CUstream stream = nullptr;
    CUevent begin = nullptr, end = nullptr;
    CUdeviceptr input = 0, output = 0;
    size_t inputPitch = 0, outputPitch = 0;
    NvCVImage *sourceImage = nullptr, *destinationImage = nullptr;
    Effect effect = nullptr;

    ~Probe()
    {
        if (stream) cuStreamSynchronize(stream); // Isolated harness cleanup only.
        if (effect) sdk.destroy(effect);
        if (sourceImage) sdk.imageDestroy(sourceImage);
        if (destinationImage) sdk.imageDestroy(destinationImage);
        if (input) cuMemFree(input);
        if (output) cuMemFree(output);
        if (begin) cuEventDestroy(begin);
        if (end) cuEventDestroy(end);
        if (stream) cuStreamDestroy(stream);
        if (contextPushed) { CUcontext previous; cuCtxPopCurrent(&previous); }
        if (context) cuDevicePrimaryCtxRelease(0);
    }

    QJsonObject execute(const QString &root, const QImage &source, QSize target, unsigned mode, float strength, int frames)
    {
        cuda(cuInit(0), "initialize CUDA driver");
        cuda(cuDevicePrimaryCtxRetain(&context, 0), "retain primary context");
        cuda(cuCtxPushCurrent(context), "push context");
        contextPushed = true;
        cuda(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "create stream");
        cuda(cuEventCreate(&begin, CU_EVENT_DEFAULT), "create start event");
        cuda(cuEventCreate(&end, CU_EVENT_DEFAULT), "create end event");
        sdk.open(root);
        cuda(cuMemAllocPitch(&input, &inputPitch, size_t(source.width()) * 4, source.height(), 4), "allocate input");
        cuda(cuMemAllocPitch(&output, &outputPitch, size_t(target.width()) * 4, target.height(), 4), "allocate output");
        sdk.wrap(&sourceImage, source.size(), input, inputPitch);
        sdk.wrap(&destinationImage, target, output, outputPitch);
        CUDA_MEMCPY2D transfer{};
        transfer.srcMemoryType = CU_MEMORYTYPE_HOST;
        transfer.srcHost = source.constBits();
        transfer.srcPitch = source.bytesPerLine();
        transfer.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        transfer.dstDevice = input;
        transfer.dstPitch = inputPitch;
        transfer.WidthInBytes = size_t(source.width()) * 4;
        transfer.Height = source.height();
        cuda(cuMemcpy2D(&transfer), "upload fixed probe image"); // Not timed; harness only.
        sdk.check(sdk.create("VideoSuperRes", &effect), "create VSR");
        sdk.check(sdk.setImage(effect, "SrcImage0", sourceImage), "set input");
        sdk.check(sdk.setImage(effect, "DstImage0", destinationImage), "set output");
        sdk.check(sdk.setStream(effect, "CudaStream", stream), "select stream");
        sdk.check(sdk.setU32(effect, "QualityLevel", mode), "select quality");
        sdk.check(sdk.setF32(effect, "Strength", strength), "set strength");
        auto started = Clock::now();
        sdk.check(sdk.load(effect), "load VSR");
        const double loadMs = elapsed(started);
        QJsonArray samples;
        for (int frame = 0; frame < frames + 10; ++frame) {
            // Reset output to catch an effect that reports success without writing.
            cuda(cuMemsetD2D8Async(output, outputPitch, 0, size_t(target.width()) * 4, target.height(), stream), "clear output");
            cuda(cuEventRecord(begin, stream), "record begin");
            started = Clock::now();
            sdk.check(sdk.run(effect, 1), "enqueue asynchronous VSR");
            const double submitMs = elapsed(started);
            cuda(cuEventRecord(end, stream), "record end");
            CUresult ready;
            while ((ready = cuEventQuery(end)) == CUDA_ERROR_NOT_READY) {
                require(elapsed(started) < 10000, "VFX completion timeout");
                QThread::usleep(50);
            }
            cuda(ready, "query VFX completion");
            float gpuMs = 0;
            cuda(cuEventElapsedTime(&gpuMs, begin, end), "VFX GPU timing");
            if (frame >= 10) samples.append(QJsonObject{{"submit_cpu_ms", submitMs}, {"complete_wall_ms", elapsed(started)}, {"stream_gpu_ms", gpuMs}});
        }
        QImage pixels(target, QImage::Format_RGBA8888);
        transfer = {};
        transfer.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        transfer.srcDevice = output;
        transfer.srcPitch = outputPitch;
        transfer.dstMemoryType = CU_MEMORYTYPE_HOST;
        transfer.dstHost = pixels.bits();
        transfer.dstPitch = pixels.bytesPerLine();
        transfer.WidthInBytes = size_t(target.width()) * 4;
        transfer.Height = target.height();
        cuda(cuMemcpy2D(&transfer), "read back probe output"); // Correctness only, never production.
        double sum = 0, sumSquared = 0;
        qint64 alphaMismatch = 0;
        for (int y = 0; y < pixels.height(); ++y) {
            const auto *row = pixels.constScanLine(y);
            for (int x = 0; x < pixels.width(); ++x) {
                for (int c = 0; c < 3; ++c) { const double v = row[4*x+c]; sum += v; sumSquared += v*v; }
                alphaMismatch += row[4*x+3] != 255;
            }
        }
        const double count = double(pixels.width()) * pixels.height() * 3;
        const double variance = sumSquared / count - std::pow(sum / count, 2);
        require(variance > 10, "Output is unexpectedly flat/black");
        require(alphaMismatch == 0, "Opaque input did not produce opaque output");
        result = pixels;
        char device[256] = {};
        cuda(cuDeviceGetName(device, sizeof(device), 0), "get CUDA device");
        // Confirm that destroying borrowed image descriptors does not free our
        // CUDA allocations. A double-free/ownership mismatch fails this probe.
        cuda(cuStreamSynchronize(stream), "finish standalone probe cleanup");
        sdk.destroy(effect);
        effect = nullptr;
        sdk.imageDestroy(sourceImage);
        sourceImage = nullptr;
        sdk.imageDestroy(destinationImage);
        destinationImage = nullptr;
        cuda(cuMemFree(input), "free caller-owned input after descriptor destruction");
        input = 0;
        cuda(cuMemFree(output), "free caller-owned output after descriptor destruction");
        output = 0;
        return {{"passed", true}, {"python_used", false}, {"sdk_headers_used", false},
                {"opaque_image_api", true}, {"caller_owned_gpu_buffer_cleanup", true},
                {"async_argument", 1}, {"cpu_nonblocking_guaranteed", false},
                {"gpu", device}, {"mode", int(mode)}, {"strength", strength},
                {"input_size", QJsonArray{source.width(), source.height()}}, {"output_size", QJsonArray{target.width(), target.height()}},
                {"load_ms", loadMs}, {"output_rgb_variance", variance}, {"opaque_alpha_mismatches", double(alphaMismatch)},
                {"samples", samples}, {"scope", "Standalone direct C API; stream events and CPU submission; no GL/KWin/presentation"}};
    }
    QImage result;
};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({{"runtime-root", "Extracted audited wheel root", "path"},
        {"input", "Optional PNG", "path"}, {"output-width", "Output width", "pixels", "1280"},
        {"output-height", "Output height", "pixels", "720"}, {"mode", "VSR mode", "number", "3"},
        {"strength", "Enhancement strength", "value", "1"}, {"frames", "Measured repetitions", "count", "60"},
        {"output", "JSON result path", "path"}, {"image-output", "Optional result PNG", "path"}});
    parser.process(app);
    QJsonObject report;
    int status = 0;
    try {
        require(parser.isSet("runtime-root"), "--runtime-root is required");
        auto integer = [&](const char *name, int minimum, int maximum) {
            bool ok = false;
            const int value = parser.value(name).toInt(&ok);
            require(ok && value >= minimum && value <= maximum, "Invalid integer argument");
            return value;
        };
        const QSize target(integer("output-width", 1, 8192), integer("output-height", 1, 8192));
        const int mode = integer("mode", 0, 23);
        require(mode <= 4 || (mode >= 8 && mode <= 19) || mode == 21 || mode == 23, "Reserved VSR mode");
        bool ok = false;
        const float strength = parser.value("strength").toFloat(&ok);
        require(ok && std::isfinite(strength) && strength >= 0 && strength <= 1, "Invalid strength");
        const int frames = integer("frames", 1, 100000);
        QImage input;
        if (parser.isSet("input")) input = QImage(parser.value("input")).convertToFormat(QImage::Format_RGBA8888);
        else {
            input = QImage(640, 360, QImage::Format_RGBA8888);
            for (int y = 0; y < input.height(); ++y) for (int x = 0; x < input.width(); ++x)
                input.setPixelColor(x, y, QColor(x * 255 / input.width(), y * 255 / input.height(), ((x / 32 + y / 32) % 2) ? 180 : 80));
        }
        require(!input.isNull(), "Cannot read input image");
        require(mode < 8 || mode > 15 || input.size() == target, "Denoise/deblur require equal dimensions");
        Probe probe;
        report = probe.execute(parser.value("runtime-root"), input, target, mode, strength, frames);
        if (parser.isSet("image-output")) require(probe.result.save(parser.value("image-output")), "Cannot save output image");
    } catch (const std::exception &error) { report = {{"passed", false}, {"error", error.what()}}; status = 1; }
    const auto json = QJsonDocument(report).toJson();
    if (parser.isSet("output")) {
        QFile file(parser.value("output"));
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) return 2;
    } else std::cout << json.constData();
    return status;
}
