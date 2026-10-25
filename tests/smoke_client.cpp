// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QLabel>
#include <QPalette>
#include <QPainter>
#include <QImage>
#include <QTimer>

class FilterFixture : public QLabel
{
public:
    using QLabel::QLabel;
    QImage pattern;
    int frame = -1;
protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (pattern.isNull()) {
            QLabel::paintEvent(event);
            return;
        }
        QPainter painter(this);
        painter.drawImage(rect(), pattern);
        painter.setPen(Qt::white);
        painter.drawText(QRect(10, 180, 330, 50), Qt::AlignCenter, text());
        if (frame >= 0) painter.fillRect((frame * 3) % width(), 0, 4, height(), Qt::white);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    FilterFixture target(QStringLiteral("Target: Meta+Ctrl+E"));
    FilterFixture control(QStringLiteral("Control: must remain unchanged"));
    if (qEnvironmentVariableIsSet("KWIN_VFX_TEST_PATTERN")) {
        QImage pattern(360, 240, QImage::Format_RGB32);
        unsigned seed = 1234567;
        for (int y = 0; y < pattern.height(); ++y) {
            for (int x = 0; x < pattern.width(); ++x) {
                seed = seed * 1664525u + 1013904223u;
                int value = x < 180 ? 128 + int(seed >> 24) % 25 - 12 : (x < 240 ? 65 : 195);
                pattern.setPixelColor(x, y, QColor(value, value, value));
            }
        }
        target.pattern = pattern;
        control.pattern = pattern;
    }
    for (QLabel *window : {&target, &control}) {
        window->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        window->setAutoFillBackground(true);
        QPalette palette = window->palette();
        palette.setColor(QPalette::Window, QColor(200, 120, 80));
        palette.setColor(QPalette::WindowText, Qt::white);
        window->setPalette(palette);
        window->setAlignment(Qt::AlignCenter);
        window->resize(360, 240);
        window->show();
    }
    target.setWindowTitle(QStringLiteral("VFX test target"));
    control.setWindowTitle(QStringLiteral("VFX test control"));
    QTimer animation;
    if (qEnvironmentVariableIsSet("KWIN_VFX_ANIMATE")) {
        QObject::connect(&animation, &QTimer::timeout, &target, [&target] { ++target.frame; target.update(); });
        animation.start(16);
    }
    return app.exec();
}
