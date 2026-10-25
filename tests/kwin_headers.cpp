// Compile-only KWin interface probe. This is not an effect implementation or runtime test.
#include <effect/effect.h>
#include <effect/effecthandler.h>
#include <effect/offscreeneffect.h>
#include <type_traits>

static_assert(std::is_base_of_v<KWin::Effect, KWin::OffscreenEffect>);
static_assert(KWIN_EFFECT_API_VERSION_MINOR == 237);
using DrawWindow = bool (KWin::Effect::*)(const KWin::RenderTarget &,
    const KWin::RenderViewport &, KWin::EffectWindow *, int,
    const KWin::Region &, KWin::WindowPaintData &);
static_assert(std::is_same_v<decltype(&KWin::Effect::drawWindow), DrawWindow>);
static_assert(std::is_same_v<decltype(std::declval<KWin::EffectsHandler>().activeWindow()),
    KWin::EffectWindow *>);
