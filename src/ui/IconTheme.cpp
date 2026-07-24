#include "IconTheme.h"

#include <QSvgRenderer>
#include <QPixmap>
#include <QPainter>

namespace IconTheme {

namespace {
QPixmap renderTinted(const QString &resourcePath, const QColor &color, int pixelSize)
{
    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid())
        return QPixmap();

    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter);   // draws the SVG's own stroke (currentColor -> black by default)

    // Recolor: SourceIn keeps only the destination's alpha and replaces its
    // color with the source fill — every non-transparent pixel of the
    // rendered glyph becomes `color`, its shape (the alpha channel) is
    // untouched. Standard Qt technique for tinting monochrome icons.
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), color);
    painter.end();

    return pixmap;
}
}

QIcon tintedIcon(const QString &resourcePath, const QColor &color, int size)
{
    const QPixmap base = renderTinted(resourcePath, color, size);
    if (base.isNull())
        return QIcon();

    QIcon icon;
    icon.addPixmap(base);

    // @2x variant for HiDPI displays. setDevicePixelRatio() is what tells
    // Qt this pixmap represents the same logical size as `base` at twice
    // the resolution — without it, QIcon would treat this as a second,
    // larger *logical* size rather than a retina variant of the first.
    QPixmap hidpi = renderTinted(resourcePath, color, size * 2);
    if (!hidpi.isNull()) {
        hidpi.setDevicePixelRatio(2.0);
        icon.addPixmap(hidpi);
    }

    return icon;
}

}
