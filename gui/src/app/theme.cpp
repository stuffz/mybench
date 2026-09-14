#include "app/theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QScreen>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <cmath>

namespace
{

QString g_mono;
const AppPalette *g_current = nullptr;
const EditorPalette *g_currentEditor = nullptr;
// Mirrors the Preferences slider; the default matches main.cpp's first apply().
int g_uiFontSize = 13;

// 1.0 when Qt applies the display scaling itself (the usual case: logical DPI
// stays 96 and the device pixel ratio carries the scale), otherwise the factor
// px sizes have to be multiplied by to come out the right physical size.
QPointer<QScreen> g_dpiScreen;

double dpiScale()
{
    const QScreen *s = g_dpiScreen ? g_dpiScreen.data() : QGuiApplication::primaryScreen();
    if (!s)
    {
        return 1.0;
    }
    const double dpi = s->logicalDotsPerInchY();
    return dpi > 0 ? qBound(1.0, dpi / 96.0, 4.0) : 1.0;
}

// The size hierarchy, as multiples of the slider value. Kept here so the
// stylesheet and the painter-drawn text scale by the same factors.
constexpr double ScaleBig = 1.5;    // dashboard card values
constexpr double ScaleSmall = 0.85; // captions, legends, status strip
// One shared height for every one-line control (buttons, line edits, spin
// boxes, combos). Left to their own devices each widget type derives a
// slightly different height — the port spin box taller than the host field,
// combos and buttons shorter — which reads as misalignment on any form.
constexpr double ScaleControl = 1.7;

AppPalette blackPalette()
{
    // index.css `.dark`: oklch(0.145 0 0) → #0a0a0a, 0.985 → #fafafa,
    // 0.205 → #171717, 0.922 → #e5e5e5, 0.269 → #262626, 0.708 → #a1a1a1,
    // 0.556 → #737373, destructive oklch(0.704 0.191 22.216) → #ff6467.
    // border/input are white at 10%/15% composited over the background.
    AppPalette p;
    p.background = QColor("#0a0a0a");
    p.foreground = QColor("#fafafa");
    p.card = QColor("#171717");
    p.cardFg = QColor("#fafafa");
    p.primary = QColor("#e5e5e5");
    p.primaryFg = QColor("#171717");
    p.secondary = QColor("#262626");
    p.muted = QColor("#262626");
    p.mutedFg = QColor("#a1a1a1");
    p.accent = QColor("#262626");
    p.destructive = QColor("#ff6467");
    p.warning = QColor("#f59e0b");
    p.success = QColor("#10b981");
    p.info = QColor("#38bdf8");
    p.special = QColor("#a78bfa");
    p.border = QColor("#222222");
    p.input = QColor("#2f2f2f");
    p.ring = QColor("#737373");
    p.sidebar = QColor("#0a0a0a");
    p.sidebarBorder = QColor("#222222");
    return p;
}

AppPalette gruvboxPalette()
{
    AppPalette p;
    p.background = QColor("#282828");
    p.foreground = QColor("#ebdbb2");
    p.card = QColor("#3c3836");
    p.cardFg = QColor("#ebdbb2");
    p.primary = QColor("#fabd2f");
    p.primaryFg = QColor("#282828");
    p.secondary = QColor("#3c3836");
    p.muted = QColor("#3c3836");
    p.mutedFg = QColor("#a89984");
    p.accent = QColor("#504945");
    p.destructive = QColor("#fb4934");
    p.warning = QColor("#fabd2f");
    p.success = QColor("#b8bb26");
    p.info = QColor("#83a598");
    p.special = QColor("#d3869b");
    p.border = QColor("#504945");
    p.input = QColor("#504945");
    p.ring = QColor("#928374");
    p.sidebar = QColor("#3c3836");
    p.sidebarBorder = QColor("#504945");
    return p;
}

AppPalette nordPalette()
{
    AppPalette p;
    p.background = QColor("#2e3440");
    p.foreground = QColor("#eceff4");
    p.card = QColor("#3b4252");
    p.cardFg = QColor("#eceff4");
    p.primary = QColor("#88c0d0");
    p.primaryFg = QColor("#2e3440");
    p.secondary = QColor("#3b4252");
    p.muted = QColor("#3b4252");
    p.mutedFg = QColor("#94a3b8");
    p.accent = QColor("#434c5e");
    p.destructive = QColor("#bf616a");
    p.warning = QColor("#ebcb8b");
    p.success = QColor("#a3be8c");
    p.info = QColor("#81a1c1");
    p.special = QColor("#b48ead");
    p.border = QColor("#434c5e");
    p.input = QColor("#434c5e");
    p.ring = QColor("#4c566a");
    p.sidebar = QColor("#3b4252");
    p.sidebarBorder = QColor("#434c5e");
    return p;
}

AppPalette solarizedPalette()
{
    AppPalette p;
    p.background = QColor("#002b36");
    p.foreground = QColor("#93a1a1");
    p.card = QColor("#073642");
    p.cardFg = QColor("#93a1a1");
    p.primary = QColor("#268bd2");
    p.primaryFg = QColor("#fdf6e3");
    p.secondary = QColor("#073642");
    p.muted = QColor("#073642");
    p.mutedFg = QColor("#657b83");
    p.accent = QColor("#0f4b5b");
    p.destructive = QColor("#dc322f");
    p.warning = QColor("#b58900");
    p.success = QColor("#859900");
    p.info = QColor("#268bd2");
    p.special = QColor("#d33682");
    p.border = QColor("#0f4b5b");
    p.input = QColor("#0f4b5b");
    p.ring = QColor("#586e75");
    p.sidebar = QColor("#073642");
    p.sidebarBorder = QColor("#0f4b5b");
    return p;
}

EditorPalette gruvboxEditor()
{
    EditorPalette c;
    c.bg = QColor("#282828");
    c.fg = QColor("#ebdbb2");
    c.caret = QColor("#ebdbb2");
    c.sel = QColor("#504945");
    c.line = QColor("#3c3836");
    c.panel = QColor("#3c3836");
    c.comment = QColor("#928374");
    c.keyword = QColor("#fb4934");
    c.string = QColor("#b8bb26");
    c.number = QColor("#d3869b");
    c.func = QColor("#fabd2f");
    c.type = QColor("#83a598");
    return c;
}

EditorPalette oneDarkEditor()
{
    EditorPalette c;
    c.bg = QColor("#282c34");
    c.fg = QColor("#abb2bf");
    c.caret = QColor("#528bff");
    c.sel = QColor("#3E4451");
    c.line = QColor("#2c313a");
    c.panel = QColor("#21252b");
    c.comment = QColor("#5c6370");
    c.keyword = QColor("#c678dd");
    c.string = QColor("#98c379");
    c.number = QColor("#d19a66");
    c.func = QColor("#61afef");
    c.type = QColor("#e5c07b");
    return c;
}

EditorPalette nordEditor()
{
    EditorPalette c;
    c.bg = QColor("#2e3440");
    c.fg = QColor("#d8dee9");
    c.caret = QColor("#d8dee9");
    c.sel = QColor("#434c5e");
    c.line = QColor("#3b4252");
    c.panel = QColor("#3b4252");
    c.comment = QColor("#616e88");
    c.keyword = QColor("#81a1c1");
    c.string = QColor("#a3be8c");
    c.number = QColor("#b48ead");
    c.func = QColor("#88c0d0");
    c.type = QColor("#8fbcbb");
    return c;
}

EditorPalette solarizedEditor()
{
    EditorPalette c;
    c.bg = QColor("#002b36");
    c.fg = QColor("#93a1a1");
    c.caret = QColor("#93a1a1");
    c.sel = QColor("#073642");
    c.line = QColor("#073642");
    c.panel = QColor("#073642");
    c.comment = QColor("#586e75");
    c.keyword = QColor("#859900");
    c.string = QColor("#2aa198");
    c.number = QColor("#d33682");
    c.func = QColor("#b58900");
    c.type = QColor("#268bd2");
    return c;
}

// The stylesheet can only take an image as a url() — no tinting, and QtSvg
// ignores currentColor — so the dropdown chevron (QComboBox arrow, snippets
// menu indicator) is rendered from the icon SVG in the palette's colour and
// written to the cache dir, one pair per colour and size. The @2x sibling keeps Qt's
// per-screen DPI pick working exactly as the old baked PNG pair did.
void renderChevron(const QString &path, const QColor &colour, int px)
{
    QFile f(QStringLiteral(":/icons/chevron-down.svg"));
    if (!f.open(QIODevice::ReadOnly))
    {
        return;
    }
    QByteArray svg = f.readAll();
    svg.replace("currentColor", colour.name().toUtf8());
    QSvgRenderer renderer(svg);
    QImage img(QSize(px, px), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(0, 0, px, px));
    painter.end();
    img.save(path);
}

QString chevronPath(const QColor &colour, int px)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString base =
        dir + "/chevron-" + colour.name().mid(1) + "-" + QString::number(px) + ".png";
    if (!QFile::exists(base))
    {
        renderChevron(base, colour, px);
        QString two = base;
        two.replace(QStringLiteral(".png"), QStringLiteral("@2x.png"));
        renderChevron(two, colour, px * 2);
    }
    return base;
}

QString sheet(const AppPalette &p, int fs)
{
    const QString mono = theme::monoFamily();
    const QString bg = p.background.name();
    const QString fg = p.foreground.name();
    const QString card = p.card.name();
    const QString border = p.border.name();
    const QString input = p.input.name();
    const QString muted = p.muted.name();
    const QString mutedFg = p.mutedFg.name();
    const QString primary = p.primary.name();
    const QString primaryFg = p.primaryFg.name();
    const QString destructive = p.destructive.name();
    const QString ring = p.ring.name();

    // Content-box heights: qss adds padding and border on top, so a control
    // renders ctlh + 4px padding + 2px border tall. The sidebar's mode-toggle
    // row is one control plus its 12px layout margins; a tab must fill that
    // row minus the strip's 1px separator and its own 2px selection underline
    // for the two hairlines to meet on the same y.
    const int ctlh = qMax(16, int(std::lround(fs * ScaleControl)));
    const int tabh = ctlh + 6 + 12 - 1 - 2;
    // The dropdown chevron follows the font slider, 1:1 with the font size.
    const int chev = qMax(8, fs);

    // Mirrors the tailwind/shadcn look of the web UI: 8px radii, 1px borders
    // in --border, muted hover fills, monospace everywhere.
    return QString(R"(
* { outline: none; }
/* Only the surfaces that own a background get one. A blanket rule here paints
   every plain container — layout rows, slider wrappers, label groups — the
   window colour, which reads as black holes punched in a dialog's card. Bare
   QWidgets are transparent in Qt, which is what we want: they inherit
   whatever they sit on. */
QWidget { color: %FG%; font-family: "%MONO%"; font-size: %FS%px; }
QMainWindow, QMainWindow > QWidget { background: %BG%; }
QWidget#Header { background: %BG%; }
QWidget#Card, QDialog { background: %CARD%; }
QLabel { background: transparent; }
QLabel[muted="true"] { color: %MUTEDFG%; }
QLabel[tone="warning"] { color: %WARNING%; }
QLabel[tone="success"] { color: %SUCCESS%; }
QLabel[tone="destructive"] { color: %DESTRUCTIVE%; }

QPushButton, QToolButton#SnippetsBtn {
    background: transparent; color: %FG%;
    border: 1px solid %INPUT%; border-radius: 8px;
    padding: 2px 10px;
}
QPushButton:hover, QToolButton#SnippetsBtn:hover { background: %MUTED%; }
/* Named rather than a bare QToolButton rule: QLineEdit's clear button is a
   QToolButton subclass and must keep its native compact look. */
QToolButton#SnippetsBtn { padding-right: 24px; }
QToolButton#SnippetsBtn::menu-indicator {
    image: url("%CHEVRON%"); width: %CHEVPX%px; height: %CHEVPX%px;
    subcontrol-origin: padding; subcontrol-position: center right; right: 6px;
}
QPushButton:disabled { color: %MUTEDFG%; border-color: %BORDER%; }
/* Filled variants keep a transparent border so they stay the same height as
   the outlined default; none of them is bold — the web buttons are not. */
QPushButton[variant="primary"] { background: %PRIMARY%; color: %PRIMARYFG%; border: 1px solid transparent; }
QPushButton[variant="primary"]:hover { background: %PRIMARY%; }
QPushButton[variant="primary"]:disabled { background: %MUTED%; color: %MUTEDFG%; }
QPushButton[variant="ghost"] { border: 1px solid transparent; color: %MUTEDFG%; }
QPushButton[variant="ghost"]:hover { background: %MUTED%; color: %FG%; }
/* Fixed-width −/+ steppers: the base 10px side padding leaves a ~26px button
   no room for its glyph, which clips to a sliver. */
QPushButton#Stepper { padding: 0; }
QPushButton[variant="destructive"] { background: %DESTRUCTIVE%; color: %BG%; border: 1px solid transparent; }
QPushButton[variant="toggle"] { border: 1px solid transparent; border-radius: 6px; color: %MUTEDFG%; padding: 2px 8px; }
QPushButton[variant="toggle"]:hover { background: %MUTED%; color: %FG%; }
QPushButton[variant="toggle"]:checked { background: %MUTED%; color: %FG%; }

QLineEdit, QSpinBox, QPlainTextEdit#Plain {
    background: transparent; border: 1px solid %INPUT%; border-radius: 6px;
    padding: 2px 6px; selection-background-color: %RING%;
}
QLineEdit:focus, QSpinBox:focus { border-color: %RING%; }
/* The shared control height (see ScaleControl). min and max together, because
   each of these widgets pads its sizeHint by a different amount. */
QLineEdit, QSpinBox, QComboBox, QPushButton, QToolButton#SnippetsBtn {
    min-height: %CTLH%px; max-height: %CTLH%px;
}

QComboBox { background: transparent; border: 1px solid %INPUT%; border-radius: 6px; padding: 2px 6px; }
/* No ::drop-down override: overriding it without supplying an arrow image
   left the indicator invisible, so combo boxes read as plain text fields.
   The style's own arrow follows QPalette::ButtonText. */
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right;
                       border: none; width: 18px; }
QComboBox::down-arrow { image: url("%CHEVRON%"); width: %CHEVPX%px; height: %CHEVPX%px; }
QComboBox QAbstractItemView {
    background: %CARD%; border: 1px solid %BORDER%; selection-background-color: %MUTED%;
    selection-color: %FG%; padding: 2px;
}

QCheckBox, QRadioButton, QSlider, QGroupBox { background: transparent; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid %INPUT%; border-radius: 4px; }
QCheckBox::indicator:checked { background: %PRIMARY%; border-color: %PRIMARY%; }

/* The pane's top border is the full-width rule under the tab strip — the tab
   bar itself only spans its tabs, so a border on it would stop short. It only
   paints with documentMode off: QTabWidget::paintEvent skips the pane frame
   entirely in document mode. */
QTabWidget::pane { border: none; border-top: 1px solid %BORDER%; }
QTabWidget::tab-bar { left: 0; }
QTabBar { qproperty-drawBase: 0; background: %BG%; }
/* Tab height is derived, not chosen: the strip's separator must land on the
   same y as the sidebar mode-toggle's, so tab + selection underline must equal
   that row (toggle button + its 12px margins) minus the 1px separator. */
QTabBar::tab {
    background: transparent; color: %MUTEDFG%;
    border: none; border-bottom: 2px solid transparent;
    border-right: 1px solid %BORDER%;
    padding: 0 12px; margin: 0; min-width: 40px; height: %TABH%px;
}
QTabBar::tab:hover { background: %MUTED%; color: %FG%; }
QTabBar::tab:selected { color: %FG%; border-bottom: 2px solid %PRIMARY%; }
/* Query tabs (named in mainwindow.cpp) end in a close button that already
   carries the glyph-to-divider gap inside itself (styleTabCloseButton), and
   the style adds its own spacing between text and button — so the tab's own
   right padding nearly vanishes or the text-to-× gap doubles up. */
QTabBar#QueryTabs::tab { padding-right: 2px; }

QTreeView, QTableView, QListView {
    background: %BG%; border: none; alternate-background-color: %BG%;
    selection-background-color: %MUTED%; selection-color: %FG%;
}
QTreeView::item, QListView::item { padding: 1px 2px; border: none; }
/* Admin tables (server info, processlist, users, …) are QTableWidgets; the
   results grid is a bare QTableView with its own delegate and stays as is.
   Without this, cell text sits flush against the column lines. */
QTableWidget::item { padding: 2px 8px; }
/* The sidebar's Administration page list, matching the web build: the list
   inset 4px, each row an 8px-padded pill that rounds on hover/selection. The
   explicit height matters: without it the platform style's own list-item
   metric wins, which on Windows 11 is a ~40px Explorer-style row. */
QListWidget#AdminList { padding: 4px; }
QListWidget#AdminList::item { padding: 4px 8px; border-radius: 6px; height: %CTLH%px; }

/* Rows that end in a hairline on the web: the sidebar's mode toggle and
   filter, and the title/toolbar row of the admin panels. The widgets carrying
   these names must set WA_StyledBackground or the border never paints. */
QWidget#SideModes, QWidget#SideFilter, QWidget#PanelHeader { border-bottom: 1px solid %BORDER%; }
QTreeView::item:hover, QTableView::item:hover, QListView::item:hover { background: %MUTED%; }
QHeaderView { background: %BG%; border: none; }
QHeaderView::section {
    background: %BG%; color: %FG%; border: none;
    border-bottom: 1px solid %BORDER%; border-right: 1px solid %BORDER%;
    padding: 2px 8px; /* same left inset as QTableWidget::item, so columns line up */
}
QHeaderView::section:hover { background: %MUTED%; }
QTableCornerButton::section { background: %BG%; border: none; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle { background: %INPUT%; border-radius: 5px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: %RING%; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QMenu { background: %CARD%; border: 1px solid %BORDER%; border-radius: 8px; padding: 4px; }
QMenu::item { padding: 4px 18px 4px 10px; border-radius: 6px; color: %FG%; }
/* primary/primaryFg, not muted: on light themes muted sits so close to the
   card colour that hover read as nothing happening. */
QMenu::item:selected { background: %PRIMARY%; color: %PRIMARYFG%; }
QMenu::item:disabled { color: %MUTEDFG%; }
QMenu::separator { height: 1px; background: %BORDER%; margin: 4px 6px; }

QSplitter::handle { background: %BORDER%; }
QSplitter::handle:hover { background: %RING%; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

QToolTip { background: %CARD%; color: %FG%; border: 1px solid %BORDER%; padding: 3px 5px; }

QSlider::groove:horizontal { height: 3px; background: %INPUT%; border-radius: 2px; }
QSlider::handle:horizontal { background: %FG%; width: 11px; height: 11px; margin: -4px 0; border-radius: 6px; }
QSlider::sub-page:horizontal { background: %RING%; border-radius: 2px; }

QStatusBar { border-top: 1px solid %BORDER%; color: %MUTEDFG%; }
QStatusBar::item { border: none; }

/* The size hierarchy lives here, not in setFont(): the QWidget rule above
   matches every widget and overrides any font set programmatically. All three
   are multiples of the slider value, so they follow it. */
QLabel#kpiValue { font-size: %FSBIG%px; font-weight: 600; }
QLabel#kpiSub   { font-size: %FSSMALL%px; }
QLabel#smallText { font-size: %FSSMALL%px; }
)")
        // Every token is delimited on both sides, so no token is a prefix of
        // another and this chain is order-independent.
        .replace("%BG%", bg)
        .replace("%FG%", fg)
        .replace("%CARD%", card)
        .replace("%BORDER%", border)
        .replace("%INPUT%", input)
        .replace("%MUTEDFG%", mutedFg)
        .replace("%MUTED%", muted)
        .replace("%PRIMARYFG%", primaryFg)
        .replace("%PRIMARY%", primary)
        .replace("%DESTRUCTIVE%", destructive)
        .replace("%WARNING%", p.warning.name())
        .replace("%SUCCESS%", p.success.name())
        .replace("%RING%", ring)
        .replace("%CHEVRON%", chevronPath(p.mutedFg, chev))
        .replace("%CHEVPX%", QString::number(chev))
        .replace("%MONO%", mono)
        .replace("%FSBIG%", QString::number(qMax(8, int(std::lround(fs * ScaleBig)))))
        .replace("%FSSMALL%", QString::number(qMax(8, int(std::lround(fs * ScaleSmall)))))
        .replace("%CTLH%", QString::number(ctlh))
        .replace("%TABH%", QString::number(tabh))
        .replace("%FS%", QString::number(fs));
}

} // namespace

namespace theme
{

void setDpiScreen(QScreen *screen)
{
    g_dpiScreen = screen;
}

int uiFontSize()
{
    return g_uiFontSize;
}

int scaledPx(double factor, int minPx)
{
    return qMax(minPx, int(std::lround(g_uiFontSize * factor)));
}

int dpiPx(int px)
{
    return qMax(1, int(std::lround(px * dpiScale())));
}

QColor connAccent(const QString &id)
{
    int h = 0;
    for (const QChar &ch : id)
    {
        h = (h * 31 + ch.unicode()) % 360;
    }
    return QColor::fromHsl(h, 153, 128);
}

const QVector<AppTheme> &appThemes()
{
    static const QVector<AppTheme> v{
        {"black", "Black", blackPalette()},
        {"gruvbox", "Gruvbox", gruvboxPalette()},
        {"nord", "Nord", nordPalette()},
        {"solarized", "Solarized", solarizedPalette()},
    };
    return v;
}

const QVector<EditorTheme> &editorThemes()
{
    static const QVector<EditorTheme> v{
        {"gruvbox", "Gruvbox", gruvboxEditor()},
        {"onedark", "One Dark", oneDarkEditor()},
        {"nord", "Nord", nordEditor()},
        {"solarized", "Solarized", solarizedEditor()},
    };
    return v;
}

const AppPalette &app(const QString &id)
{
    for (const auto &t : appThemes())
    {
        if (t.id == id)
        {
            return t.p;
        }
    }
    // The named default, not whichever theme happens to be listed first:
    // reordering the table must not change what an unknown id resolves to.
    for (const auto &t : appThemes())
    {
        if (t.id == QLatin1String(defaultApp))
        {
            return t.p;
        }
    }
    return appThemes().first().p;
}

const EditorPalette &editor(const QString &id)
{
    for (const auto &t : editorThemes())
    {
        if (t.id == id)
        {
            return t.p;
        }
    }
    // The named default, not whichever theme happens to be listed first:
    // reordering the table must not change what an unknown id resolves to.
    for (const auto &t : editorThemes())
    {
        if (t.id == QLatin1String(defaultEditor))
        {
            return t.p;
        }
    }
    return editorThemes().first().p;
}

QString loadFonts()
{
    const QStringList files{
        ":/fonts/RobotoMonoNerdFont-Regular.ttf",
        ":/fonts/RobotoMonoNerdFont-Medium.ttf",
        ":/fonts/RobotoMonoNerdFont-Bold.ttf",
        ":/fonts/RobotoMonoNerdFont-Italic.ttf",
    };
    for (const QString &f : files)
    {
        const int id = QFontDatabase::addApplicationFont(f);
        if (id >= 0 && g_mono.isEmpty())
        {
            const auto fams = QFontDatabase::applicationFontFamilies(id);
            if (!fams.isEmpty())
            {
                g_mono = fams.first();
            }
        }
    }
    if (g_mono.isEmpty())
    {
        g_mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    }
    return g_mono;
}

QString monoFamily()
{
    if (g_mono.isEmpty())
    {
        loadFonts();
    }
    return g_mono;
}

Notifier *notifier()
{
    static Notifier *n = new Notifier;
    return n;
}

const AppPalette &current()
{
    if (!g_current)
    {
        g_current = &app(defaultApp);
    }
    return *g_current;
}

const EditorPalette &currentEditor()
{
    if (!g_currentEditor)
    {
        g_currentEditor = &editor(defaultEditor);
    }
    return *g_currentEditor;
}

void setCurrentEditor(const QString &id)
{
    g_currentEditor = &editor(id);
}

void apply(const QString &appThemeId, int uiFontSize)
{
    const AppPalette &p = app(appThemeId);
    g_current = &p;

    // QPalette first: painted widgets (the grid, the graph) and native bits
    // read it directly, the stylesheet only covers styled controls.
    QPalette q;
    q.setColor(QPalette::Window, p.background);
    q.setColor(QPalette::WindowText, p.foreground);
    q.setColor(QPalette::Base, p.background);
    q.setColor(QPalette::AlternateBase, p.card);
    q.setColor(QPalette::Text, p.foreground);
    q.setColor(QPalette::Button, p.background);
    q.setColor(QPalette::ButtonText, p.foreground);
    q.setColor(QPalette::Highlight, p.muted);
    q.setColor(QPalette::HighlightedText, p.foreground);
    q.setColor(QPalette::ToolTipBase, p.card);
    q.setColor(QPalette::ToolTipText, p.foreground);
    q.setColor(QPalette::PlaceholderText, p.mutedFg);
    q.setColor(QPalette::Disabled, QPalette::Text, p.mutedFg);
    q.setColor(QPalette::Disabled, QPalette::ButtonText, p.mutedFg);
    qApp->setPalette(q);

    // Everything here is sized in px, which is only physically correct while
    // Qt carries the display scaling in the device pixel ratio (logical DPI
    // stays 96). It does not always: with QT_ENABLE_HIGHDPI_SCALING=0, and on
    // X11 setups that scale through the font DPI instead, Qt reports dpr 1 and
    // a logical DPI of 96*scale, so raw px would come out scale-times too
    // small. Fold that ratio in and the app is the same physical size either
    // way — and matches other platforms at the same display scale.
    g_uiFontSize = qMax(1, int(std::lround(uiFontSize * dpiScale())));

    QFont f(monoFamily());
    // px, not pt: the stylesheet sizes in px so the two must agree.
    f.setPixelSize(g_uiFontSize);
    f.setStyleHint(QFont::Monospace);
    qApp->setFont(f);

    qApp->setStyleSheet(sheet(p, g_uiFontSize));
    notifier()->notifyChanged();
}

} // namespace theme
