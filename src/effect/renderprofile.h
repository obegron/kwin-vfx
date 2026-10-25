// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QSize>
#include <chrono>
#include <vector>

namespace KWin
{
// Explicit opt-in CPU submission timing. GPU timings are measured independently.
class RenderProfile
{
public:
    void begin();
    void end(const QSize &size, bool captured, bool success);
    QJsonObject results(bool reset = false);

private:
    struct Sample {
        QSize size;
        double cpuMs;
        bool captured;
        bool success;
    };
    std::vector<Sample> m_samples;
    std::chrono::steady_clock::time_point m_start;
};
}
