// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <effect/effect.h>
#include <map>
#include <memory>
#include <vector>
#include <QTimer>

namespace KWin
{
class GLShader;
class RenderProfile;
class Item;
class NvidiaSlot;
class ColorDescription;

class WindowEnhancementEffect : public Effect
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.WindowEnhancement")
public:
    WindowEnhancementEffect();
    ~WindowEnhancementEffect() override;
    static bool supported();
    bool isActive() const override;
    void reconfigure(ReconfigureFlags flags) override;
    QString debug(const QString &parameter) const override;
    int requestedEffectChainPosition() const override;
    bool blocksDirectScanout() const override;
    bool drawWindow(const RenderTarget &target, const RenderViewport &viewport,
        EffectWindow *window, int mask, const Region &region, WindowPaintData &data) override;

public Q_SLOTS:
    Q_SCRIPTABLE QString windowList() const;
    Q_SCRIPTABLE bool setWindowEnabled(const QString &id, bool enabled);
    Q_SCRIPTABLE void disableAllWindows();

private:
    struct WindowState;
    void toggleFocusedWindow();
    bool selectWindow(EffectWindow *window);
    void disableWindow(EffectWindow *window);
    void collectRetired();
    void retireSlot(WindowState &state);
    void watchItems(EffectWindow *window, WindowState &state, Item *item, bool root = true);
    bool ensureShader();
    bool processingEnabled() const;
    bool usingNvidia() const;
    void watchOutput(LogicalOutput *output);
    void updateHdrAvailability();
    void updateRedirects();
    void changeStrength(float delta);
    bool capture(WindowState &state, EffectWindow *window, const RenderViewport &viewport);
    bool paint(WindowState &state, const RenderTarget &target, const RenderViewport &viewport,
        EffectWindow *window, const Region &region, const WindowPaintData &data);

    std::unique_ptr<GLShader> m_shader;
    std::unique_ptr<GLShader> m_hdrShader;
    std::shared_ptr<ColorDescription> m_hdrColor;
    std::unique_ptr<RenderProfile> m_profile;
    std::map<EffectWindow *, std::unique_ptr<WindowState>> m_windows;
    std::vector<std::unique_ptr<WindowState>> m_retired;
#ifdef KWIN_VFX_NVIDIA_BACKEND
    std::vector<std::unique_ptr<NvidiaSlot>> m_retiredSlots;
#endif
    QTimer m_cleanupTimer;
    uint64_t m_staleAiOutputs = 0;
    uint64_t m_aiSlotStarts = 0;
    const QByteArray m_testFailure;
    const bool m_testTint;
    float m_strength = 0.6f;
    float m_denoise = 0.5f;
    float m_sharpen = 0.35f;
    bool m_shaderFailed = false;
    bool m_controlRegistered = false;
    QString m_nvidiaRoot;
    QString m_nvidiaError;
    unsigned m_nvidiaMode = 8;
    float m_nvidiaSharpen = 0;
    bool m_casOnly = false;
    bool m_trueHdr = false;
    bool m_trueHdrRequested = false;
    bool m_trueHdrSdr = false;
    unsigned m_hdrPeakNits = 650;
    bool m_hdrOutputSeen = false;
    std::shared_ptr<ColorDescription> m_lastTargetColor;
    float m_grain = 0;
    int m_grainFrame = 0;
    QTimer m_grainTimer;
    uint64_t m_nvidiaDraws = 0;
    QTimer *m_nvidiaTimer = nullptr;
};
}
