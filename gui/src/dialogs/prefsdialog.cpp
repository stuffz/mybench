#include "dialogs/prefsdialog.h"

#include "app/api.h"
#include "app/icons.h"
#include "app/theme.h"
#include "ui/fmt.h"
#include "ui/switchbox.h"
#include "ui/widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace
{

// Label + info glyph, with the tip as rich text so long descriptions wrap
// instead of running off the screen (the web used a 280px wrapping variant).
QWidget *tippedLabel(const QString &text, const QString &tip, const QColor &glyphColour)
{
    auto *row = new QWidget;
    auto *l = new QHBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    auto *name = mutedLabel(text);
    auto *icon = new QLabel;
    icon->setPixmap(icons::pixmap("info", glyphColour, 12));
    const QString rich =
        QString("<p style='width: 320px; margin: 0;'>%1</p>").arg(tip.toHtmlEscaped());
    for (QWidget *w :
         {static_cast<QWidget *>(row), static_cast<QWidget *>(name), static_cast<QWidget *>(icon)})
    {
        w->setToolTip(rich);
    }
    l->addWidget(name);
    l->addWidget(icon);
    l->addStretch();
    return row;
}

QSpinBox *intBox(int lo, int hi, int value)
{
    auto *s = new QSpinBox;
    s->setRange(lo, hi);
    s->setValue(value);
    // No group separator: the value is a plain limit, and a comma in a field
    // you type into reads like a syntax error.
    s->setGroupSeparatorShown(false);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setMaximumWidth(140);
    return s;
}

// Flash a copy button's label, then restore it.
void flashCopied(QPushButton *btn, const QString &original)
{
    btn->setText(QObject::tr("Copied"));
    QTimer::singleShot(1500, btn, [btn, original]() { btn->setText(original); });
}

} // namespace

PrefsDialog::PrefsDialog(const QJsonObject &prefs, QWidget *parent)
    : QDialog(parent), m_prefs(prefs)
{
    setWindowTitle(tr("Preferences"));
    setMinimumWidth(theme::scaledPx(38.0));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 16);
    root->setSpacing(6);

    // In-dialog header, like the web DialogHeader (and the About box).
    auto *title = new QLabel(tr("Preferences"));
    title->setObjectName("kpiValue"); // sized by the sheet; setFont would be overridden
    root->addWidget(title);
    auto *sub = mutedLabel(tr("Applied immediately, saved with your workspace."));
    root->addWidget(sub);
    root->addSpacing(8);

    auto *form = new QFormLayout;
    m_form = form;
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(10);

    m_appTheme = new QComboBox;
    for (const AppTheme &t : theme::appThemes())
    {
        m_appTheme->addItem(t.label, t.id);
    }
    {
        const int ix = m_appTheme->findData(prefs.value("appTheme").toString(theme::defaultApp));
        m_appTheme->setCurrentIndex(qMax(0, ix));
    }
    form->addRow(mutedLabel(tr("App Theme")), m_appTheme);

    m_editorTheme = new QComboBox;
    for (const EditorTheme &t : theme::editorThemes())
    {
        m_editorTheme->addItem(t.label, t.id);
    }
    {
        const int ix =
            m_editorTheme->findData(prefs.value("editorTheme").toString(theme::defaultEditor));
        m_editorTheme->setCurrentIndex(qMax(0, ix));
    }
    form->addRow(mutedLabel(tr("Editor Theme")), m_editorTheme);

    auto slider = [&form](
                      const QString &label, int lo, int hi, int value, QSlider **out, QLabel **lbl,
                      const QString &suffix
                  )
    {
        auto *row = new QWidget;
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto *s = new QSlider(Qt::Horizontal);
        s->setRange(lo, hi);
        s->setValue(value);
        auto *l = new QLabel(QString::number(value) + suffix);
        l->setMinimumWidth(44);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rl->addWidget(s, 1);
        rl->addWidget(l);
        form->addRow(mutedLabel(label), row);
        *out = s;
        *lbl = l;
    };
    slider(
        tr("UI Font Size"), 10, 22, prefs.value("uiFontSize").toInt(13), &m_uiFontSize,
        &m_uiFontSizeLbl, "px"
    );
    slider(
        tr("Editor Font Size"), 9, 24, prefs.value("editorFontSize").toInt(13), &m_editorFontSize,
        &m_editorFontSizeLbl, "px"
    );
    slider(tr("Tab Size"), 2, 8, prefs.value("tabSize").toInt(4), &m_tabSize, &m_tabSizeLbl, "");

    const QColor glyph = theme::current().mutedFg;

    m_hideDefaultDBs = new SwitchBox;
    m_hideDefaultDBs->setChecked(prefs.value("hideDefaultDBs").toBool(true));
    form->addRow(
        tippedLabel(
            tr("Hide Default Databases"),
            tr("Hides information_schema, performance_schema, mysql and sys in "
               "the schema tree"),
            glyph
        ),
        m_hideDefaultDBs
    );

    m_historyKeep = intBox(100, 1000000, prefs.value("historyKeep").toInt(10000));
    m_sentHistoryKeep = m_historyKeep->value(); // the backend already has the stored value
    auto *historyLabel = tippedLabel(
        tr("History Per Server"),
        tr("Newest executed statements kept per server; older entries are pruned."), glyph
    );
    form->addRow(historyLabel, m_historyKeep);
    // The web appended what is actually stored right now; fetched on open.
    api()->call(
        "query", "HistoryStats", {}, this,
        [historyLabel](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                return;
            }
            const QJsonObject o = res.toObject();
            const QString extra = tr(" Currently storing %L1 statements (%2).")
                                      .arg(qint64(o.value("count").toDouble()))
                                      .arg(fmtBytes(qint64(o.value("bytes").toDouble())));
            const QString base =
                tr("Newest executed statements kept per server; older entries are pruned.");
            const QString rich = QString("<p style='width: 320px; margin: 0;'>%1</p>")
                                     .arg((base + extra).toHtmlEscaped());
            historyLabel->setToolTip(rich);
            for (QObject *child : historyLabel->children())
            {
                if (auto *w = qobject_cast<QWidget *>(child))
                {
                    w->setToolTip(rich);
                }
            }
        }
    );

    m_copySep = new QComboBox;
    m_copySep->addItem(tr("Tab"), QStringLiteral("\t"));
    m_copySep->addItem(tr("Comma (,)"), QStringLiteral(","));
    m_copySep->addItem(tr("Semicolon (;)"), QStringLiteral(";"));
    m_copySep->addItem(tr("Pipe (|)"), QStringLiteral("|"));
    {
        const int ix = m_copySep->findData(prefs.value("copySeparator").toString("\t"));
        m_copySep->setCurrentIndex(qMax(0, ix));
    }
    form->addRow(
        tippedLabel(
            tr("Copy Separator"),
            tr("What separates cells when copying rows or selections from a "
               "result grid."),
            glyph
        ),
        m_copySep
    );

    m_defaultRows = intBox(0, 100000000, prefs.value("defaultRowLimit").toInt(50000));
    m_defaultRows->setSpecialValueText(tr("Unlimited")); // shown at 0
    form->addRow(
        tippedLabel(
            tr("Default Row Limit"),
            tr("Rows a run fetches into memory before truncating the result — "
               "a query tab's Limit dropdown can pick a smaller per-tab value, "
               "and 0 means unlimited. Only fetching is limited; the SQL is "
               "never modified and the server still runs the full query."),
            glyph
        ),
        m_defaultRows
    );

    // --- MCP -----------------------------------------------------------
    m_mcp = new SwitchBox;
    form->addRow(
        tippedLabel(
            tr("MCP Server"),
            tr("Read-only Model Context Protocol endpoint for AI agents: schema "
               "introspection plus gated SELECT/SHOW/EXPLAIN through your OPEN "
               "connections (tunnels included, credentials never exposed). "
               "Loopback-only with bearer-token auth; every agent query is "
               "recorded in Query History."),
            glyph
        ),
        m_mcp
    );

    // Port, endpoint and error rows exist only while the listener runs —
    // exactly the rows the web dialog reveals behind the switch.
    m_mcpPort = intBox(1, 65535, 1);
    m_mcpPortRow = form->rowCount();
    form->addRow(mutedLabel(tr("MCP Port")), m_mcpPort);

    auto *endpoint = new QWidget;
    {
        auto *el = new QHBoxLayout(endpoint);
        el->setContentsMargins(0, 0, 0, 0);
        el->setSpacing(8);
        m_mcpUrl = smallLabel();
        m_mcpUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        // Ignored: the URL yields to the buttons instead of widening the row.
        m_mcpUrl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        el->addWidget(m_mcpUrl, 1);
        auto *copyUrl = new QPushButton(tr("Copy URL"));
        connect(
            copyUrl, &QPushButton::clicked, this,
            [this, copyUrl]()
            {
                QApplication::clipboard()->setText(m_mcpUrl->text());
                flashCopied(copyUrl, tr("Copy URL"));
            }
        );
        el->addWidget(copyUrl);
        auto *copyToken = new QPushButton(tr("Copy Token"));
        connect(
            copyToken, &QPushButton::clicked, this,
            [this, copyToken]()
            {
                QApplication::clipboard()->setText(m_mcpToken);
                flashCopied(copyToken, tr("Copy Token"));
            }
        );
        el->addWidget(copyToken);
        auto *regen = new QPushButton(tr("Regenerate"));
        regen->setProperty("variant", "ghost");
        regen->setToolTip(tr("Invalidates the current token"));
        connect(
            regen, &QPushButton::clicked, this,
            [this]()
            {
                api()->call(
                    "mcp", "RegenerateToken", {}, this,
                    [this](const QJsonValue &res, const QString &err)
                    {
                        if (err.isEmpty())
                        {
                            applyMcpStatus(res.toObject());
                        }
                    }
                );
            }
        );
        el->addWidget(regen);
    }
    m_mcpEndpointRow = form->rowCount();
    form->addRow(mutedLabel(tr("MCP Endpoint")), endpoint);

    m_mcpError = new QLabel;
    m_mcpError->setWordWrap(true);
    m_mcpError->setProperty("tone", "destructive");
    m_mcpErrorRow = form->rowCount();
    form->addRow(QString(), m_mcpError);

    for (int row : {m_mcpPortRow, m_mcpEndpointRow, m_mcpErrorRow})
    {
        form->setRowVisible(row, false);
    }

    api()->call(
        "mcp", "Status", {}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (err.isEmpty())
            {
                applyMcpStatus(res.toObject());
            }
        }
    );
    connect(
        m_mcp, &QCheckBox::toggled, this, [this](bool on) { configureMcp(on, m_mcpPort->value()); }
    );
    // Blur or Enter applies a port change, like the web input — but only a
    // real change: editingFinished also fires on plain focus loss.
    connect(
        m_mcpPort, &QSpinBox::editingFinished, this,
        [this]()
        {
            if (m_mcp->isChecked() && m_mcpPort->value() != m_mcpAppliedPort)
            {
                configureMcp(true, m_mcpPort->value());
            }
        }
    );

    root->addLayout(form);
    // Surplus height pools here instead of being spread between the rows.
    root->addStretch(1);

    // Applying a smaller UI font shrinks every row but not the window, which
    // keeps the size the bigger font forced on it — refit once the new
    // stylesheet has settled (deferred: apply() runs inside this signal).
    connect(
        theme::notifier(), &Notifier::changed, this,
        [this]() { QTimer::singleShot(0, this, [this]() { adjustSize(); }); }
    );

    connect(m_appTheme, &QComboBox::currentIndexChanged, this, &PrefsDialog::emitPrefs);
    connect(m_editorTheme, &QComboBox::currentIndexChanged, this, &PrefsDialog::emitPrefs);
    connect(m_copySep, &QComboBox::currentIndexChanged, this, &PrefsDialog::emitPrefs);

    // Font sizes re-lay out this dialog as well as the app, so applying on
    // every valueChanged makes the handle jump away from the pointer. The
    // readout follows the drag; the apply waits for the release, with a timer
    // as the fallback for keyboard and mouse-wheel changes, which never
    // produce one.
    m_applyTimer = new QTimer(this);
    m_applyTimer->setSingleShot(true);
    m_applyTimer->setInterval(350);
    connect(m_applyTimer, &QTimer::timeout, this, &PrefsDialog::emitPrefs);
    for (QSlider *s : {m_uiFontSize, m_editorFontSize, m_tabSize})
    {
        connect(
            s, &QSlider::valueChanged, this,
            [this, s]()
            {
                updateSizeLabels();
                if (s->isSliderDown())
                {
                    m_applyTimer->stop(); // the release will apply it
                }
                else
                {
                    m_applyTimer->start();
                }
            }
        );
        connect(
            s, &QSlider::sliderReleased, this,
            [this]()
            {
                m_applyTimer->stop();
                emitPrefs();
            }
        );
    }

    connect(m_hideDefaultDBs, &QCheckBox::toggled, this, &PrefsDialog::emitPrefs);
    for (QSpinBox *s : {m_historyKeep, m_defaultRows})
    {
        connect(s, &QSpinBox::valueChanged, this, &PrefsDialog::emitPrefs);
    }
}

void PrefsDialog::applyMcpStatus(const QJsonObject &status)
{
    const bool on = status.value("enabled").toBool();
    {
        // Reflecting the current state must not echo a Configure back — that
        // could bounce the MCP listener just for opening this dialog.
        const QSignalBlocker block(m_mcp);
        m_mcp->setChecked(on);
    }
    {
        const QSignalBlocker block(m_mcpPort);
        m_mcpPort->setValue(status.value("port").toInt());
    }
    m_mcpAppliedPort = m_mcpPort->value();
    m_mcpToken = status.value("token").toString();
    m_mcpUrl->setText(status.value("url").toString());
    m_mcpUrl->setToolTip(m_mcpUrl->text());

    const QString mcpErr = status.value("error").toString();
    m_mcpError->setText(mcpErr.isEmpty() ? QString() : tr("MCP: %1").arg(mcpErr));
    m_form->setRowVisible(m_mcpPortRow, on);
    m_form->setRowVisible(m_mcpEndpointRow, on);
    m_form->setRowVisible(m_mcpErrorRow, !mcpErr.isEmpty());
    adjustSize();
}

void PrefsDialog::configureMcp(bool enabled, int port)
{
    api()->call(
        "mcp", "Configure", {enabled, port}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                m_mcpError->setText(err);
                m_form->setRowVisible(m_mcpErrorRow, true);
                return;
            }
            applyMcpStatus(res.toObject());
        }
    );
}

void PrefsDialog::updateSizeLabels()
{
    m_uiFontSizeLbl->setText(QString::number(m_uiFontSize->value()) + "px");
    m_editorFontSizeLbl->setText(QString::number(m_editorFontSize->value()) + "px");
    m_tabSizeLbl->setText(QString::number(m_tabSize->value()));
}

void PrefsDialog::emitPrefs()
{
    updateSizeLabels();

    QJsonObject p = m_prefs;
    p.insert("appTheme", m_appTheme->currentData().toString());
    p.insert("editorTheme", m_editorTheme->currentData().toString());
    p.insert("uiFontSize", m_uiFontSize->value());
    p.insert("editorFontSize", m_editorFontSize->value());
    p.insert("tabSize", m_tabSize->value());
    p.insert("hideDefaultDBs", m_hideDefaultDBs->isChecked());
    p.insert("historyKeep", m_historyKeep->value());
    p.insert("copySeparator", m_copySep->currentData().toString());
    p.insert("defaultRowLimit", m_defaultRows->value());
    m_prefs = p;

    // History retention lives backend-side; the blob is opaque to Go, so the
    // preference is pushed explicitly — but only when it changed, not on
    // every tick of an unrelated font-size drag.
    if (m_historyKeep->value() != m_sentHistoryKeep)
    {
        m_sentHistoryKeep = m_historyKeep->value();
        api()->post("query", "SetHistoryLimit", {m_sentHistoryKeep});
    }
    emit prefsChanged(p);
}
