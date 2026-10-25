// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone correctness + throughput benchmark. No KWin dependency.
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

class Probe : protected QOpenGLFunctions_3_3_Core
{
public:
    Probe()
    {
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setRenderableType(QSurfaceFormat::OpenGL);
        context.setFormat(format);
        require(context.create(), "Cannot create desktop OpenGL context");
        surface.setFormat(context.format());
        surface.create();
        require(context.makeCurrent(&surface), "Cannot make offscreen context current");
        require(initializeOpenGLFunctions(), "OpenGL 3.3 functions unavailable");
        QFile filter(QStringLiteral(":/opengl/kwin-vfx/filter.glsl"));
        require(filter.open(QIODevice::ReadOnly), "Missing shared filter resource");
        QFile cas(QStringLiteral(":/opengl/kwin-vfx/cas.glsl"));
        require(cas.open(QIODevice::ReadOnly), "Missing CAS resource");
        const QByteArray fragment = QByteArrayLiteral("#version 330 core\n") + filter.readAll() + cas.readAll() + R"(
uniform sampler2D image;
uniform vec2 texel;
uniform float denoise;
uniform float sharpen;
uniform int filterType;
in vec2 uv;
out vec4 color;
void main() { color = filterType == 1 ? casSharpen(image, uv, texel, sharpen) : enhanceWindow(image, uv, texel, denoise, sharpen); }
)";
        require(program.addShaderFromSourceCode(QOpenGLShader::Vertex, R"(#version 330 core
out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})"), "Vertex shader compile failed");
        require(program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragment), "Shared filter compile failed");
        require(program.link(), "Shader link failed");
        program.bind();
        program.setUniformValue("image", 0);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenTextures(1, &input);
        glBindTexture(GL_TEXTURE_2D, input);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenTextures(1, &output);
        glGenFramebuffers(1, &fbo);
        glDisable(GL_BLEND);
        glDisable(GL_DITHER);
    }

    ~Probe()
    {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &input);
        glDeleteTextures(1, &output);
        glDeleteVertexArrays(1, &vao);
    }

    QJsonObject device()
    {
        return {{"vendor", reinterpret_cast<const char *>(glGetString(GL_VENDOR))},
            {"renderer", reinterpret_cast<const char *>(glGetString(GL_RENDERER))},
            {"version", reinterpret_cast<const char *>(glGetString(GL_VERSION))}};
    }

    void setup(const QImage &image, QSize size)
    {
        require(image.format() == QImage::Format_RGBA8888, "Expected raw RGBA8 input");
        glBindTexture(GL_TEXTURE_2D, input);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
        glBindTexture(GL_TEXTURE_2D, output);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Incomplete output FBO");
        glBindTexture(GL_TEXTURE_2D, input);
        glViewport(0, 0, size.width(), size.height());
        program.setUniformValue("texel", QVector2D(1.0f / image.width(), 1.0f / image.height()));
    }

    void draw(float denoise, float sharpen)
    {
        program.setUniformValue("denoise", denoise);
        program.setUniformValue("sharpen", sharpen);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    QImage render(const QImage &image, float denoise, float sharpen, QSize size = {})
    {
        if (size.isEmpty()) size = image.size();
        setup(image, size);
        draw(denoise, sharpen);
        QImage result(size, QImage::Format_RGBA8888);
        glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, result.bits());
        require(glGetError() == GL_NO_ERROR, "OpenGL error in correctness test");
        return result;
    }

    QImage renderCas(const QImage &image, float strength)
    {
        program.setUniformValue("filterType", 1);
        const auto result = render(image, 0, strength);
        program.setUniformValue("filterType", 0);
        return result;
    }

    QJsonObject benchmark(const QImage &image, float denoise, float sharpen)
    {
        setup(image, image.size());
        for (int i = 0; i < 120; ++i) draw(denoise, sharpen);
        glFinish(); // Harness warmup only; never used by the compositor effect.
        constexpr int count = 600;
        GLuint queries[count];
        glGenQueries(count, queries);
        std::vector<double> cpu, gpu;
        QElapsedTimer wall;
        wall.start();
        for (int i = 0; i < count; ++i) {
            QElapsedTimer timer;
            timer.start();
            glBeginQuery(GL_TIME_ELAPSED, queries[i]);
            draw(denoise, sharpen);
            glEndQuery(GL_TIME_ELAPSED);
            cpu.push_back(timer.nsecsElapsed() / 1e6);
            if (i % 16 == 15) glFlush();
        }
        glFlush();
        // Read only completed results; no per-frame synchronous result request.
        GLint ready = GL_FALSE;
        while (!ready) {
            glGetQueryObjectiv(queries[count - 1], GL_QUERY_RESULT_AVAILABLE, &ready);
            require(wall.elapsed() < 30000, "GPU benchmark timeout");
            if (!ready) QThread::usleep(100);
        }
        const double wallMs = wall.nsecsElapsed() / 1e6;
        for (GLuint query : queries) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);
            gpu.push_back(ns / 1e6);
        }
        glDeleteQueries(count, queries);
        require(glGetError() == GL_NO_ERROR, "OpenGL error in GPU benchmark");
        auto summary = [](std::vector<double> samples) {
            std::sort(samples.begin(), samples.end());
            return QJsonObject{{"median_ms", samples[samples.size() / 2]},
                {"p95_ms", samples[size_t(std::ceil(samples.size() * 0.95)) - 1]},
                {"p99_ms", samples[size_t(std::ceil(samples.size() * 0.99)) - 1]}};
        };
        QJsonArray raw;
        for (double time : gpu) raw.append(time);
        return {{"size", QJsonArray{image.width(), image.height()}}, {"samples", count},
            {"denoise", denoise}, {"sharpen", sharpen}, {"gpu", summary(gpu)},
            {"cpu_submit", summary(cpu)}, {"batch_wall_ms", wallMs}, {"gpu_samples_ms", raw}};
    }

private:
    QOpenGLContext context;
    QOffscreenSurface surface;
    QOpenGLShaderProgram program;
    GLuint vao = 0, input = 0, output = 0, fbo = 0;
};

static QImage fixture(QSize size)
{
    QImage image(size, QImage::Format_RGBA8888);
    unsigned seed = 1234567;
    for (int y = 0; y < size.height(); ++y) {
        auto *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            seed = seed * 1664525u + 1013904223u;
            const int noise = int(seed >> 24) % 17 - 8;
            const int base = x < size.width() / 2 ? 100 : 200;
            for (int c = 0; c < 3; ++c) row[x * 4 + c] = base + noise;
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}

static double variance(const QImage &image)
{
    double sum = 0, square = 0;
    int count = 0;
    for (int y = 2; y < image.height() - 2; ++y) {
        for (int x = 2; x < image.width() / 2 - 2; ++x) {
            const double value = image.constScanLine(y)[4 * x];
            sum += value; square += value * value; ++count;
        }
    }
    return square / count - (sum / count) * (sum / count);
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    try {
        Probe probe;
        const QImage input = fixture({128, 96});
        const QImage zero = probe.render(input, 0, 0);
        require(zero == input, "Zero coefficients must reproduce the original bytes");
        const QImage denoised = probe.render(input, 1, 0);
        const double noiseRatio = variance(denoised) / variance(input);
        require(noiseRatio < 0.5, "Denoise did not reduce low-amplitude noise variance");
        require(denoised.constScanLine(40)[63 * 4] < 125 && denoised.constScanLine(40)[64 * 4] > 175,
            "Denoise blurred the hard edge");
        QImage soft({32, 16}, QImage::Format_RGBA8888);
        const int values[] = {40, 40, 60, 110, 145, 195, 215, 215};
        for (int y = 0; y < soft.height(); ++y) {
            for (int x = 0; x < soft.width(); ++x) {
                auto *p = soft.scanLine(y) + x * 4;
                std::fill_n(p, 3, values[std::clamp(x - 12, 0, 7)]);
                p[3] = 255;
            }
        }
        QImage sharp = probe.render(soft, 0, 1);
        const int before = soft.constScanLine(8)[17 * 4] - soft.constScanLine(8)[14 * 4];
        const int after = sharp.constScanLine(8)[17 * 4] - sharp.constScanLine(8)[14 * 4];
        require(after > before + 2, "Sharpening did not increase soft-edge contrast");
        for (int y = 0; y < sharp.height(); ++y) {
            for (int x = 0; x < sharp.width(); ++x) {
                const int value = sharp.constScanLine(y)[x * 4];
                require(value >= 40 && value <= 215, "Sharpening introduced overshoot/halos");
            }
        }
        QImage alpha = fixture({32, 32});
        for (int y = 0; y < alpha.height(); ++y) {
            for (int x = 0; x < alpha.width(); ++x) {
                auto *p = alpha.scanLine(y) + x * 4;
                const int a = x < 8 ? 0 : (x < 16 ? 128 : 255);
                for (int c = 0; c < 3; ++c) p[c] = p[c] * a / 255;
                p[3] = a;
            }
        }
        const QImage filtered = probe.render(alpha, 1, 1);
        for (int y = 0; y < alpha.height(); ++y) {
            for (int x = 0; x < alpha.width(); ++x) {
                const auto *p = filtered.constScanLine(y) + x * 4;
                require(p[3] == alpha.constScanLine(y)[x * 4 + 3], "Alpha changed");
                require(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3], "Invalid premultiplied RGB");
            }
        }
        QImage flat(16, 16, QImage::Format_RGBA8888);
        flat.fill(QColor(100, 120, 140));
        require(probe.render(flat, 1, 1) == flat, "Constant color or borders changed");
        const QImage enlarged = probe.render(flat, 1, 1, {32, 32});
        require(enlarged.pixelColor(0, 0) == flat.pixelColor(0, 0), "Resize/clamped border corrupted constant color");
        require(probe.renderCas(input, 0) == input, "CAS zero did not bypass exactly");
        require(probe.renderCas(flat, 1) == flat, "CAS changed a constant color");
        const auto casAlpha = probe.renderCas(alpha, 1);
        for (int y = 0; y < alpha.height(); ++y) for (int x = 0; x < alpha.width(); ++x) {
            const auto *p = casAlpha.constScanLine(y) + x * 4;
            require(p[3] == alpha.constScanLine(y)[x * 4 + 3], "CAS changed alpha");
            require(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3], "CAS invalid premultiplied RGB");
        }
        const auto casEdge = probe.renderCas(soft, 1);
        const int casContrast = casEdge.constScanLine(8)[17 * 4] - casEdge.constScanLine(8)[14 * 4];
        require(casContrast > before + 2, "CAS did not increase soft-edge contrast");
        QJsonObject result{{"device", probe.device()}, {"correctness_passed", true},
            {"noise_variance_ratio", noiseRatio}, {"soft_edge_contrast_before", before},
            {"soft_edge_contrast_after", after}};
        result.insert("cas_correctness_passed", true);
        result.insert("cas_soft_edge_contrast_after", casContrast);
        if (app.arguments().contains(QStringLiteral("--benchmark"))) {
            QJsonArray cases;
            for (QSize size : {QSize(1920, 1080), QSize(3840, 2160)}) {
                const QImage frame = fixture(size);
                cases.append(probe.benchmark(frame, 0, 0));
                cases.append(probe.benchmark(frame, 0.3f, 0.21f));
                cases.append(probe.benchmark(frame, 1, 1));
            }
            result.insert("benchmark", cases);
            result.insert("scope", "GPU draw from resident RGBA8 texture; excludes KWin capture, presentation and video decode; unpaced throughput");
        }
        const QByteArray json = QJsonDocument(result).toJson();
        const int output = app.arguments().indexOf(QStringLiteral("--output"));
        if (output >= 0) {
            require(output + 1 < app.arguments().size(), "Missing output path");
            QFile file(app.arguments()[output + 1]);
            require(file.open(QIODevice::WriteOnly) && file.write(json) == json.size(), "Cannot write result JSON");
        } else {
            std::cout << json.constData();
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
