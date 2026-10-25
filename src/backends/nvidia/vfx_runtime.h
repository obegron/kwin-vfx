// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Narrow C binding to the audited wheel; opaque descriptors only. See README.md in this directory.
#include <QDir>
#include <QFile>
#include <QSize>
#include <cuda.h>
#include <dlfcn.h>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <stdexcept>
#include <vector>
namespace VfxNvidia {
struct NvCVImage;
struct NvVFX_Object;
using Effect = NvVFX_Object *;
inline void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
class Runtime
{
public:
    // Narrow, documented Linux C ABI. Images are always SDK-owned descriptors.
    int (*create)(const char *, Effect *) = nullptr;
    void (*destroy)(Effect) = nullptr;
    int (*setU32)(Effect, const char *, unsigned) = nullptr;
    int (*setF32)(Effect, const char *, float) = nullptr;
    int (*setImage)(Effect, const char *, NvCVImage *) = nullptr;
    int (*setStream)(Effect, const char *, CUstream) = nullptr;
    int (*load)(Effect) = nullptr;
    int (*run)(Effect, int) = nullptr;
    int (*imageCreate)(unsigned, unsigned, int, int, unsigned, unsigned, unsigned, NvCVImage **) = nullptr;
    void (*imageDealloc)(NvCVImage *) = nullptr;
    void (*imageDestroy)(NvCVImage *) = nullptr;
    int (*imageInit)(NvCVImage *, unsigned, unsigned, unsigned, void *, int, int, unsigned, unsigned) = nullptr;
    int (*imageTransfer)(const NvCVImage *, NvCVImage *, float, CUstream, NvCVImage *) = nullptr;
    void (*componentOffsets)(int, int *, int *, int *, int *, int *) = nullptr;
    const char *(*errorText)(int) = nullptr;

    void open(const QString &root, bool trueHdr = false)
    {
        const QDir package(root);
        QFile metadata(package.filePath("nvidia_vfx-0.2.0.0.dist-info/METADATA"));
        require(metadata.open(QIODevice::ReadOnly) && metadata.readAll().contains("\nVersion: 0.2.0.0\n"),
                "This experimental loader only accepts the audited nvidia-vfx 0.2.0.0 wheel layout");
        const QDir dependencies(package.filePath("nvidia_vfx.libs"));
        const auto cudaLibraries = dependencies.entryList({"libcudart-*.so.12.9.79"}, QDir::Files);
        require(cudaLibraries.size() == 1, "Expected the audited bundled CUDA 12.9 runtime");
        library(dependencies.filePath(cudaLibraries.front()));
        const QDir libs(package.filePath("nvvfx/libs"));
        library(libs.filePath("libnppc.so.12"));
        for (const auto &name : libs.entryList({"libnpp*.so.12"}, QDir::Files)) {
            if (name != "libnppc.so.12") library(libs.filePath(name));
        }
        library(libs.filePath("libcudnn.so.9"));
        void *image = library(libs.filePath("libNVCVImage.so"));
        library(libs.filePath("libnvngxruntime.so"));
        library(libs.filePath("libVideoFXLocal.so"));
        void *vfx = library(libs.filePath("libVideoFX.so"));
        library(libs.filePath("libnvidia-ngx-vsr.so.1.8.3"));
        library(libs.filePath("libnvVFXVideoSuperRes.so"));
        if (trueHdr) {
            library(libs.filePath("libnvidia-ngx-truehdr.so.1.2.3"));
            library(libs.filePath("libnvVFXTrueHDR.so"));
        }
        resolve(create, vfx, "NvVFX_CreateEffect");
        resolve(destroy, vfx, "NvVFX_DestroyEffect");
        resolve(setU32, vfx, "NvVFX_SetU32");
        resolve(setF32, vfx, "NvVFX_SetF32");
        resolve(setImage, vfx, "NvVFX_SetImage");
        resolve(setStream, vfx, "NvVFX_SetCudaStream");
        resolve(load, vfx, "NvVFX_Load");
        resolve(run, vfx, "NvVFX_Run");
        resolve(imageCreate, image, "NvCVImage_Create");
        resolve(imageDealloc, image, "NvCVImage_Dealloc");
        resolve(imageDestroy, image, "NvCVImage_Destroy");
        resolve(imageInit, image, "NvCVImage_Init");
        resolve(imageTransfer, image, "NvCVImage_Transfer");
        resolve(componentOffsets, image, "NvCVImage_ComponentOffsets");
        resolve(errorText, image, "NvCV_GetErrorStringFromCode");
        int red = -1, green = -1, blue = -1, alpha = -1;
        componentOffsets(6, &red, &green, &blue, &alpha, nullptr);
        require(red == 0 && green == 1 && blue == 2 && alpha == 3, "Runtime RGBA enumeration/channel contract mismatch");
    }
    // Pin this audited runtime for process lifetime. CUDA/NGX may retain runtime
    // callbacks; per-window dlclose is neither safe isolation nor useful cleanup.
    ~Runtime() = default;
    void check(int status, const char *stage) const
    {
        if (!status) return;
        const char *message = errorText(status);
        throw std::runtime_error(std::string(stage) + ": " + (message ? message : "unknown VFX status") + " (" + std::to_string(status) + ")");
    }
    void wrap(NvCVImage **image, QSize size, CUdeviceptr buffer, size_t pitch, int format = 6, int type = 1)
    {
        require(pitch <= std::numeric_limits<unsigned>::max(), "Image pitch exceeds API range");
        // Obtain a descriptor through the SDK's documented C constructor. Free
        // its tiny startup-only buffer before wrapping our own GPU allocation.
        check(imageCreate(1, 1, 6, 1, 0, 0, 1, image), "create opaque image descriptor");
        imageDealloc(*image); // Documented to free the buffer and zero the descriptor.
        check(imageInit(*image, size.width(), size.height(), unsigned(pitch), reinterpret_cast<void *>(buffer),
                        format, type, 0, 1), "wrap pitch-linear GPU image");
    }
private:
    void *library(const QString &path)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, void *> libraries;
        const auto encoded = QFile::encodeName(path);
        const std::string key(encoded.constData());
        const std::lock_guard lock(mutex);
        if (const auto found = libraries.find(key); found != libraries.end()) return found->second;
        void *handle = dlopen(encoded.constData(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
        if (!handle) throw std::runtime_error(path.toStdString() + ": " + dlerror());
        libraries.emplace(key, handle);
        return handle;
    }
    template<class T> void resolve(T &target, void *handle, const char *name)
    {
        target = reinterpret_cast<T>(dlsym(handle, name));
        if (!target) throw std::runtime_error(std::string("Missing SDK symbol: ") + name);
    }
};

} // namespace VfxNvidia
