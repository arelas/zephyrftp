// v2: renders EACH target size independently rather than downsampling one
// master — a 512px master downsampled to 16px loses the glyph almost
// entirely (confirmed: only 2 clearly-white pixels out of 256 in the
// first attempt). Small sizes get a boosted stroke width and a larger
// glyph-to-badge ratio so the "breeze" symbol survives at taskbar scale.
#include <QApplication>
#include <QSvgRenderer>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QFile>
#include <QByteArray>

QPixmap renderIconAtSize(int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath badgePath;
    const qreal margin = size * 0.016;
    const qreal radius = size * 0.22;
    badgePath.addRoundedRect(margin, margin, size - margin * 2, size - margin * 2, radius, radius);

    // Night-sky indigo-to-violet gradient, replacing the original flat
    // daytime blue (#5b8def) — deliberately distinct from
    // IconTheme::Blue, which is a separate in-app semantic color
    // (navigation/download) with its own reason for staying bright and
    // legible against the app's own dark UI; the app icon badge has no
    // such constraint and can read as "dark" on its own.
    QLinearGradient badgeGradient(margin, margin, size - margin, size - margin);
    badgeGradient.setColorAt(0.0, QColor(0x1e, 0x1b, 0x4b));   // deep midnight indigo
    badgeGradient.setColorAt(1.0, QColor(0x43, 0x38, 0x84));   // muted royal blue-violet
    painter.fillPath(badgePath, badgeGradient);

    // Small icons get a thicker relative stroke and a bigger glyph
    // fraction — thin strokes and small margins that look fine at 256px
    // vanish into anti-aliasing mush at 16-32px.
    double strokeWidth;
    double glyphFraction;
    if (size <= 20)       { strokeWidth = 4.2; glyphFraction = 0.62; }
    else if (size <= 40)  { strokeWidth = 3.2; glyphFraction = 0.58; }
    else if (size <= 72)  { strokeWidth = 2.4; glyphFraction = 0.54; }
    else                  { strokeWidth = 2.0; glyphFraction = 0.52; }

    QFile svgFile("wind.svg");
    svgFile.open(QIODevice::ReadOnly | QIODevice::Text);
    QByteArray svgData = svgFile.readAll();
    svgData.replace("stroke-width=\"2\"",
                     QByteArray("stroke-width=\"") + QByteArray::number(strokeWidth) + "\"");

    QSvgRenderer renderer(svgData);
    if (!renderer.isValid()) {
        qWarning("Failed to load modified wind.svg at size %d", size);
        return pixmap;
    }

    const int glyphSize = static_cast<int>(size * glyphFraction);
    QPixmap glyph(glyphSize, glyphSize);
    glyph.fill(Qt::transparent);
    QPainter glyphPainter(&glyph);
    glyphPainter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&glyphPainter);
    glyphPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    glyphPainter.fillRect(glyph.rect(), Qt::white);
    glyphPainter.end();

    const int glyphX = (size - glyphSize) / 2;
    const int glyphY = (size - glyphSize) / 2;
    painter.drawPixmap(glyphX, glyphY, glyph);
    painter.end();

    return pixmap;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    const int sizes[] = {16, 24, 32, 48, 64, 128, 256, 512};
    for (int size : sizes) {
        QPixmap pm = renderIconAtSize(size);
        const QString filename = QStringLiteral("app-icon-%1.png").arg(size);
        pm.save(filename);
        qDebug("Saved %s", qPrintable(filename));
    }
    return 0;
}
