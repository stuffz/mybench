#pragma once
// Theming seam. The app ships one palette (black) and one editor palette
// (gruvbox), but both are table-driven from the same shape the web build used
// (frontend/src/index.css tokens, frontend/src/lib/editorThemes.ts palettes),
// so adding the remaining themes is data, not code.
#include <QColor>
#include <QObject>
#include <QString>
#include <QVector>

class QScreen;

// Mirrors the shadcn token set the web UI themes with. Values below are the
// oklch tokens from index.css converted to sRGB — same look, no colour maths
// at runtime.
struct AppPalette
{
    QColor background, foreground;
    QColor card, cardFg;       // dialogs, popovers
    QColor primary, primaryFg; // solid buttons
    QColor secondary, muted, mutedFg, accent;
    // Hover fill. Kept apart from muted because muted is the card's own tone in
    // most themes, and a control hovering to the colour it sits on shows
    // nothing at all.
    QColor hover;
    QColor destructive, warning, success, info, special;
    QColor border, input, ring;
    QColor sidebar, sidebarBorder;
};

// Mirrors the Palette type in lib/editorThemes.ts one-for-one.
struct EditorPalette
{
    QColor bg, fg, caret, sel, line, panel;
    QColor comment, keyword, string, number, func, type;
};

struct AppTheme
{
    QString id, label;
    AppPalette p;
};

struct EditorTheme
{
    QString id, label;
    EditorPalette p;
};

// Emitted after apply() installs a new palette. Widgets that captured colours
// — per-widget stylesheets, rendered icons, item foregrounds — reload on this;
// widgets that read theme::current() per paint need nothing.
class Notifier : public QObject
{
    Q_OBJECT
public:
    void notifyChanged() { emit changed(); }

signals:
    void changed();
};

namespace theme
{

Notifier *notifier();

const QVector<AppTheme> &appThemes();
const QVector<EditorTheme> &editorThemes();

// Lookup by id, falling back to the default (black / gruvbox).
const AppPalette &app(const QString &id);
// The palette apply() last installed. Painted widgets read this every paint,
// so they follow a theme switch without being rebuilt.
const AppPalette &current();
const EditorPalette &currentEditor();
void setCurrentEditor(const QString &id);
const EditorPalette &editor(const QString &id);

// inline: one entity across TUs, and clang's -Wunused-const-variable stays
// quiet in TUs (and standalone-header parses) that never read them.
inline constexpr auto defaultApp = "black";
inline constexpr auto defaultEditor = "gruvbox";

// Stable per-connection accent so prod and dev are never confused — the same
// hash the web build uses, so a profile keeps its colour across both. Shared
// by the server tabs and the connections list; a divergence would silently
// desync their colours.
QColor connAccent(const QString &id);

// Embedded RobotoMono Nerd Font (the web build embeds the same files) so the
// app looks identical on a machine with no fonts installed.
QString loadFonts();
QString monoFamily();

// The screen whose logical DPI apply() sizes against — the one the main window
// is on, which is not always the primary: with a 150% primary and a 100%
// secondary, sizing everything against the primary leaves the window oversized
// after it is dragged across. Pass nullptr to fall back to the primary screen.
void setDpiScreen(QScreen *screen);

// Applies palette + stylesheet to the whole application.
void apply(const QString &appThemeId, int uiFontSize);

// The base font size apply() last installed, in px: the Preferences slider
// value after the logical-DPI correction described in theme.cpp. This — not
// the raw slider value — is what other sizes must be derived from.
//
// Widget text must NOT be sized with QWidget::setFont(): the global
// `QWidget { font-size: ...px }` rule in the stylesheet overrides it, so such
// calls are silently dropped. Size widget text through the sheet instead
// (#kpiValue, #kpiSub, #smallText below), which follows the slider by itself.
//
// This accessor is for painter-drawn text — charts, the graph canvas, the grid
// header — which the stylesheet cannot reach. Derive from it in *pixels*:
// deriving from QFont::pointSizeF() yields -1, because the base font is set
// with setPixelSize, and point sizes then vary with the screen DPI while every
// other size in the app is in pixels.
int uiFontSize();
// uiFontSize() * factor, clamped to at least minPx. For painter fonts.
int scaledPx(double factor, int minPx = 8);
// px through the same logical-DPI correction apply() gives the base font —
// for sizes that come from their own pref slider rather than uiFontSize()
// (the editor font, whose slider is separate from the UI one).
int dpiPx(int px);

} // namespace theme
