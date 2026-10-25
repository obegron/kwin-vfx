// SPDX-License-Identifier: GPL-2.0-or-later
#include "nvidiaslot.h"
#include "../backends/nvidia/vfx_worker.h"
#include <opengl/glutils.h>
#include <QScopeGuard>
#include <QJsonArray>
#include <stdexcept>

namespace KWin {
NvidiaSlot::NvidiaSlot(const QString &runtime, QSize size, unsigned mode, bool trueHdr, bool restoration, unsigned peakNits)
    : m_size(size), m_mode(mode), m_trueHdr(trueHdr), m_restoration(restoration), m_peakNits(peakNits)
{
    if (size.isEmpty() || size.width() > 3840 || size.height() > 2160)
        throw std::runtime_error("NVIDIA input exceeds experimental size limit");
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (size.width() > maximum || size.height() > maximum)
        throw std::runtime_error("NVIDIA intermediate exceeds GL texture limit");
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    auto restore = qScopeGuard([&] { glBindTexture(GL_TEXTURE_2D, previous); });
    glGenTextures(3, m_names.data());
    auto cleanup = qScopeGuard([&] { glDeleteTextures(3, m_names.data()); });
    for (size_t index = 0; index < m_names.size(); ++index) {
        const GLuint texture = m_names[index];
        const QSize dimensions = size;
        const bool floating = index > 0 && m_trueHdr;
        if (!texture) throw std::runtime_error("Cannot create NVIDIA staging texture");
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, floating ? GL_RGBA16F : GL_RGBA8, dimensions.width(), dimensions.height(), 0,
            GL_RGBA, floating ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
        GLint allocatedWidth = 0, allocatedHeight = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &allocatedWidth);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &allocatedHeight);
        if (allocatedWidth != dimensions.width() || allocatedHeight != dimensions.height())
            throw std::runtime_error("Cannot allocate NVIDIA staging texture");
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    m_input = GLTexture::createNonOwningWrapper(m_names[0], GL_RGBA8, size);
    for (size_t index = 0; index < m_outputs.size(); ++index) {
        m_outputs[index] = GLTexture::createNonOwningWrapper(m_names[1 + index], m_trueHdr ? GL_RGBA16F : GL_RGBA8, size);
        m_outputs[index]->setFilter(GL_LINEAR);
    }
    m_framebuffer = std::make_unique<GLFramebuffer>(m_input.get());
    if (!m_framebuffer->valid()) throw std::runtime_error("NVIDIA staging framebuffer failed");
    m_copyShader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, {}, QStringLiteral(":/kwin-vfx/nvidia-input.frag"));
    if (!m_copyShader) throw std::runtime_error("NVIDIA input shader failed");
    glFlush();
    m_worker = std::make_unique<VfxNvidia::Worker>(runtime, size, m_names[0], m_names[1], mode, 1.0f,
        m_names[2], m_trueHdr, m_restoration, m_peakNits);
    cleanup.dismiss();
}

NvidiaSlot::~NvidiaSlot()
{
    m_worker.reset(); // Unmap/unregister and join before deleting shared textures.
    m_framebuffer.reset();
    m_input.reset();
    for (auto &output : m_outputs) output.reset();
    glDeleteTextures(3, m_names.data());
}

GLTexture *NvidiaSlot::result() const
{
    return !m_stopping && m_publishedRevision ? m_outputs[m_publishedIndex].get() : nullptr;
}
bool NvidiaSlot::matches(QSize size, unsigned mode, bool hdr, bool restoration, unsigned peak) const
{
    return m_size == size && m_mode == mode && m_trueHdr == hdr
        && m_restoration == restoration && (!hdr || m_peakNits == peak);
}
uint64_t NvidiaSlot::completedRevision() const { return m_publishedRevision; }
bool NvidiaSlot::needsPublish() const { return !m_stopping && m_worker->state() == VfxNvidia::Worker::State::Ready && m_worker->sequence() != m_publishedRevision; }

QString NvidiaSlot::error() const
{
    return m_worker->state() == VfxNvidia::Worker::State::Failed ? m_worker->error() : QString();
}

bool NvidiaSlot::pending() const
{
    return m_stopping ? !finished() : !m_worker->accessible() && error().isEmpty();
}
bool NvidiaSlot::needsSubmission(uint64_t revision) const { return !m_stopping && m_worker->accessible() && revision != m_submitted; }
void NvidiaSlot::stop() { if (!m_stopping) { m_stopping = true; m_worker->requestStop(); } }
bool NvidiaSlot::finished() const { return m_worker->finished(); }

void NvidiaSlot::submit(GLTexture *source, uint64_t revision, float preCasStrength)
{
    if (m_stopping || !m_worker->accessible() || revision == m_submitted) return;
    GLint binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    const bool blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST), dither = glIsEnabled(GL_DITHER);
    GLFramebuffer::pushFramebuffer(m_framebuffer.get());
    auto restore = qScopeGuard([&] {
        GLFramebuffer::popFramebuffer();
        glBindTexture(GL_TEXTURE_2D, binding);
        if (blend) glEnable(GL_BLEND);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (dither) glEnable(GL_DITHER);
    });
    glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_DITHER);
    ShaderBinder binder(m_copyShader.get());
    QMatrix4x4 projection;
    projection.ortho(0, m_size.width(), m_size.height(), 0, -1, 1);
    m_copyShader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
    m_copyShader->setUniform(GLShader::IntUniform::Sampler, 0);
    m_copyShader->setUniform("preCasStrength", preCasStrength);
    m_copyShader->setUniform("texelSize", QVector2D(1.0f / source->width(), 1.0f / source->height()));
    source->bind(); source->render(m_size); source->unbind();
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!fence) throw std::runtime_error("NVIDIA input fence failed");
    glFlush();
    // Keep the published output GL-readable while CUDA writes the other one.
    const unsigned nextOutput = m_publishedRevision ? 1 - m_publishedIndex : 0;
    m_worker->submit(fence, revision, nextOutput);
    m_submitted = revision;
}

QJsonObject NvidiaSlot::debug() const
{
    QJsonObject result{{"state", int(m_worker->state())}, {"submitted_revision", double(m_submitted)}, {"stopping", m_stopping}};
    result.insert("mode", int(m_mode));
    result.insert("true_hdr", m_trueHdr);
    result.insert("restoration", m_restoration);
    result.insert("hdr_peak_nits", int(m_peakNits));
    result.insert("input_size", QJsonArray{m_size.width(), m_size.height()});
    result.insert("output_size", QJsonArray{m_size.width(), m_size.height()});
    result.insert("published_revision", double(m_publishedRevision));
    if (m_worker->state() == VfxNvidia::Worker::State::Ready) {
        const auto timing = m_worker->timing();
        result.insert("completed_revision", double(m_worker->sequence()));
        result.insert("sdk_cpu_ms", timing.sdkCpuMs); result.insert("worker_total_ms", timing.totalMs);
        result.insert("sdk_gpu_ms", timing.sdkGpuMs);
    }
    if (const auto message = error(); !message.isEmpty()) result.insert("error", message);
    result.insert("timing_samples", m_timings);
    return result;
}

void NvidiaSlot::observe(uint64_t captureRevision)
{
    if (!needsPublish()) return;
    m_publishedIndex = m_worker->outputIndex();
    m_publishedRevision = m_worker->sequence();
    m_recorded = m_worker->sequence();
    if (!qEnvironmentVariableIsSet("KWIN_VFX_PROFILE") || m_timings.size() >= 512) return;
    const auto timing = m_worker->timing();
    m_timings.append(QJsonObject{{"sdk_cpu_ms", timing.sdkCpuMs}, {"sdk_gpu_ms", timing.sdkGpuMs},
        {"worker_total_ms", timing.totalMs}, {"revision_gap", double(captureRevision - m_recorded)},
        {"completed_revision", double(m_recorded)}});
}
}
