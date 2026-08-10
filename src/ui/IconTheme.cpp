#include "IconTheme.h"

#include <QSvgRenderer>
#include <QPixmap>
#include <QPainter>
#include <QHash>

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
    // A real efficiency issue found by code review: every call re-parsed
    // the SVG and re-painted both the base and @2x pixmaps from scratch,
    // including from rebuildModel()'s per-row iconForEntry() call — a
    // directory listing of a few hundred entries re-renders the same
    // handful of distinct (path, color, size) icons hundreds of times
    // over on every navigation and every "Show hidden files" toggle.
    // GUI-thread only (every caller — FilePaneWidget, MainWindow's
    // toolbar, etc. — runs on the GUI thread; nothing in the backend/
    // worker-thread layer touches icons), so a plain QHash needs no
    // locking. Keyed on the resource path, color, and size together —
    // the only three inputs renderTinted() actually depends on.
    static QHash<QString, QIcon> cache;
    const QString key = resourcePath + QLatin1Char(':') + color.name(QColor::HexArgb)
                       + QLatin1Char(':') + QString::number(size);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();

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

    cache.insert(key, icon);
    return icon;
}

}
