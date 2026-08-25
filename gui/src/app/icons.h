#pragma once
// Lucide glyphs, the same set the web build imported from lucide-react. The
// SVGs in assets/icons were extracted from that package's icon definitions
// (ISC — see assets/icons/LICENSE.txt), so these are the same shapes, not
// lookalikes.
//
// Rendered on demand at the requested size and colour: lucide strokes with
// `currentColor`, so recolouring is a substitution before rasterising, which
// keeps the icons following the palette instead of baking a theme in.
#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace icons
{

// px is the logical size; the pixmap is rendered for the current device pixel
// ratio so it stays sharp on a scaled display.
QPixmap pixmap(const QString &name, const QColor &colour, int px = 16);
QIcon icon(const QString &name, const QColor &colour, int px = 16);

} // namespace icons
