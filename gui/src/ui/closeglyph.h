#pragma once
// The size of the × on a closable tab. Shared by the server tab row and the
// query tab strip so both close buttons stay the same size.

// A fraction of the base font size rather than a fixed px, so the glyph
// follows the prefs slider. The × sits below the body text.
inline constexpr double CloseGlyphScale = 0.7;
