#pragma once
// The small widget makers the dialogs, the shell chrome and the views share:
// the themed QLabel variants and the rule that separates stacked rows.
#include <QFont>
#include <QString>

class QFrame;
class QLabel;

// The dimmed label the sheet paints with the muted foreground.
QLabel *mutedLabel(const QString &text = {});

// Muted, one step down in size. The size comes from the #smallText rule rather
// than setFont, which the sheet's global QWidget rule would override — and
// this way the text follows the prefs slider.
QLabel *smallLabel(const QString &text = {});

// Weight is the one font property a label may still set for itself: the sheet
// fixes font-size only, so a heavier label survives a theme or slider change.
QLabel *weightedLabel(const QString &text, QFont::Weight weight);

// A 1px themed rule between stacked rows. The colour is baked at construction,
// so a caller that outlives a theme switch has to rebuild its rules.
QFrame *hairline();

// The vertical twin, for separating control groups inside a toolbar row.
// Same baked-colour caveat as hairline().
QFrame *vhairline();
