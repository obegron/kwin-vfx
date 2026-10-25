// SPDX-License-Identifier: GPL-2.0-or-later
#include "vfx_worker.h"
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); }
static void require(bool value, const char *text) { if (!value) throw std::runtime_error(text); }

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    auto argument = [&](const char *key, QString fallback = {}) {
        const int i = app.arguments().lastIndexOf(QString::fromLatin1(key));
        return i >= 0 && i+1 < app.arguments().size() ? app.arguments()[i+1] : fallback;
    };
    QJsonObject report;
    int status = 0;
    try {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGLES);
        format.setVersion(3, 2);
        QOpenGLContext context;
        context.setFormat(format);
        require(context.create(), "Cannot create main GLES context");
        QOffscreenSurface surface;
        surface.setFormat(context.format()); surface.create();
        require(context.makeCurrent(&surface), "Cannot activate main context");
        const QSize size(argument("--width", "640").toInt(), argument("--height", "360").toInt());
        require(size.width() >= 128 && size.height() >= 128, "Fixture must be at least 128x128");
        const unsigned mode = argument("--mode", "8").toUInt();
        const bool hdr = app.arguments().contains("--true-hdr");
        const bool restoration = !app.arguments().contains("--hdr-only");
        require(mode == 0 || (mode >= 8 && mode <= 15), "Probe requires same-size mode");
        const int frames = argument("--frames", "24").toInt();
        require(frames > 0 && frames <= 1000, "Invalid frame count");
        QImage source(size, QImage::Format_RGBA8888);
        uint32_t random = 12345;
        for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x) {
            random = 1664525 * random + 1013904223;
            const int v = 128 + int((random >> 24) % 25) - 12;
            source.setPixelColor(x, y, QColor(v, v, v));
        }
        std::array<GLuint, 4> textures{}, fbos{};
        glGenTextures(4, textures.data()); glGenFramebuffers(4, fbos.data());
        for (size_t i = 0; i < textures.size(); ++i) {
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            const bool floating = hdr && (i == 1 || i == 3);
            glTexImage2D(GL_TEXTURE_2D, 0, floating ? GL_RGBA16F : GL_RGBA8, size.width(), size.height(), 0, GL_RGBA,
                         floating ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE,
                         i == 0 ? source.constBits() : nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
            require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Invalid fixture FBO");
        }
        QJsonArray heartbeat, workerSamples;
        GLuint published = 0;
        QOpenGLShaderProgram presentation;
        require(presentation.addShaderFromSourceCode(QOpenGLShader::Vertex, R"(#version 300 es
out vec2 uv;
void main() { vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0); }
)"), "Presentation vertex shader failed");
        require(presentation.addShaderFromSourceCode(QOpenGLShader::Fragment, R"(#version 300 es
precision highp float;
uniform sampler2D image;
in vec2 uv;
out vec4 color;
void main() { color=texture(image,uv); }
)"), "Presentation fragment shader failed");
        require(presentation.link(), "Presentation shader link failed");
        auto pulse = [&] {
            auto started = Clock::now();
            glBindFramebuffer(GL_FRAMEBUFFER, fbos[2]);
            glViewport(0, 0, size.width(), size.height());
            if (published) {
                glBindTexture(GL_TEXTURE_2D, published);
                presentation.bind(); presentation.setUniformValue("image", 0);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                presentation.release(); glBindTexture(GL_TEXTURE_2D, 0);
            } else {
                glClearColor(0.1f, 0.3f, 0.5f, 1); glClear(GL_COLOR_BUFFER_BIT);
            }
            glFlush();
            heartbeat.append(ms(started));
            QThread::usleep(1000);
        };
        double lastInputVariance = 0, lastOutputVariance = 0;
        int completed = 0;
        bool failureObserved = false;
        {
            VfxNvidia::Worker worker(argument("--runtime-root"), size, textures[0], textures[1], mode, 1,
                textures[3], hdr, restoration, 650);
            auto wait = [&] {
                auto started = Clock::now();
                while (!worker.accessible() && worker.state() != VfxNvidia::Worker::State::Failed) {
                    require(ms(started) < 30000, "Worker wait timeout");
                    pulse();
                }
                if (worker.state() == VfxNvidia::Worker::State::Failed) {
                    failureObserved = true;
                    if (!app.arguments().contains("--expect-failure")) throw std::runtime_error(worker.error().toStdString());
                    report.insert("expected_error", worker.error());
                    worker.requestStop();
                    require(worker.state() == VfxNvidia::Worker::State::Failed, "Stop overwrote terminal failure");
                }
            };
            wait();
            for (int frame = 0; frame < frames && !failureObserved; ++frame) {
                // No GL access to either slot texture occurs while it is mapped.
                glBindFramebuffer(GL_FRAMEBUFFER, fbos[0]);
                glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 64, 64);
                glClearColor(frame % 2 ? 1 : 0, 0, frame % 2 ? 0 : 1, 1);
                glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
                GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); glFlush();
                worker.submit(fence, uint64_t(frame+1), unsigned(frame % 2));
                pulse(); // Sample the previous output while CUDA owns the other.
                if (app.arguments().contains("--cancel")) {
                    worker.requestStop();
                    auto started = Clock::now();
                    while (!worker.finished()) { require(ms(started) < 30000, "Cancellation timeout"); pulse(); }
                    report.insert("cancelled", true);
                    break;
                }
                wait();
                if (failureObserved) break;
                require(worker.sequence() == uint64_t(frame+1), "Stale output sequence");
                const auto markerValid = [&](const QColor &marker, int sequence) {
                    if (hdr) return (sequence % 2 ? marker.red() > marker.blue() + 20 : marker.blue() > marker.red() + 20);
                    return (sequence % 2 ? marker.red() : marker.blue()) >= 245 && marker.green() <= 10;
                };
                if (frame > 0) {
                    QImage shown(size, QImage::Format_RGBA8888);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbos[2]);
                    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, shown.bits());
                    require(markerValid(shown.pixelColor(32, 32), frame-1), "Published output changed while the other slot was processing");
                }
                auto timing = worker.timing();
                workerSamples.append(QJsonObject{{"sdk_cpu_ms", timing.sdkCpuMs}, {"sdk_gpu_ms", timing.sdkGpuMs}, {"worker_total_ms", timing.totalMs}});
                QImage output(size, QImage::Format_RGBA8888);
                const size_t outputIndex = frame % 2 ? 3 : 1;
                glBindFramebuffer(GL_FRAMEBUFFER, fbos[outputIndex]);
                // Rebind the shared image after the other context modified it.
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                glBindTexture(GL_TEXTURE_2D, textures[outputIndex]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[outputIndex], 0);
                if (hdr) {
                    // GLES does not permit RGBA8 readback directly from this
                    // floating attachment. Read float values in this probe only.
                    std::vector<float> pixels(size_t(size.width()) * size.height() * 4);
                    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, pixels.data());
                    for (size_t i = 0; i < pixels.size(); ++i) {
                        require(std::isfinite(pixels[i]), "Non-finite TrueHDR output");
                        output.bits()[i] = uint8_t(std::clamp(std::lround(pixels[i] * 255), 0l, 255l));
                    }
                } else glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, output.bits()); // Test only.
                const QColor marker = output.pixelColor(32, 32);
                report.insert("last_frame", frame);
                report.insert("last_marker", QJsonArray{marker.red(), marker.green(), marker.blue(), marker.alpha()});
                report.insert("last_gl_error", int(glGetError()));
                report.insert("last_worker_sdk_cpu_ms", timing.sdkCpuMs);
                if (!argument("--image-output").isEmpty()) output.save(argument("--image-output"));
                require(markerValid(marker, frame), "Wrong marker/channel/frame ordering");
                published = textures[outputIndex];
                double a = 0, aa = 0, b = 0, bb = 0;
                for (int y = 80; y < 120; ++y) for (int x = 80; x < 120; ++x) {
                    const double in = source.pixelColor(x, y).red(), out = output.pixelColor(x, y).red();
                    a += in; aa += in*in; b += out; bb += out*out;
                    if (mode == 0 && !hdr) require(source.pixelColor(x,y) == output.pixelColor(x,y), "Identity transfer is not byte exact");
                }
                lastInputVariance = aa/1600 - (a/1600)*(a/1600);
                lastOutputVariance = bb/1600 - (b/1600)*(b/1600);
                ++completed;
            }
            if (app.arguments().contains("--expect-failure")) require(failureObserved, "Expected failure did not occur");
        }
        if (mode == 8 && completed && !hdr) require(lastOutputVariance < lastInputVariance * 0.95, "AI denoise did not reduce fixture noise");
        require(glGetError() == GL_NO_ERROR, "Main GL error after worker teardown");
        glDeleteFramebuffers(4, fbos.data()); glDeleteTextures(4, textures.data());
        std::vector<double> sorted;
        for (const auto &sample : heartbeat) sorted.push_back(sample.toDouble());
        std::sort(sorted.begin(), sorted.end());
        report.insert("passed", true);
        report.insert("completed_frames", completed);
        report.insert("mode", int(mode));
        report.insert("true_hdr", hdr);
        report.insert("restoration", restoration);
        report.insert("ping_pong_presentation", true);
        report.insert("size", QJsonArray{size.width(), size.height()});
        report.insert("main_heartbeat_count", heartbeat.size());
        report.insert("main_gl_submit_p95_ms", sorted.at(size_t((sorted.size()-1)*0.95)));
        report.insert("input_noise_variance", lastInputVariance);
        report.insert("output_noise_variance", lastOutputVariance);
        report.insert("worker_samples", workerSamples);
        report.insert("heartbeat_samples_ms", heartbeat);
        report.insert("scope", "Independent shared EGL/CUDA/VFX worker; GL heartbeat submission, not presentation latency; readback confined to correctness harness");
    } catch (const std::exception &error) { report.insert("passed", false); report.insert("error", error.what()); status = 1; }
    const auto json = QJsonDocument(report).toJson();
    if (const auto output = argument("--output"); !output.isEmpty()) {
        QFile file(output);
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size()) return 2;
    } else std::cout << json.constData();
    return status;
}
