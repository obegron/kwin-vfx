// SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>
// SPDX-License-Identifier: GPL-2.0-or-later
// Capture/drawing patterns adapted from KWin 6.8.1 OffscreenEffect.
#include "windoweffect.h"
#include "renderprofile.h"
#ifdef KWIN_VFX_NVIDIA_BACKEND
#include "nvidiaslot.h"
#endif

#include <core/pixelgrid.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <core/backendoutput.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <opengl/glutils.h>
#include <scene/item.h>
#include <scene/itemgeometry.h>
#include <scene/windowitem.h>
#include <scene/surfaceitem.h>
#include <kdecoration3/decoration.h>

#include <KGlobalAccel>
#include <KConfigGroup>
#include <QAction>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <QDBusConnection>
#include <QUuid>
#include <cmath>
#include <algorithm>

Q_LOGGING_CATEGORY(KWIN_VFX, "kwin.effect.vfx", QtWarningMsg)

namespace KWin
{
static constexpr auto controlPath = "/WindowEnhancement";
static bool selectableWindow(const EffectWindow *window)
{
    return window && !window->isDeleted() && !window->isLockScreen() && !window->isInputMethod()
        && (window->isNormalWindow() || window->isDialog() || window->isUtility());
}

static bool hdrOutputReady(const LogicalOutput *output)
{
    if (!output || !output->backendOutput()->isEnabled()) return false;
    const auto *backend = output->backendOutput();
    if (backend->highDynamicRange()) return true;
    const auto &color = backend->colorDescription();
    const bool extended = color->maxHdrLuminance().value_or(color->referenceLuminance()) > color->referenceLuminance() * 1.01;
    // A nested Wayland output receives extended gamma-2.2 metadata from its
    // parent instead of exposing the DRM HDR flag. EDR uses extended range too.
    return extended && (qgetenv("KWIN_VFX_NESTED") == "1"
        || backend->capabilities().testFlag(BackendOutput::Capability::Edr));
}
// KWin 6.8.1 WorkspaceScene::desiredHdrHeadroom recursively inspects item
// color descriptions. Our drawWindow replacement generates HDR although the
// application's surface stays SDR. This effect-owned item declares that output
// headroom. It has no geometry/quads and never relabels the original pixels.
class HdrHeadroomItem final : public Item
{
public:
    HdrHeadroomItem(Item *parent, const std::shared_ptr<ColorDescription> &color)
        : Item(parent) { setColorDescription(color); }
    void setColor(const std::shared_ptr<ColorDescription> &color) { setColorDescription(color); }
};

static bool hasHdrContent(const Item *item, const Item *effectOutput = nullptr)
{
    if (item == effectOutput) return false;
    const auto &color = item->colorDescription();
    const auto transfer = color->transferFunction().type;
    if (transfer == TransferFunction::PerceptualQuantizer || transfer == TransferFunction::HLG
        || color->maxHdrLuminance().value_or(color->referenceLuminance()) > color->referenceLuminance() * 1.01) {
        return true;
    }
    for (Item *child : item->childItems()) {
        if (hasHdrContent(child, effectOutput)) {
            return true;
        }
    }
    return false;
}

// Unlike OffscreenEffect, this small capture path owns its FBO and propagates
// allocation/capture/vertex-map failures so drawWindow can retry normal rendering.
struct WindowEnhancementEffect::WindowState
{
    // Reverse destruction order keeps the texture alive until its FBO is gone.
    std::unique_ptr<GLTexture> texture;
    std::unique_ptr<GLFramebuffer> framebuffer;
    ItemEffect itemEffect;
    std::unique_ptr<HdrHeadroomItem> hdrHeadroom;
    QMetaObject::Connection damageConnection;
    std::vector<QMetaObject::Connection> itemConnections;
    std::vector<QMetaObject::Connection> windowConnections;
    QSize size;
    qreal scale = 0;
    bool dirty = true;
    bool redirected = false;
    bool captured = false;
    uint64_t revision = 0;
    uint64_t presentedRevision = 0;
    uint64_t rejectedRevision = 0;
    uint64_t lastRevisionGap = 0;
    bool itemTreeDirty = true;
    bool awaitingAi = false;
    bool hdrEnabled = false;
    std::chrono::steady_clock::time_point aiStableAt;
#ifdef KWIN_VFX_NVIDIA_BACKEND
    std::unique_ptr<NvidiaSlot> nvidia;
#endif

    ~WindowState() {
        QObject::disconnect(damageConnection);
        for (const auto &connection : itemConnections) QObject::disconnect(connection);
        for (const auto &connection : windowConnections) QObject::disconnect(connection);
    }
};

WindowEnhancementEffect::WindowEnhancementEffect()
    : m_testFailure(qgetenv("KWIN_VFX_TEST_FAILURE"))
    , m_testTint(qEnvironmentVariableIsSet("KWIN_VFX_TEST_TINT"))
{
#ifdef KWIN_VFX_NVIDIA_BACKEND
    if (qgetenv("KWIN_VFX_NESTED") == "1" && !qEnvironmentVariableIsEmpty("KWIN_VFX_NVIDIA_ROOT")) {
        m_nvidiaRoot = qEnvironmentVariable("KWIN_VFX_NVIDIA_ROOT");
        if (!m_nvidiaRoot.isEmpty()) {
            bool ok = false;
            const int mode = qEnvironmentVariableIntValue("KWIN_VFX_NVIDIA_MODE", &ok);
            if (ok && mode >= 8 && mode <= 15) m_nvidiaMode = unsigned(mode);
            else if (qEnvironmentVariableIsSet("KWIN_VFX_NVIDIA_MODE")) {
                m_nvidiaError = QStringLiteral("NVIDIA preview requires a same-size mode 8 through 15");
                m_nvidiaRoot.clear();
            }
        }
    } else {
        // Desktop activation is an explicit persisted opt-in; the nested
        // launcher's environment never enables CUDA in the desktop compositor.
        effects->config()->reparseConfiguration();
        const KConfigGroup config(effects->config(), QStringLiteral("Effect-kwin_vfx"));
        if (config.readEntry("NvidiaEnabled", false))
            m_nvidiaRoot = config.readPathEntry("NvidiaRuntime", QString());
    }
    if (!m_nvidiaRoot.isEmpty()) {
        auto *timer = new ::QTimer(this);
        timer->setInterval(16);
        connect(timer, &::QTimer::timeout, this, [this] {
            if (!processingEnabled() || effects->isScreenLocked()) { m_nvidiaTimer->stop(); return; }
            bool pending = false;
            for (auto &[window, state] : m_windows) {
                if (state->awaitingAi || (state->nvidia && (state->nvidia->pending() || !state->nvidia->error().isEmpty()
                    || state->nvidia->needsSubmission(state->revision)
                    || state->nvidia->needsPublish()
                    || state->nvidia->size() != state->size
                    || !state->nvidia->matches(state->size, m_nvidiaMode, state->hdrEnabled, !m_casOnly, m_hdrPeakNits)
                    || (m_casOnly && state->nvidia->finished())
                    || (state->nvidia->result() && state->presentedRevision != state->nvidia->completedRevision()))))
                {
                    window->addRepaintFull();
                    pending = true;
                }
            }
            if (!pending) m_nvidiaTimer->stop();
        });
        m_nvidiaTimer = timer;
    }
#endif
    if (qEnvironmentVariableIsSet("KWIN_VFX_PROFILE")) m_profile = std::make_unique<RenderProfile>();
    auto *action = new QAction(this);
    action->setObjectName(QStringLiteral("ToggleWindowEnhancement"));
    action->setProperty("componentName", QStringLiteral("kwin"));
    action->setText(QStringLiteral("Toggle Window Enhancement"));
    action->setAutoRepeat(false);
    KGlobalAccel::self()->setGlobalShortcut(action, QKeySequence(Qt::META | Qt::CTRL | Qt::Key_E));
    connect(action, &QAction::triggered, this, &WindowEnhancementEffect::toggleFocusedWindow);
    auto strengthAction = [this](const QString &name, const QString &label, int key, float delta) {
        auto *action = new QAction(this);
        action->setObjectName(name);
        action->setProperty("componentName", QStringLiteral("kwin"));
        action->setText(label);
        KGlobalAccel::setGlobalShortcut(action, QKeySequence(Qt::META | Qt::CTRL | key));
        connect(action, &QAction::triggered, this, [this, delta] { changeStrength(delta); });
    };
    strengthAction(QStringLiteral("IncreaseWindowEnhancementStrength"), QStringLiteral("Increase Window Enhancement Strength"), Qt::Key_PageUp, 0.1f);
    strengthAction(QStringLiteral("DecreaseWindowEnhancementStrength"), QStringLiteral("Decrease Window Enhancement Strength"), Qt::Key_PageDown, -0.1f);
    connect(effects, &EffectsHandler::windowClosed, this, &WindowEnhancementEffect::disableWindow);
    connect(effects, &EffectsHandler::windowDeleted, this, &WindowEnhancementEffect::disableWindow);
    m_cleanupTimer.setInterval(16);
    connect(&m_cleanupTimer, &QTimer::timeout, this, &WindowEnhancementEffect::collectRetired);
    m_grainTimer.setInterval(42); // Approximately 24 distinct grain patterns/second.
    connect(&m_grainTimer, &QTimer::timeout, this, [this] {
        m_grainFrame = (m_grainFrame + 1) % 4096;
        for (auto &[window, state] : m_windows) window->addRepaintFull();
    });
    connect(effects, &EffectsHandler::screenLockingChanged, this, [this] { updateRedirects(); });
    for (auto *output : effects->screens()) watchOutput(output);
    connect(effects, &EffectsHandler::screenAdded, this, [this](LogicalOutput *output) {
        watchOutput(output); updateHdrAvailability();
    });
    connect(effects, &EffectsHandler::screenRemoved, this, [this] { updateHdrAvailability(); });
    reconfigure(ReconfigureAll);
    m_controlRegistered = QDBusConnection::sessionBus().registerObject(QString::fromLatin1(controlPath), this,
        QDBusConnection::ExportScriptableSlots);
    if (!m_controlRegistered) qCWarning(KWIN_VFX) << "Could not register window enhancement controls";
}

WindowEnhancementEffect::~WindowEnhancementEffect()
{
    if (m_controlRegistered) QDBusConnection::sessionBus().unregisterObject(QString::fromLatin1(controlPath));
    // KWin guarantees a current GL context on effect destruction.
    for (auto &[window, state] : m_windows) retireSlot(*state);
    m_cleanupTimer.stop();
    m_windows.clear();
    m_retired.clear();
#ifdef KWIN_VFX_NVIDIA_BACKEND
    // Module unload is an explicit one-time join: worker code must not outlive
    // its DSO. Normal disable/failure/resize only uses deferred retirement.
    m_retiredSlots.clear();
#endif
    m_shader.reset();
    m_hdrShader.reset();
    m_profile.reset();
    effects->addRepaintFull();
}

bool WindowEnhancementEffect::supported()
{
    return effects->isOpenGLCompositing();
}

bool WindowEnhancementEffect::isActive() const
{
    return processingEnabled() && !m_windows.empty() && !effects->isScreenLocked();
}

bool WindowEnhancementEffect::processingEnabled() const
{
    return m_testTint || (m_strength > 0 && (usingNvidia() || m_grain > 0
        || (m_casOnly ? m_nvidiaSharpen > 0 : m_denoise > 0 || m_sharpen > 0)));
}

bool WindowEnhancementEffect::usingNvidia() const { return !m_nvidiaRoot.isEmpty() && (!m_casOnly || m_trueHdr); }

void WindowEnhancementEffect::watchOutput(LogicalOutput *output)
{
    const auto changed = [this] { QTimer::singleShot(0, this, &WindowEnhancementEffect::updateHdrAvailability); };
    auto *backend = output->backendOutput();
    connect(backend, &BackendOutput::colorDescriptionChanged, this, changed);
    connect(backend, &BackendOutput::highDynamicRangeChanged, this, changed);
    connect(backend, &BackendOutput::enabledChanged, this, changed);
    connect(backend, &BackendOutput::capabilitiesChanged, this, changed);
}

void WindowEnhancementEffect::updateHdrAvailability()
{
    const auto screens = effects->screens();
    m_trueHdr = m_trueHdrSdr || (m_trueHdrRequested && std::any_of(screens.begin(), screens.end(), hdrOutputReady));
    updateRedirects();
}

void WindowEnhancementEffect::updateRedirects()
{
    const bool enabled = processingEnabled();
    if (!enabled && m_nvidiaTimer) m_nvidiaTimer->stop();
    if (enabled && m_grain > 0 && !m_windows.empty() && !effects->isScreenLocked()) m_grainTimer.start();
    else m_grainTimer.stop();
    for (auto &[window, state] : m_windows) {
        const bool outputHdr = m_trueHdrRequested && hdrOutputReady(window->screen());
        const bool hdrEnabled = m_trueHdrSdr || outputHdr;
        if (state->hdrEnabled != hdrEnabled) {
            state->hdrEnabled = hdrEnabled;
            retireSlot(*state);
        }
        if (enabled && outputHdr && !effects->isScreenLocked()) {
            if (!state->hdrHeadroom) state->hdrHeadroom = std::make_unique<HdrHeadroomItem>(window->windowItem(), m_hdrColor);
            else state->hdrHeadroom->setColor(m_hdrColor);
        } else {
            state->hdrHeadroom.reset();
        }
        if (state->redirected != enabled) {
            state->itemEffect = enabled ? ItemEffect(window->windowItem()) : ItemEffect();
            state->redirected = enabled;
            state->dirty = true;
        }
        window->addRepaintFull();
    }
}

void WindowEnhancementEffect::reconfigure(ReconfigureFlags)
{
    effects->config()->reparseConfiguration();
    const KConfigGroup config(effects->config(), QStringLiteral("Effect-kwin_vfx"));
    auto read = [&config](const char *key, float fallback) {
        const float value = config.readEntry(key, fallback);
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
    };
    m_strength = read("Strength", 0.6f);
    m_denoise = read("Denoise", 0.5f);
    m_sharpen = read("Sharpen", 0.35f);
    const float oldCas = m_nvidiaSharpen;
    m_nvidiaSharpen = read("CasStrength", read("NvidiaSharpen", 0.0f));
    m_casOnly = config.readEntry("CasOnly", false);
    m_trueHdrRequested = !m_nvidiaRoot.isEmpty() && config.readEntry("TrueHdr", false);
    m_trueHdrSdr = !m_nvidiaRoot.isEmpty() && config.readEntry("TrueHdrSdr", false);
    const auto oldPeak = m_hdrPeakNits;
    m_hdrPeakNits = std::clamp(config.readEntry("HdrPeakNits", 650u), 400u, 2000u);
    if ((m_trueHdrRequested || m_trueHdrSdr) && (!m_hdrColor || m_hdrPeakNits != oldPeak))
        m_hdrColor = ColorDescription::BT2020PQ->withHdrMetadata(m_hdrPeakNits, m_hdrPeakNits);
    if (m_trueHdr && oldCas != m_nvidiaSharpen)
        for (auto &[window, state] : m_windows) state->dirty = true;
    m_grain = read("FilmGrain", 0.0f);
    const int mode = config.readEntry("NvidiaMode", int(m_nvidiaMode));
    if (mode >= 8 && mode <= 15) m_nvidiaMode = unsigned(mode);
    // A completely disabled shader path doesn't enter paint(), so retire here
    // too when changing from an SDK pipeline to plain CAS/passthrough.
    if (!usingNvidia())
        for (auto &[window, state] : m_windows) retireSlot(*state);
    updateHdrAvailability();
    qCInfo(KWIN_VFX) << "Settings: strength" << m_strength << "denoise" << m_denoise << "sharpen" << m_sharpen;
}

void WindowEnhancementEffect::changeStrength(float delta)
{
    // Round to tenths so repeated decreases reach exact zero (true passthrough).
    m_strength = std::clamp(std::round((m_strength + delta) * 10.0f) / 10.0f, 0.0f, 1.0f);
    KConfigGroup config(effects->config(), QStringLiteral("Effect-kwin_vfx"));
    config.writeEntry("Strength", m_strength);
    config.sync();
    updateRedirects();
    qCInfo(KWIN_VFX) << "Strength:" << m_strength;
}

QString WindowEnhancementEffect::debug(const QString &parameter) const
{
    QJsonObject result{{"strength", m_strength}, {"denoise", m_denoise}, {"sharpen", m_sharpen},
        {"selected", int(m_windows.size())}, {"processing", processingEnabled()}, {"test_tint", m_testTint}};
    result.insert("backend", usingNvidia() ? "nvidia-experimental" : m_casOnly ? "cas" : "shader");
    result.insert("true_hdr", m_trueHdr);
    result.insert("true_hdr_requested", m_trueHdrRequested);
    result.insert("true_hdr_sdr", m_trueHdrSdr);
    const auto screens = effects->screens();
    result.insert("hdr_available", std::any_of(screens.begin(), screens.end(), hdrOutputReady));
    result.insert("true_hdr_output", m_trueHdrRequested && std::any_of(screens.begin(), screens.end(), hdrOutputReady));
    result.insert("hdr_capable", std::any_of(screens.begin(), screens.end(), [](const auto *screen) {
        return screen->backendOutput()->isEnabled() && (hdrOutputReady(screen)
            || screen->backendOutput()->capabilities().testFlag(BackendOutput::Capability::HighDynamicRange));
    }));
    result.insert("hdr_peak_nits", int(m_hdrPeakNits));
    result.insert("nvidia_available", !m_nvidiaRoot.isEmpty());
    result.insert("control_available", m_controlRegistered);
    result.insert("cas_only", m_casOnly);
    result.insert("cas_strength", m_nvidiaSharpen);
    result.insert("hdr_output_seen", m_hdrOutputSeen);
    const auto colorInfo = [](const std::shared_ptr<ColorDescription> &color) {
        return QJsonObject{{"transfer", int(color->transferFunction().type)},
            {"reference_nits", color->referenceLuminance()},
            {"transfer_max_nits", color->transferFunction().maxLuminance},
            {"max_hdr_nits", color->maxHdrLuminance().value_or(color->referenceLuminance())}};
    };
    QJsonArray outputs;
    for (const auto *screen : effects->screens()) {
        outputs.append(QJsonObject{{"name", screen->name()}, {"blending", colorInfo(screen->blendingColor())},
            {"output", colorInfo(screen->backendOutput()->colorDescription())}});
    }
    result.insert("output_colors", outputs);
    if (m_lastTargetColor) result.insert("last_target_color", colorInfo(m_lastTargetColor));
    result.insert("nvidia_draws", double(m_nvidiaDraws));
    result.insert("nvidia_error", m_nvidiaError);
    result.insert("stale_ai_outputs", double(m_staleAiOutputs));
    result.insert("ai_slot_starts", double(m_aiSlotStarts));
#ifdef KWIN_VFX_NVIDIA_BACKEND
    result.insert("retiring_ai_slots", int(m_retiredSlots.size()));
#endif
    result.insert("nvidia_mode", int(m_nvidiaMode));
    result.insert("nvidia_output_scale", 1);
    result.insert("nvidia_sharpen", m_nvidiaSharpen);
    result.insert("film_grain", m_grain);
    result.insert("grain_frame", m_grainFrame);
    result.insert("grain_animating", m_grainTimer.isActive());
#ifdef KWIN_VFX_NVIDIA_BACKEND
    QJsonArray jobs;
    for (const auto &[window, state] : m_windows) {
        Q_UNUSED(window)
        if (state->nvidia) {
            auto job = state->nvidia->debug();
            job.insert("capture_revision", double(state->revision));
            job.insert("last_result_revision_gap", double(state->lastRevisionGap));
            jobs.append(job);
        }
    }
    result.insert("nvidia_jobs", jobs);
#endif
    if (m_profile) {
        result.insert("profile", m_profile->results(parameter == QStringLiteral("reset")));
    }
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

int WindowEnhancementEffect::requestedEffectChainPosition() const
{
    return 99;
}

bool WindowEnhancementEffect::blocksDirectScanout() const
{
    // WindowState's ItemEffect excludes only selected items from promotion.
    return false;
}

bool WindowEnhancementEffect::ensureShader()
{
    if (m_shaderFailed) {
        return false;
    }
    if (!m_shader) {
        if (m_testFailure == "shader") {
            m_shader = ShaderManager::instance()->generateCustomShader(
                ShaderTrait::MapTexture, {}, QByteArrayLiteral("invalid shader for failure test"));
        } else {
            m_shader = ShaderManager::instance()->generateShaderFromFile(
                ShaderTrait::MapTexture, {}, m_testTint ? QStringLiteral(":/kwin-vfx/test.frag") : QStringLiteral(":/kwin-vfx/enhance.frag"));
        }
        if (!m_shader) {
            m_shaderFailed = true;
            qCWarning(KWIN_VFX) << "Shader failed; effect remains passthrough until reloaded";
        }
    }
    if (m_trueHdr && !m_hdrShader) {
        m_hdrShader = ShaderManager::instance()->generateShaderFromFile(
            ShaderTrait::MapTexture, {}, QStringLiteral(":/kwin-vfx/enhance-hdr.frag"));
        if (!m_hdrShader) { m_shaderFailed = true; return false; }
    }
    return bool(m_shader);
}

void WindowEnhancementEffect::toggleFocusedWindow()
{
    EffectWindow *window = effects->activeWindow();
    qCInfo(KWIN_VFX) << "Toggle requested for" << window;
    if (!selectableWindow(window) || effects->isScreenLocked()) return;
    if (m_windows.contains(window)) {
        disableWindow(window);
        return;
    }
    selectWindow(window);
}

QString WindowEnhancementEffect::windowList() const
{
    QJsonArray windows;
    if (!effects->isScreenLocked()) for (auto *window : effects->stackingOrder()) {
        if (!selectableWindow(window)) continue;
        windows.append(QJsonObject{{"id", window->internalId().toString(QUuid::WithoutBraces)},
            {"title", window->caption()}, {"application", window->windowClass()},
            {"enabled", m_windows.contains(window)}, {"active", window == effects->activeWindow()},
            {"hdr_available", hdrOutputReady(window->screen())}});
    }
    return QString::fromUtf8(QJsonDocument(windows).toJson(QJsonDocument::Compact));
}

bool WindowEnhancementEffect::setWindowEnabled(const QString &id, bool enabled)
{
    const QUuid uuid(id);
    if (uuid.isNull() || effects->isScreenLocked()) return false;
    for (auto *window : effects->stackingOrder()) {
        if (window->internalId() != uuid || !selectableWindow(window)) continue;
        if (!enabled) { disableWindow(window); return true; }
        return m_windows.contains(window) || selectWindow(window);
    }
    return false; // The window may have closed since the tray was opened.
}

void WindowEnhancementEffect::disableAllWindows()
{
    while (!m_windows.empty()) disableWindow(m_windows.begin()->first);
}

bool WindowEnhancementEffect::selectWindow(EffectWindow *window)
{
    if (!selectableWindow(window) || effects->isScreenLocked()) return false;
    if (m_windows.contains(window)) return true;
    if (!effects->makeOpenGLContextCurrent() || (processingEnabled() && !ensureShader())) return false;
    collectRetired();
    auto state = std::make_unique<WindowState>();
    state->damageConnection = connect(window, &EffectWindow::windowDamaged, this, [this, window] {
        if (auto it = m_windows.find(window); it != m_windows.end()) {
            it->second->dirty = true;
        }
    });
    const auto invalidate = [this, window] {
        if (const auto it = m_windows.find(window); it != m_windows.end()) {
            it->second->dirty = true;
            it->second->itemTreeDirty = true;
        }
    };
    state->windowConnections.push_back(connect(window, &EffectWindow::windowExpandedGeometryChanged, this, invalidate));
    state->windowConnections.push_back(connect(window, &EffectWindow::windowDecorationChanged, this, invalidate));
    state->windowConnections.push_back(connect(window, &EffectWindow::windowFullScreenChanged, this, invalidate));
    state->windowConnections.push_back(connect(window, &EffectWindow::windowFrameGeometryChanged, this, [this] {
        QTimer::singleShot(0, this, &WindowEnhancementEffect::updateRedirects);
    }));
    m_windows.emplace(window, std::move(state));
    updateRedirects();
    qCInfo(KWIN_VFX) << "Selected window" << window;
    return true;
}

void WindowEnhancementEffect::disableWindow(EffectWindow *window)
{
    if (!m_windows.contains(window)) {
        return;
    }
    // Closed/deleted signals and shortcuts may arrive outside a paint callback.
    auto state = std::move(m_windows.at(window));
    m_windows.erase(window);
    retireSlot(*state);
    if (m_windows.empty() && m_nvidiaTimer) m_nvidiaTimer->stop();
    if (m_windows.empty()) m_grainTimer.stop();
    if (!effects->makeOpenGLContextCurrent()) {
        // Drop selection and scanout restriction even when the context is lost.
        // Retain only GPU allocations until a current context or destruction.
        QObject::disconnect(state->damageConnection);
        state->itemEffect = ItemEffect();
        m_retired.push_back(std::move(state));
        m_cleanupTimer.start();
    } else {
        state.reset();
        collectRetired();
    }
    effects->addRepaintFull();
    qCInfo(KWIN_VFX) << "Deselected window" << window;
}

void WindowEnhancementEffect::retireSlot(WindowState &state)
{
    state.awaitingAi = false;
#ifdef KWIN_VFX_NVIDIA_BACKEND
    if (state.nvidia) {
        state.nvidia->stop();
        m_retiredSlots.push_back(std::move(state.nvidia));
        m_cleanupTimer.start();
    }
#endif
}

void WindowEnhancementEffect::collectRetired()
{
    if (m_retired.empty()
#ifdef KWIN_VFX_NVIDIA_BACKEND
        && m_retiredSlots.empty()
#endif
    ) {
        m_cleanupTimer.stop();
        return;
    }
    if (!effects->makeOpenGLContextCurrent()) return;
    m_retired.clear();
#ifdef KWIN_VFX_NVIDIA_BACKEND
    std::erase_if(m_retiredSlots, [](const auto &slot) { return slot->finished(); });
    if (!m_retiredSlots.empty()) return;
#endif
    m_cleanupTimer.stop();
}

void WindowEnhancementEffect::watchItems(EffectWindow *window, WindowState &state, Item *item, bool root)
{
    const auto invalidate = [this, window] {
        if (const auto it = m_windows.find(window); it != m_windows.end()) it->second->dirty = true;
    };
    const auto structure = [this, window, invalidate] {
        invalidate();
        if (const auto it = m_windows.find(window); it != m_windows.end()) it->second->itemTreeDirty = true;
    };
    state.itemConnections.push_back(connect(item, &Item::childAdded, this, structure));
    state.itemConnections.push_back(connect(item, &Item::childRemoved, this, structure));
    state.itemConnections.push_back(connect(item, &Item::visibleChanged, this, invalidate));
    state.itemConnections.push_back(connect(item, &Item::sizeChanged, this, invalidate));
    state.itemConnections.push_back(connect(item, &Item::boundingRectChanged, this, invalidate));
    if (!root) state.itemConnections.push_back(connect(item, &Item::positionChanged, this, invalidate));
    if (auto *surface = qobject_cast<SurfaceItem *>(item))
        state.itemConnections.push_back(connect(surface, &SurfaceItem::damaged, this, invalidate));
    if (root && window->decoration()) {
        state.itemConnections.push_back(connect(window->decoration(), &KDecoration3::Decoration::damaged, this, invalidate));
        state.itemConnections.push_back(connect(window->decoration(), &KDecoration3::Decoration::shadowChanged, this, structure));
    }
    for (Item *child : item->childItems()) watchItems(window, state, child, false);
}

bool WindowEnhancementEffect::capture(WindowState &state, EffectWindow *window, const RenderViewport &viewport)
{
    state.captured = false;
    if (state.itemTreeDirty) {
        for (const auto &connection : state.itemConnections) QObject::disconnect(connection);
        state.itemConnections.clear();
        watchItems(window, state, window->windowItem());
        state.itemTreeDirty = false;
    }
    const qreal scale = viewport.scale();
    const RectF logicalGeometry = snapToPixels(window->expandedGeometry(), scale);
    const QSize size = (logicalGeometry.size() * scale).toSize();
    if (size.isEmpty() || m_testFailure == "allocation") {
        return false;
    }
    if (!state.texture || state.size != size || state.scale != scale) {
        state.framebuffer.reset();
        state.texture = GLTexture::allocate(GL_RGBA8, size);
        if (!state.texture) {
            return false;
        }
        state.texture->setFilter(GL_LINEAR);
        state.texture->setWrapMode(GL_CLAMP_TO_EDGE);
        state.framebuffer = std::make_unique<GLFramebuffer>(state.texture.get());
        if (!state.framebuffer->valid()) {
            return false;
        }
        state.size = size;
        state.scale = scale;
        state.aiStableAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        state.dirty = true;
    }
    if (!state.dirty) {
        return true;
    }

    const RenderTarget captureTarget(state.framebuffer.get());
    const RenderViewport captureViewport(logicalGeometry, scale, captureTarget, QPoint());
    GLFramebuffer::pushFramebuffer(state.framebuffer.get());
    const auto restoreFramebuffer = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
    GLfloat clearColor[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    const auto restoreClearState = qScopeGuard([&] {
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        if (scissor) {
            glEnable(GL_SCISSOR_TEST);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    });
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    WindowPaintData captureData;
    captureData.setOpacity(1);
    if (!effects->drawWindow(captureTarget, captureViewport, window,
            PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_TRANSLUCENT, Region::infinite(), captureData)
        || m_testFailure == "capture") {
        return false;
    }
    state.dirty = false;
    state.captured = true;
    ++state.revision;
    return true;
}

bool WindowEnhancementEffect::paint(WindowState &state, const RenderTarget &target,
    const RenderViewport &viewport, EffectWindow *window, const Region &region, const WindowPaintData &data)
{
    const bool hdrEnabled = state.hdrEnabled;
    const bool nvidia = !m_nvidiaRoot.isEmpty() && (!m_casOnly || hdrEnabled);
    GLTexture *aiOutput = nullptr;
#ifdef KWIN_VFX_NVIDIA_BACKEND
    if (nvidia && !m_testTint) {
        if (state.nvidia && !state.nvidia->matches(state.size, m_nvidiaMode, hdrEnabled, !m_casOnly, m_hdrPeakNits)) {
            retireSlot(state);
        }
        state.awaitingAi = false;
        if (!state.nvidia) {
            if (std::chrono::steady_clock::now() >= state.aiStableAt && m_retiredSlots.empty()) {
                state.nvidia = std::make_unique<NvidiaSlot>(m_nvidiaRoot, state.size, m_nvidiaMode, hdrEnabled, !m_casOnly, m_hdrPeakNits);
                ++m_aiSlotStarts;
            } else state.awaitingAi = true;
        }
        if (state.nvidia && !state.nvidia->error().isEmpty()) {
            const auto error = state.nvidia->error();
            m_nvidiaError = error;
            qCWarning(KWIN_VFX) << "NVIDIA processing failed:" << error;
            return false;
        }
        if (state.nvidia) {
            state.nvidia->observe(state.revision);
            aiOutput = state.nvidia->result();
            if (aiOutput) {
                state.lastRevisionGap = state.revision - state.nvidia->completedRevision();
                if (state.lastRevisionGap > 1) {
                    if (state.rejectedRevision != state.nvidia->completedRevision()) {
                        ++m_staleAiOutputs;
                        state.rejectedRevision = state.nvidia->completedRevision();
                    }
                    aiOutput = nullptr;
                }
            }
        }
    } else if (state.nvidia) {
        retireSlot(state);
    }
#endif
    const double scale = viewport.scale();
    RectF rect = snapToPixels(window->expandedGeometry(), scale);
    rect.moveTopLeft(rect.topLeft() - snapToPixels(window->frameGeometry(), scale).topLeft());
    RenderGeometry geometry;
    geometry.appendWindowQuad(WindowQuad::fromRect(rect), scale);
    geometry.postProcessTextureCoordinates(state.texture->matrix(NormalizedCoordinates));

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));
    const auto mapped = vbo->map<GLVertex2D>(geometry.size());
    if (!mapped) {
        return false;
    }
    geometry.copy(*mapped);
    vbo->unmap();
    if (m_testFailure == "vertex") {
        return false;
    }

    const bool hdrResult = aiOutput && hdrEnabled;
    auto *shader = hdrResult ? m_hdrShader.get() : m_shader.get();
    ShaderBinder binder(shader);
    shader->setUniform(GLShader::IntUniform::Sampler, 0);
    if (!m_testTint) {
        shader->setUniform("texelSize", QVector2D(1.0f / state.texture->width(), 1.0f / state.texture->height()));
        shader->setUniform("denoiseStrength", m_denoise * m_strength);
        shader->setUniform("sharpenStrength", m_sharpen * m_strength);
        shader->setUniform("filterMode", m_casOnly ? 3 : !nvidia ? 0 : aiOutput ? 2 : m_nvidiaSharpen > 0 ? 3 : 1);
        shader->setUniform("aiSampler", 1);
        shader->setUniform("enhancementStrength", m_strength);
        shader->setUniform("aiSharpenStrength", m_nvidiaSharpen);
        shader->setUniform("aiTexelSize", aiOutput
            ? QVector2D(1.0f / aiOutput->width(), 1.0f / aiOutput->height()) : QVector2D());
        shader->setUniform("grainStrength", m_grain * m_strength);
        shader->setUniform("grainFrame", m_grainFrame);
    }
    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(std::round(window->x() * scale), std::round(window->y() * scale));
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp * data.toMatrix(scale));
    const float rgb = data.brightness() * data.opacity();
    shader->setUniform(GLShader::Vec4Uniform::ModulationConstant, QVector4D(rgb, rgb, rgb, data.opacity()));
    shader->setUniform(GLShader::FloatUniform::Saturation, data.saturation());
    const auto xyz = target.colorDescription()->containerColorimetry().toXYZ();
    shader->setUniform(GLShader::Vec3Uniform::PrimaryBrightness, QVector3D(xyz(1, 0), xyz(1, 1), xyz(1, 2)));
    shader->setColorspaceUniforms(hdrResult ? m_hdrColor : ColorDescription::sRGB, target.colorDescription(), RenderingIntent::Perceptual);
    if (hdrResult) {
        shader->setUniform("originalToDestination", ColorDescription::sRGB->toOther(*target.colorDescription(), RenderingIntent::Perceptual));
        const auto tf = ColorDescription::sRGB->transferFunction();
        shader->setUniform("originalTransferParams", QVector2D(tf.minLuminance, tf.maxLuminance - tf.minLuminance));
    }

    const bool clipping = region != Region::infinite();
    const Region clipRegion = clipping ? viewport.transform().map(region, target.transformedSize()) : Region::infinite();
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    const bool blend = glIsEnabled(GL_BLEND);
    GLint scissorBox[4], srcRgb, dstRgb, srcAlpha, dstAlpha, activeTexture, textureBinding, aiBinding;
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &aiBinding);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
    const auto restoreDrawState = qScopeGuard([&] {
        glBindTexture(GL_TEXTURE_2D, textureBinding);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, aiBinding);
        glActiveTexture(activeTexture);
        glBlendFuncSeparate(srcRgb, dstRgb, srcAlpha, dstAlpha);
        if (!blend) {
            glDisable(GL_BLEND);
        }
        glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
        if (scissor) {
            glEnable(GL_SCISSOR_TEST);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    });
    if (clipping) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    state.texture->bind();
    if (aiOutput) { glActiveTexture(GL_TEXTURE1); aiOutput->bind(); glActiveTexture(GL_TEXTURE0); }
    vbo->bindArrays();
    vbo->draw(clipRegion, GL_TRIANGLES, 0, geometry.count(), clipping);
    vbo->unbindArrays();
    state.texture->unbind();
    if (aiOutput) {
        glActiveTexture(GL_TEXTURE1); aiOutput->unbind(); glActiveTexture(GL_TEXTURE0); ++m_nvidiaDraws;
#ifdef KWIN_VFX_NVIDIA_BACKEND
        state.presentedRevision = state.nvidia->completedRevision();
#endif
    }
#ifdef KWIN_VFX_NVIDIA_BACKEND
    if (nvidia && state.nvidia && state.nvidia->size() == state.size)
        state.nvidia->submit(state.texture.get(), state.revision, hdrEnabled ? m_nvidiaSharpen : 0);
    if (m_nvidiaTimer && (state.awaitingAi || (state.nvidia && (state.nvidia->pending() || state.nvidia->size() != state.size
        || !state.nvidia->matches(state.size, m_nvidiaMode, hdrEnabled, !m_casOnly, m_hdrPeakNits))))) m_nvidiaTimer->start();
#endif
    return true;
}

bool WindowEnhancementEffect::drawWindow(const RenderTarget &target, const RenderViewport &viewport,
    EffectWindow *window, int mask, const Region &region, WindowPaintData &data)
{
    collectRetired();
    const auto it = m_windows.find(window);
    if (it == m_windows.end() || !processingEnabled()) {
        return effects->drawWindow(target, viewport, window, mask, region, data);
    }
    // Capture is explicitly SDR gamma 2.2. Only the TrueHDR shader supports an
    // HDR destination; already-HDR source windows bypass SDR enhancement.
    const auto &color = target.colorDescription();
    m_lastTargetColor = color;
    const bool hdr = color->transferFunction().type == TransferFunction::PerceptualQuantizer
        || color->transferFunction().type == TransferFunction::HLG
        || color->maxHdrLuminance().value_or(color->referenceLuminance()) > color->referenceLuminance() * 1.01;
    m_hdrOutputSeen |= hdr;
    if (m_profile) m_profile->begin();
    bool success = false;
    try {
        success = ensureShader() && (!hdr || m_trueHdr) && !hasHdrContent(window->windowItem(), it->second->hdrHeadroom.get())
            && capture(*it->second, window, viewport) && paint(*it->second, target, viewport, window, region, data);
    } catch (const std::exception &error) {
        m_nvidiaError = QString::fromUtf8(error.what());
        qCWarning(KWIN_VFX) << "Enhancement exception:" << error.what();
    }
    if (m_profile) m_profile->end(it->second->size, it->second->captured, success);
    if (!success) {
        qCWarning(KWIN_VFX) << "Unsupported color target or processing failure; disabling" << window;
        disableWindow(window);
        return effects->drawWindow(target, viewport, window, mask, region, data);
    }
    return true;
}
}
