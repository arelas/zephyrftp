# tools/

Build-time helper scripts — not part of the shipped app, not built by the
main CMake configuration.

## generate_app_icon.cpp

Regenerates `resources/icons/app-icon-*.png` and (via the Pillow step
below) `resources/icons/app-icon.ico` from `resources/icons/wind.svg`.
Only needs re-running if the icon design changes.

512 and 1024px are also rendered, beyond what the Windows `.ico` step
below needs (still capped at 256) — they exist for macOS's `.icns`,
assembled by the `build-macos` CI job from these committed PNGs per
Apple's iconset naming convention (`icon_512x512@2x.png` = 1024px).
`.icns` itself is never committed here since `iconutil` only exists on
macOS.

Renders each target size **independently** rather than downsampling one
large master — a shared master downsampled to 16px lost the glyph almost
entirely (2 legible white pixels out of 256, confirmed by direct pixel
inspection) before this was corrected. Small sizes get a thicker stroke
and a larger glyph-to-badge ratio to survive anti-aliasing at taskbar
scale.

```
# From this directory:
g++ -std=c++17 -fPIC $(pkg-config --cflags Qt6Widgets Qt6Svg) \
    -o generate_app_icon generate_app_icon.cpp \
    $(pkg-config --libs Qt6Widgets Qt6Svg)

cp ../resources/icons/wind.svg .
QT_QPA_PLATFORM=offscreen ./generate_app_icon
cp app-icon-*.png ../resources/icons/
```

Then rebuild `app-icon.ico` from the natively-rendered frames (not a
downsample — Pillow will silently resize from the base image if you
don't pass each size explicitly via `append_images`):

```python
from PIL import Image
sizes_needed = [16, 24, 32, 48, 64, 128, 256]
frames = {s: Image.open(f"app-icon-{s}.png").convert("RGBA") for s in sizes_needed}
base = frames[256]
others = [frames[s] for s in sizes_needed if s != 256]
base.save("../resources/icons/app-icon.ico", format="ICO",
          sizes=[(s, s) for s in sizes_needed], append_images=others)
```
