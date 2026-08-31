#include "dialogs/importrowsdialog.h"

#include "app/api.h"
#include "app/theme.h"
#include "ui/widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

ImportRowsDialog::ImportRowsDialog(
    const QString &connID, const QString &schema, const QString &table, QWidget *parent
)
    : QDialog(parent), m_connID(connID), m_schema(schema), m_table(table)
{
    setWindowTitle(tr("Import Rows — %1.%2").arg(schema, table));
    // Sized in font units like the other dialogs (prefs, about), so the
    // dialog follows the prefs slider and DPI. A grid's own sizeHint is
    // tiny, so give it a real opening size, not only a floor.
    setMinimumSize(theme::scaledPx(48.0), theme::scaledPx(30.0));
    resize(theme::scaledPx(70.0), theme::scaledPx(46.0));

    auto *root = new QVBoxLayout(this);

    auto *options = new QHBoxLayout;
    m_csvBtn = new QPushButton(tr("Load CSV…"));
    m_sepCombo = new QComboBox;
    m_sepCombo->addItem(tr("Comma"), ",");
    m_sepCombo->addItem(tr("Semicolon"), ";");
    m_sepCombo->addItem(tr("Tab"), "\t");
    m_sepCombo->addItem(tr("Pipe"), "|");
    m_sepCombo->setEnabled(false); // meaningful only once a file is loaded
    m_headerCheck = new QCheckBox(tr("Header Row"));
    m_headerCheck->setEnabled(false);
    m_nullCheck = new QCheckBox(tr("Empty Means NULL"));
    m_nullCheck->setChecked(true);
    m_nullCheck->setToolTip(tr("Unchecked, empty cells insert '' instead of NULL"));
    options->addWidget(m_csvBtn);
    options->addWidget(m_sepCombo);
    options->addWidget(m_headerCheck);
    options->addStretch(1);
    options->addWidget(m_nullCheck);
    root->addLayout(options);

    m_grid = new QTableWidget;
    m_grid->setEditTriggers(
        QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
        QAbstractItemView::AnyKeyPressed
    );
    m_grid->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_grid->horizontalHeader()->setStretchLastSection(true);
    m_grid->verticalHeader()->setDefaultSectionSize(theme::scaledPx(1.8));
    root->addWidget(m_grid, 1);

    m_hintLbl =
        mutedLabel(tr("Columns left entirely empty are omitted — the server default applies."));
    m_hintLbl->setWordWrap(true);
    root->addWidget(m_hintLbl);
    m_errorLbl = new QLabel;
    m_errorLbl->setWordWrap(true);
    m_errorLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_errorLbl->setStyleSheet(
        QString("QLabel { color: %1; }").arg(theme::current().destructive.name())
    );
    m_errorLbl->setVisible(false);
    root->addWidget(m_errorLbl);

    auto *bottom = new QHBoxLayout;
    m_addBtn = new QPushButton(tr("Add Row"));
    m_removeBtn = new QPushButton(tr("Remove Selected"));
    m_removeBtn->setProperty("variant", "ghost");
    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_insertBtn = box->addButton(tr("Insert"), QDialogButtonBox::AcceptRole);
    m_insertBtn->setProperty("variant", "primary");
    bottom->addWidget(m_addBtn);
    bottom->addWidget(m_removeBtn);
    bottom->addStretch(1);
    bottom->addWidget(box);
    root->addLayout(bottom);

    connect(m_csvBtn, &QPushButton::clicked, this, &ImportRowsDialog::chooseCsv);
    connect(m_sepCombo, &QComboBox::currentIndexChanged, this, [this]() { requestCsv(false); });
    connect(m_headerCheck, &QCheckBox::toggled, this, [this]() { requestCsv(false); });
    connect(m_addBtn, &QPushButton::clicked, this, &ImportRowsDialog::addRow);
    connect(m_removeBtn, &QPushButton::clicked, this, &ImportRowsDialog::removeSelectedRows);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // Accepting is earned by a successful ImportRows reply, not the click.
    connect(box, &QDialogButtonBox::accepted, this, &ImportRowsDialog::runImport);
    connect(m_grid, &QTableWidget::itemChanged, this, &ImportRowsDialog::updateInsertButton);

    updateInsertButton();
    loadColumns();
}

void ImportRowsDialog::loadColumns()
{
    api()->call(
        "admin", "Columns", {m_connID, m_schema, m_table}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                setError(err);
                return;
            }
            m_colNames.clear();
            QStringList labels;
            QVector<QString> tips;
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                m_colNames.append(o.value("name").toString());
                labels << o.value("name").toString();
                QString tip = o.value("type").toString();
                if (!o.value("extra").toString().isEmpty())
                {
                    tip += " " + o.value("extra").toString();
                }
                tips.append(tip);
            }
            m_grid->setColumnCount(int(m_colNames.size()));
            m_grid->setHorizontalHeaderLabels(labels);
            for (int c = 0; c < tips.size(); ++c)
            {
                if (auto *h = m_grid->horizontalHeaderItem(c))
                {
                    h->setToolTip(tips.at(c));
                }
            }
            if (m_grid->rowCount() == 0)
            {
                addRow();
            }
        }
    );
}

void ImportRowsDialog::chooseCsv()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load CSV"), dir, tr("CSV Files (*.csv *.tsv *.txt)")
    );
    if (path.isEmpty())
    {
        return;
    }
    m_csvPath = path;
    requestCsv(true);
}

void ImportRowsDialog::requestCsv(bool sniff)
{
    if (m_csvPath.isEmpty())
    {
        return;
    }
    const QString sep = m_sepCombo->currentData().toString();
    const bool header = m_headerCheck->isChecked();
    api()->call(
        "query", "ReadCSV", {m_csvPath, sep, header, sniff}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                setError(err);
                return;
            }
            setError({});
            fillFromCsv(res.toObject());
        }
    );
}

void ImportRowsDialog::fillFromCsv(const QJsonObject &preview)
{
    // Reflect what the parser actually used without re-triggering a parse.
    const QSignalBlocker sepBlock(m_sepCombo);
    const QSignalBlocker headerBlock(m_headerCheck);
    m_sepCombo->setEnabled(true);
    m_headerCheck->setEnabled(true);
    const int sepIx = m_sepCombo->findData(preview.value("sep").toString());
    if (sepIx >= 0)
    {
        m_sepCombo->setCurrentIndex(sepIx);
    }
    m_headerCheck->setChecked(preview.value("hasHeader").toBool());

    // CSV column → grid column: by header name when there is one, by
    // position otherwise. Unmatched CSV columns are dropped, with a note.
    QHash<QString, int> byName;
    for (int i = 0; i < m_colNames.size(); ++i)
    {
        byName.insert(m_colNames.at(i).toLower(), i);
    }
    QVector<int> target; // per CSV column; -1 = dropped
    int dropped = 0;
    if (preview.value("hasHeader").toBool())
    {
        for (const auto &h : preview.value("header").toArray())
        {
            const int ix = byName.value(h.toString().trimmed().toLower(), -1);
            target.append(ix);
            dropped += ix < 0 ? 1 : 0;
        }
    }
    else
    {
        for (int i = 0; i < m_colNames.size(); ++i)
        {
            target.append(i);
        }
    }

    const QJsonArray rows = preview.value("rows").toArray();
    const QSignalBlocker gridBlock(m_grid); // one updateInsertButton at the end
    m_grid->setRowCount(0);
    m_grid->setRowCount(int(rows.size()));
    int widest = 0;
    for (int r = 0; r < rows.size(); ++r)
    {
        ensureItems(r);
        const QJsonArray cells = rows.at(r).toArray();
        widest = qMax(widest, int(cells.size()));
        for (int c = 0; c < cells.size() && c < target.size(); ++c)
        {
            const int t = target.at(c);
            if (t >= 0)
            {
                m_grid->item(r, t)->setText(cells.at(c).toString());
            }
        }
    }
    // CSV columns beyond the mapping (ragged or extra) have nowhere to go.
    dropped += qMax(0, widest - int(target.size()));

    QStringList notes;
    notes << tr("%L1 rows loaded").arg(rows.size());
    if (preview.value("truncated").toBool())
    {
        notes << tr("file truncated — it holds %L1 rows").arg(preview.value("totalRows").toInt());
    }
    if (dropped > 0)
    {
        notes << tr("%1 CSV columns matched no table column and were dropped").arg(dropped);
    }
    setHint(notes.join("; ") + ".");
    updateInsertButton();
}

void ImportRowsDialog::addRow()
{
    m_grid->setRowCount(m_grid->rowCount() + 1);
    ensureItems(m_grid->rowCount() - 1);
}

// A QTableWidget cell without an item is not editable; every cell gets one.
void ImportRowsDialog::ensureItems(int row)
{
    for (int c = 0; c < m_grid->columnCount(); ++c)
    {
        if (!m_grid->item(row, c))
        {
            m_grid->setItem(row, c, new QTableWidgetItem);
        }
    }
}

void ImportRowsDialog::removeSelectedRows()
{
    QSet<int> rows;
    for (const auto *it : m_grid->selectedItems())
    {
        rows.insert(it->row());
    }
    QList<int> ordered = rows.values();
    std::sort(ordered.begin(), ordered.end(), std::greater<>());
    for (int r : ordered)
    {
        m_grid->removeRow(r);
    }
    updateInsertButton();
}

int ImportRowsDialog::filledRowCount() const
{
    int n = 0;
    for (int r = 0; r < m_grid->rowCount(); ++r)
    {
        for (int c = 0; c < m_grid->columnCount(); ++c)
        {
            const QTableWidgetItem *it = m_grid->item(r, c);
            if (it && !it->text().isEmpty())
            {
                ++n;
                break;
            }
        }
    }
    return n;
}

void ImportRowsDialog::updateInsertButton()
{
    const int n = filledRowCount();
    m_insertBtn->setEnabled(n > 0);
    m_insertBtn->setText(n == 1 ? tr("Insert 1 Row") : tr("Insert %L1 Rows").arg(n));
}

void ImportRowsDialog::runImport()
{
    // A column some row filled is part of the INSERT for every row; columns
    // nobody filled are omitted so the server default (or auto_increment)
    // applies.
    QVector<int> included;
    for (int c = 0; c < m_grid->columnCount(); ++c)
    {
        for (int r = 0; r < m_grid->rowCount(); ++r)
        {
            const QTableWidgetItem *it = m_grid->item(r, c);
            if (it && !it->text().isEmpty())
            {
                included.append(c);
                break;
            }
        }
    }
    if (included.isEmpty())
    {
        setError(tr("nothing to insert — every cell is empty"));
        return;
    }

    QJsonArray cols;
    for (int c : included)
    {
        cols.append(m_colNames.at(c));
    }
    const bool emptyIsNull = m_nullCheck->isChecked();
    QJsonArray rows;
    for (int r = 0; r < m_grid->rowCount(); ++r)
    {
        QJsonArray row;
        bool any = false;
        for (int c : included)
        {
            const QTableWidgetItem *it = m_grid->item(r, c);
            const QString text = it ? it->text() : QString();
            any = any || !text.isEmpty();
            if (text.isEmpty() && emptyIsNull)
            {
                row.append(QJsonValue::Null);
            }
            else
            {
                row.append(text);
            }
        }
        if (any)
        {
            rows.append(row);
        }
    }

    m_insertBtn->setEnabled(false);
    api()->call(
        "query", "ImportRows", {m_connID, m_schema, m_table, cols, rows}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                // The transaction rolled back; the grid still holds the data,
                // so the user can fix the offending value and retry.
                setError(err);
                updateInsertButton();
                return;
            }
            m_inserted = res.toInt();
            accept();
        }
    );
}

void ImportRowsDialog::setError(const QString &message)
{
    m_errorLbl->setText(message);
    m_errorLbl->setVisible(!message.isEmpty());
}

void ImportRowsDialog::setHint(const QString &message)
{
    m_hintLbl->setText(message);
}
