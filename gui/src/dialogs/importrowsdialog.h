#pragma once
// Insert rows into one table: a grid typed by hand, or prefilled from a CSV
// file. The backend parses the CSV (ReadCSV), so the grid preview is exactly
// what ImportRows will insert; separator and header-row choices re-parse.
#include <QDialog>
#include <QJsonObject>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

class ImportRowsDialog : public QDialog
{
    Q_OBJECT
public:
    ImportRowsDialog(
        const QString &connID, const QString &schema, const QString &table,
        QWidget *parent = nullptr
    );

    // Rows the accepted import actually inserted.
    int insertedCount() const { return m_inserted; }

private:
    void loadColumns();
    void chooseCsv();
    void requestCsv(bool sniff);
    void fillFromCsv(const QJsonObject &preview);
    void addRow();
    void ensureItems(int row);
    void removeSelectedRows();
    void runImport();
    void setError(const QString &message);
    void setHint(const QString &message);
    void updateInsertButton();
    // Rows where at least one cell holds text; fully empty rows are skipped.
    int filledRowCount() const;

    QString m_connID, m_schema, m_table;
    QString m_csvPath;
    QVector<QString> m_colNames;
    int m_inserted = 0;

    QTableWidget *m_grid;
    QPushButton *m_csvBtn, *m_addBtn, *m_removeBtn, *m_insertBtn;
    QComboBox *m_sepCombo;
    QCheckBox *m_headerCheck, *m_nullCheck;
    QLabel *m_hintLbl, *m_errorLbl;
};
