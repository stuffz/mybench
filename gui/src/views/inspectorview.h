#pragma once
// Workbench-style inspector. Table: info | columns | indexes | fks | ddl.
// Schema: info | tables. Each section fetches on first view.
#include <QHash>
#include <QWidget>

class QPlainTextEdit;
class QTabWidget;
class QTableWidget;

class InspectorView : public QWidget
{
    Q_OBJECT
public:
    InspectorView(
        const QString &connID, const QString &schema, const QString &table,
        const QString &initialSection, QWidget *parent = nullptr
    );

private:
    void loadSection(const QString &id);
    // Error path shared by the loaders: show the error in the table and let
    // the next visit retry instead of leaving the tab permanently blank.
    void failSection(const QString &id, QTableWidget *t, const QString &err);
    void loadInfo();
    void loadColumns();
    void loadIndexes();
    void loadForeignKeys();
    void loadDdl();
    void loadTables();

    QString m_connID, m_schema, m_table;
    QTabWidget *m_tabs;
    QHash<QString, QWidget *> m_pages;
    QHash<QString, bool> m_loaded;
    QHash<int, QString> m_indexById; // tab index → section id
};
