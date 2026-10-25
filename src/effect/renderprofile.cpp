// SPDX-License-Identifier: GPL-2.0-or-later
#include "renderprofile.h"
#include <QJsonArray>

namespace KWin
{
void RenderProfile::begin()
{
    if (m_samples.capacity() == 0) m_samples.reserve(8192);
    m_start = std::chrono::steady_clock::now();
}

void RenderProfile::end(const QSize &size, bool captured, bool success)
{
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
    if (m_samples.size() < 8192) m_samples.push_back({size, elapsed, captured, success});
}

QJsonObject RenderProfile::results(bool reset)
{
    QJsonArray samples;
    for (const auto &sample : m_samples) {
        samples.append(QJsonObject{{"size", QJsonArray{sample.size.width(), sample.size.height()}},
            {"cpu_ms", sample.cpuMs}, {"captured", sample.captured}, {"success", sample.success}});
    }
    QJsonObject result{{"gpu_timing", false}, {"scope", "CPU submission of capture and draw; excludes GPU completion and presentation"},
        {"samples", samples}, {"limit", 8192}};
    if (reset) m_samples.clear();
    return result;
}
}
