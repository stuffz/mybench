#include "shell/sidebar.h"

#include "app/api.h"
#include "app/icons.h"
#include "app/theme.h"
#include "shell/tabs.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{

// Function-local statics: file-scope globals with dynamic initialisers could
// throw before main (cert-err58).

// MySQL's own schemas, hidden by the "Hide Default Databases" preference.
const QSet<QString> &defaultDBs()
{
    static const QSet<QString> set{"information_schema", "performance_schema", "mysql", "sys"};
    return set;
}

struct AdminPage
{
    TabView view;
    const char *label;
    const char *icon;
};

const QVector<AdminPage> &adminPages()
{
    static const QVector<AdminPage> list{
        {TabView::Dashboard, "Dashboard", "activity"},
        {TabView::ServerInfo, "Server Status", "gauge"},
        {TabView::InnoDB, "InnoDB Status", "database"},
        {TabView::Processlist, "Client Connections", "network"},
        {TabView::Users, "Users and Privileges", "users"},
        {TabView::Graph, "Schema Graph", "waypoints"},
        {TabView::History, "Query History", "history"},
    };
    return list;
}

constexpr int RoleKind = Qt::UserRole + 1;
constexpr int RoleSchema = Qt::UserRole + 2;
constexpr int RoleTable = Qt::UserRole + 3;
constexpr int RoleIsView = Qt::UserRole + 4;

} // namespace

Sidebar::Sidebar(QWidget *parent) : QWidget(parent)
{
    // No border-right here: the 1px splitter handle right beside it is the
    // sidebar/content divider, and a border as well doubled the line to 2px.

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Mode toggle: Administration | Schemas
    auto *modes = new QWidget;
    modes->setObjectName("SideModes"); // border-bottom rule in theme.cpp
    modes->setAttribute(Qt::WA_StyledBackground, true);
    auto *modeRow = new QHBoxLayout(modes);
    modeRow->setContentsMargins(6, 6, 6, 6);
    modeRow->setSpacing(4);
    m_adminBtn = new QPushButton(tr("Administration"));
    m_schemaBtn = new QPushButton(tr("Schemas"));
    for (QPushButton *b : {m_adminBtn, m_schemaBtn})
    {
        b->setProperty("variant", "toggle");
        b->setCheckable(true);
        modeRow->addWidget(b);
    }
    m_schemaBtn->setChecked(true);
    root->addWidget(modes);

    m_stack = new QStackedWidget;

    m_admin = new QListWidget;
    m_admin->setObjectName("AdminList"); // inset + row padding rules in theme.cpp
    // Without this QListView reserves PM_ListViewIconSize (32px) of row height
    // per icon, so the 14px glyphs came with ~20px of dead space between rows.
    m_admin->setIconSize(QSize(14, 14));
    m_admin->setFrameShape(QFrame::NoFrame);
    m_stack->addWidget(m_admin);

    auto *schemaPane = new QWidget;
    auto *schemaLayout = new QVBoxLayout(schemaPane);
    schemaLayout->setContentsMargins(0, 0, 0, 0);
    schemaLayout->setSpacing(0);
    auto *filterWrap = new QWidget;
    filterWrap->setObjectName("SideFilter"); // border-bottom rule in theme.cpp
    filterWrap->setAttribute(Qt::WA_StyledBackground, true);
    auto *filterRow = new QHBoxLayout(filterWrap);
    // 8px vertical, matching the admin panels' PanelHeader (8 + control + 8),
    // so this row's bottom hairline meets theirs across the splitter.
    filterRow->setContentsMargins(6, 8, 6, 8);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter objects"));
    m_filter->setClearButtonEnabled(true);
    filterRow->addWidget(m_filter);
    // Workbench parity: the tree loads once per connection, so a schema
    // created after that (seeding, another client) needs an explicit refetch.
    m_refreshBtn = new QPushButton;
    m_refreshBtn->setProperty("variant", "ghost");
    m_refreshBtn->setToolTip(tr("Refresh"));
    filterRow->addWidget(m_refreshBtn);
    schemaLayout->addWidget(filterWrap);

    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setUniformRowHeights(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setExpandsOnDoubleClick(false);
    schemaLayout->addWidget(m_tree, 1);
    m_stack->addWidget(schemaPane);
    m_stack->setCurrentIndex(1);
    root->addWidget(m_stack, 1);

    connect(
        m_adminBtn, &QPushButton::clicked, this,
        [this]()
        {
            m_adminBtn->setChecked(true);
            m_schemaBtn->setChecked(false);
            m_stack->setCurrentIndex(0);
        }
    );
    connect(
        m_schemaBtn, &QPushButton::clicked, this,
        [this]()
        {
            m_schemaBtn->setChecked(true);
            m_adminBtn->setChecked(false);
            m_stack->setCurrentIndex(1);
        }
    );

    // Mouse only. itemActivated is deliberately not connected: on styles that
    // activate on single click it fires alongside itemClicked, requesting the
    // tab twice. Enter is wired separately below.
    connect(
        m_admin, &QListWidget::itemClicked, this,
        [this](QListWidgetItem *it)
        {
            TabRequest r;
            r.view = TabView(it->data(Qt::UserRole).toInt());
            emit tabRequested(r);
        }
    );
    for (auto key : {Qt::Key_Return, Qt::Key_Enter})
    {
        auto *enter = new QShortcut(key, m_admin);
        enter->setContext(Qt::WidgetShortcut);
        connect(
            enter, &QShortcut::activated, this,
            [this]()
            {
                if (QListWidgetItem *it = m_admin->currentItem())
                {
                    TabRequest r;
                    r.view = TabView(it->data(Qt::UserRole).toInt());
                    emit tabRequested(r);
                }
            }
        );
    }

    connect(
        m_tree, &QTreeWidget::itemExpanded, this,
        [this](QTreeWidgetItem *it)
        {
            const int kind = it->data(0, RoleKind).toInt();
            if (kind == SchemaItem)
            {
                loadTables(it);
            }
            else if (kind == TableItem)
            {
                loadColumns(it);
            }
        }
    );
    connect(
        m_tree, &QTreeWidget::itemDoubleClicked, this,
        [this](QTreeWidgetItem *it)
        {
            if (it->data(0, RoleKind).toInt() != TableItem)
            {
                return;
            }
            TabRequest r;
            r.view = TabView::Editor;
            r.sql = "SELECT * FROM " + quoteIdent(it->data(0, RoleSchema).toString()) + "." +
                    quoteIdent(it->data(0, RoleTable).toString()) + " LIMIT 200;";
            emit tabRequested(r);
        }
    );
    connect(m_tree, &QWidget::customContextMenuRequested, this, &Sidebar::showMenu);
    connect(m_filter, &QLineEdit::textChanged, this, &Sidebar::applyFilter);
    connect(m_refreshBtn, &QPushButton::clicked, this, &Sidebar::reload);

    buildAdmin();
    applyRefreshIcon();
}

// Sized like the editor toolbar glyphs: the UI font's pixel size, with an
// explicit iconSize so the style does not scale it up to its 16px default.
void Sidebar::applyRefreshIcon()
{
    const int px = theme::scaledPx(1.0);
    m_refreshBtn->setIcon(icons::icon("rotate-cw", theme::current().mutedFg, px));
    m_refreshBtn->setIconSize(QSize(px, px));
}

void Sidebar::applyTheme()
{
    const AppPalette &pal = theme::current();
    buildAdmin();
    applyRefreshIcon();
    // Walk what is already loaded rather than refetching: the icons are the
    // only thing that depends on the palette.
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem *schema = m_tree->topLevelItem(i);
        schema->setIcon(0, icons::icon("database", pal.warning, 14));
        schema->setForeground(
            0, defaultDBs().contains(schema->text(0)) ? pal.mutedFg : pal.foreground
        );
        for (int j = 0; j < schema->childCount(); ++j)
        {
            QTreeWidgetItem *t = schema->child(j);
            if (t->data(0, RoleKind).toInt() != TableItem)
            {
                continue;
            }
            t->setIcon(
                0, t->data(0, RoleIsView).toBool() ? icons::icon("eye", pal.special, 14)
                                                   : icons::icon("table-2", pal.info, 14)
            );
            for (int k = 0; k < t->childCount(); ++k)
            {
                t->child(k)->setForeground(0, pal.mutedFg);
            }
        }
    }
}

void Sidebar::buildAdmin()
{
    m_admin->clear();
    const AppPalette &pal = theme::current();
    for (const AdminPage &p : adminPages())
    {
        auto *it = new QListWidgetItem(tr(p.label), m_admin);
        it->setIcon(icons::icon(p.icon, pal.mutedFg, 14));
        it->setData(Qt::UserRole, int(p.view));
    }
}

void Sidebar::setConnection(const QString &connID, bool connected)
{
    if (m_connID == connID && m_connected == connected)
    {
        return; // re-selecting the same server must not refetch the tree
    }
    m_connID = connID;
    m_connected = connected;
    reload();
}

void Sidebar::setHideDefaultDBs(bool on)
{
    if (m_hideDefaultDBs == on)
    {
        return;
    }
    m_hideDefaultDBs = on;
    reload();
}

void Sidebar::reload()
{
    ++m_gen;
    m_tree->clear();
    m_loadedTables.clear();
    m_loadedColumns.clear();
    if (!m_connected || m_connID.isEmpty())
    {
        return;
    }
    loadSchemas();
}

void Sidebar::loadSchemas()
{
    const QString conn = m_connID;
    const int gen = m_gen;
    api()->call(
        "admin", "Schemas", {conn}, this,
        [this, conn, gen](const QJsonValue &res, const QString &err)
        {
            if (conn != m_connID || gen != m_gen)
            {
                return;
            }
            if (!err.isEmpty())
            {
                emit errorRaised(err);
                return;
            }
            const AppPalette &pal = theme::current();
            for (const auto &v : res.toArray())
            {
                const QString name = v.toString();
                if (m_hideDefaultDBs && defaultDBs().contains(name))
                {
                    continue;
                }
                auto *it = new QTreeWidgetItem(m_tree);
                it->setText(0, name);
                it->setIcon(0, icons::icon("database", pal.warning, 14));
                it->setData(0, RoleKind, SchemaItem);
                it->setData(0, RoleSchema, name);
                it->setForeground(0, defaultDBs().contains(name) ? pal.mutedFg : pal.foreground);
                // A placeholder child makes the expander appear before the
                // children are fetched.
                it->addChild(new QTreeWidgetItem(QStringList{QStringLiteral("…")}));
            }
            applyFilter();
        }
    );
}

void Sidebar::loadTables(QTreeWidgetItem *schemaItem)
{
    const QString schema = schemaItem->data(0, RoleSchema).toString();
    if (m_loadedTables.contains(schema))
    {
        return;
    }
    m_loadedTables.insert(schema);
    const QString conn = m_connID;
    const int gen = m_gen;
    api()->call(
        "admin", "Tables", {conn, schema}, this,
        [this, conn, schema, schemaItem, gen](const QJsonValue &res, const QString &err)
        {
            if (conn != m_connID || gen != m_gen)
            {
                return;
            }
            if (!err.isEmpty())
            {
                m_loadedTables.remove(schema);
                emit errorRaised(err);
                return;
            }
            const AppPalette &pal = theme::current();
            schemaItem->takeChildren();
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                const QString name = o.value("name").toString();
                const bool isView = o.value("type").toString() == "VIEW";
                auto *it = new QTreeWidgetItem(schemaItem);
                it->setText(0, name);
                it->setData(0, RoleKind, TableItem);
                it->setData(0, RoleSchema, schema);
                it->setData(0, RoleTable, name);
                it->setData(0, RoleIsView, isView);
                it->setToolTip(0, schema + "." + name + (isView ? tr(" (view)") : QString()));
                // The glyph carries the distinction, as it did on the
                // web; colouring the label instead just raised the
                // question of why one row was purple.
                it->setIcon(
                    0, isView ? icons::icon("eye", pal.special, 14)
                              : icons::icon("table-2", pal.info, 14)
                );
                it->addChild(new QTreeWidgetItem(QStringList{QStringLiteral("…")}));
            }
            applyFilter();
        }
    );
}

void Sidebar::loadColumns(QTreeWidgetItem *tableItem)
{
    const QString schema = tableItem->data(0, RoleSchema).toString();
    const QString table = tableItem->data(0, RoleTable).toString();
    const QString key = schema + "." + table;
    if (m_loadedColumns.contains(key))
    {
        return;
    }
    m_loadedColumns.insert(key);
    const QString conn = m_connID;
    const int gen = m_gen;
    api()->call(
        "admin", "Columns", {conn, schema, table}, this,
        [this, conn, key, tableItem, gen](const QJsonValue &res, const QString &err)
        {
            if (conn != m_connID || gen != m_gen)
            {
                return;
            }
            if (!err.isEmpty())
            {
                m_loadedColumns.remove(key);
                emit errorRaised(err);
                return;
            }
            const AppPalette &pal = theme::current();
            tableItem->takeChildren();
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                auto *it = new QTreeWidgetItem(tableItem);
                const QString pk = o.value("key").toString() == "PRI" ? " 🔑" : QString();
                it->setText(0, o.value("name").toString() + "  " + o.value("type").toString() + pk);
                it->setData(0, RoleKind, ColumnItem);
                it->setForeground(0, pal.mutedFg);
                it->setFlags(Qt::ItemIsEnabled);
            }
        }
    );
}

void Sidebar::applyFilter()
{
    const QString f = m_filter->text().trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem *schema = m_tree->topLevelItem(i);
        if (f.isEmpty())
        {
            schema->setHidden(false);
            for (int j = 0; j < schema->childCount(); ++j)
            {
                schema->child(j)->setHidden(false);
            }
            continue;
        }
        const bool schemaHit = schema->text(0).contains(f, Qt::CaseInsensitive);
        bool anyChild = false;
        for (int j = 0; j < schema->childCount(); ++j)
        {
            QTreeWidgetItem *t = schema->child(j);
            if (t->data(0, RoleKind).toInt() != TableItem)
            {
                continue;
            }
            const bool hit = schemaHit || t->text(0).contains(f, Qt::CaseInsensitive);
            t->setHidden(!hit);
            anyChild = anyChild || hit;
        }
        schema->setHidden(!(schemaHit || anyChild));
    }
}

void Sidebar::showMenu(const QPoint &pos)
{
    QTreeWidgetItem *it = m_tree->itemAt(pos);
    if (!it)
    {
        return;
    }
    // The "…" placeholder child carries no roles; without this it would read
    // as a SchemaItem with an empty schema and offer menus that emit `USE ``;`.
    if (!it->data(0, RoleKind).isValid())
    {
        return;
    }
    const int kind = it->data(0, RoleKind).toInt();
    if (kind == ColumnItem)
    {
        return;
    }
    const QString schema = it->data(0, RoleSchema).toString();
    const QString table = it->data(0, RoleTable).toString();
    m_tree->setCurrentItem(it);

    QMenu menu(this);
    auto add = [this](QMenu &m, const QString &label, const TabRequest &r)
    { m.addAction(label, this, [this, r]() { emit tabRequested(r); }); };

    if (kind == TableItem)
    {
        TabRequest open;
        open.view = TabView::Editor;
        open.sql = "SELECT * FROM " + quoteIdent(schema) + "." + quoteIdent(table) + " LIMIT 200;";
        add(menu, tr("Open in New Query Tab"), open);
        menu.addSeparator();
        for (const auto &sec : QVector<QPair<QString, QString>>{
                 {"info", tr("Table Inspector")},
                 {"columns", tr("Columns")},
                 {"indexes", tr("Indexes")},
                 {"fks", tr("Foreign Keys")},
                 {"ddl", tr("DDL")}
             })
        {
            TabRequest r;
            r.view = TabView::TableInspect;
            r.schema = schema;
            r.table = table;
            r.section = sec.first;
            add(menu, sec.second, r);
        }
        menu.addSeparator();
        menu.addAction(
            tr("Show in Graph"), this,
            [this, schema, table]() { emit graphFocusRequested(schema, table); }
        );
        menu.addSeparator();
        menu.addAction(
            tr("Import Rows…"), this,
            [this, schema, table]() { emit importRowsRequested(schema, table); }
        );
        menu.addSeparator();
        menu.addAction(
            tr("Copy Name"), this,
            [schema, table]() { QApplication::clipboard()->setText(schema + "." + table); }
        );
    }
    else
    {
        TabRequest use;
        use.view = TabView::Editor;
        use.sql = "USE " + quoteIdent(schema) + ";\n";
        add(menu, tr("New Query Tab on This Schema"), use);
        TabRequest insp;
        insp.view = TabView::SchemaInspect;
        insp.schema = schema;
        insp.section = "info";
        add(menu, tr("Schema Inspector"), insp);
        menu.addSeparator();
        menu.addAction(
            tr("Copy Name"), this, [schema]() { QApplication::clipboard()->setText(schema); }
        );
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}
