// SPDX-License-Identifier: GPL-2.0-or-later
#include "windoweffect.h"

namespace KWin
{
KWIN_EFFECT_FACTORY_SUPPORTED(WindowEnhancementEffect, "metadata.json",
    return WindowEnhancementEffect::supported();)
}

#include "main.moc"
