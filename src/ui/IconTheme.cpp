#include "IconTheme.h"

#include <QSvgRenderer>
#include <QPixmap>
#include <QPainter>
#include <QHash>

namespace IconTheme {

namespace {
Theme s_currentTheme = Theme::Dark;
}

void setTheme(Theme theme)
{
    s_currentTheme = theme;
}

Theme currentTheme()
{
    return s_currentTheme;
}

QColor Gray()
{
    // --zf-text-secondary — matches resources/theme.qss's own token of
    // the same name for whichever theme is active. The light value was
    // originally the dark theme's own #5c6472 (its --zf-text-muted, not
    // -secondary — reused on the assumption it'd read fine on white too),
    // but that measured out to a real, reported "washed out" problem: on
    // white it's a noticeably softer-looking gray than the same relative
    // darkness reads as on the dark theme's near-black background (this
    // color is used for nearly every neutral icon in the app — nav
    // buttons, every file/folder row icon, most toolbar icons — so a low-
    // contrast value here was widely visible, not a one-off). Darkened to
    // #4a5160, matching the dark theme's own icon-to-background contrast
    // ratio more closely rather than reusing an unrelated dark-theme token.
    return s_currentTheme == Theme::Light ? QColor(0x4a, 0x51, 0x60) : QColor(0x87, 0x90, 0xa0);
}

QColor GrayMuted()
{
    // --zf-text-muted, same "measurably too washed out on white" reasoning
    // as Gray() above — darkened from #9098a8 (roughly 2.9:1 contrast
    // against white, below even the 3:1 WCAG floor for non-text content)
    // to #78808f (roughly 4.1:1), while keeping it visibly lighter than
    // Gray() so the two still read as distinct "secondary" vs. "muted"
    // tones.
    return s_currentTheme == Theme::Light ? QColor(0x78, 0x80, 0x8f) : QColor(0x5c, 0x64, 0x72);
}

namespace {
// Takes an already-constructed renderer rather than a resourcePath, and
// is called twice per cache miss below (base + @2x) — a real inefficiency
// found by code review: parsing the same SVG resource twice (via two
// separate QSvgRenderer(resourcePath) constructions) to render it twice
// was pure waste, since QSvgRenderer::render() can be called repeatedly
// against different target QPainters once parsed. Now parsed once, in
// tintedIcon(), and reused for both renders.
QPixmap renderTinted(QSvgRenderer &renderer, const QColor &color, int pixelSize)
{
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

    // Deliberately not cached: an invalid resourcePath would re-parse and
    // re-fail on every call for that key instead of being memoized as
    // QIcon() after the first miss — a real gap found by code review, not
    // fixed since it's currently unreachable: every call site in this
    // codebase passes a literal ":/icons/*.svg" path to a real bundled
    // resource, so there's no known way to actually hit this today.
    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid())
        return QIcon();

    const QPixmap base = renderTinted(renderer, color, size);
    if (base.isNull())
        return QIcon();

    QIcon icon;
    icon.addPixmap(base);

    // @2x variant for HiDPI displays. setDevicePixelRatio() is what tells
    // Qt this pixmap represents the same logical size as `base` at twice
    // the resolution — without it, QIcon would treat this as a second,
    // larger *logical* size rather than a retina variant of the first.
    // Hardcoded to 2.0 rather than the display's actual scale factor: a
    // real, known limitation on fractional-scaling displays (1.25x/1.5x,
    // common under GNOME/KDE fractional scaling and on some Windows
    // laptops) — QIcon then has to smooth-upscale one of only two
    // available pixmaps instead of getting a purpose-rendered one at the
    // exact runtime ratio, visibly softer than it could be. Not fixed:
    // tintedIcon() has no widget/screen context to read a real scale
    // factor from, and a naive QGuiApplication::primaryScreen() read
    // would still be wrong on a multi-monitor setup with per-monitor
    // scaling — a proper fix needs real design work, not a quick patch
    // that could be worse than this in some cases.
    QPixmap hidpi = renderTinted(renderer, color, size * 2);
    if (!hidpi.isNull()) {
        hidpi.setDevicePixelRatio(2.0);
        icon.addPixmap(hidpi);
    }

    cache.insert(key, icon);
    return icon;
}

}
