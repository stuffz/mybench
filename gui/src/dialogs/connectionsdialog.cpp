#include "dialogs/connectionsdialog.h"

#include "app/api.h"
#include "app/icons.h"
#include "app/theme.h"
#include "ui/handcursor.h"
#include "ui/widgets.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QVBoxLayout>

namespace
{

// Function-local static: a file-scope global with a dynamic initialiser could
// throw before main (cert-err58).
const QVector<QPair<QString, QString>> &methods()
{
    static const QVector<QPair<QString, QString>> list{
        {"tcp", "Standard (TCP/IP)"},
        {"ssh", "Standard (TCP/IP) over SSH"},
        {"teleport", "Teleport"},
    };
    return list;
}

// Item data on the quick-connect lists.
constexpr int quickNameRole = Qt::UserRole;    // resource or user name (empty = not usable)
constexpr int quickKeyRole = Qt::UserRole + 1; // 0-based pick-key index

// Pick keys per quick-connect list: 1-9 first, then a-z.
constexpr int quickDigitKeys = 9;
constexpr int quickLetterKeys = 26;

// The wrapping muted line under a field. A definite width lets the form layout
// compute heightForWidth, so a wrapped agent/tsh message gets the rows it needs
// instead of clipping.
QLabel *note(const QString &text = {})
{
    QLabel *l = mutedLabel(text);
    l->setWordWrap(true);
    l->setMaximumWidth(340);
    return l;
}

// Icon + explicit iconSize, like the editor toolbar: without the size the
// style scales the pixmap to its 16px default, which never matches the label.
void setButtonIcon(QAbstractButton *b, const char *name, const QColor &colour)
{
    const int px = theme::scaledPx(1.0);
    b->setIcon(icons::icon(name, colour, px));
    b->setIconSize(QSize(px, px));
}

QSpinBox *portBox(int value)
{
    auto *s = new QSpinBox;
    s->setRange(0, 65535);
    s->setValue(value);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    // A port is five digits; letting it grow like the text fields made it look
    // like the most important thing on the form.
    s->setMaximumWidth(96);
    return s;
}

} // namespace

QKeySequence quickConnectShortcut()
{
#ifdef Q_OS_MACOS
    // Qt::CTRL is ⌘ there, and ⌘Q is the system-wide Quit — bind the
    // physical Control key (Qt::META) instead: ⌃Q.
    return {Qt::META | Qt::Key_Q};
#else
    return {Qt::CTRL | Qt::Key_Q};
#endif
}

ConnectionsDialog::ConnectionsDialog(
    const QVector<QJsonObject> &saved, const QVector<QString> &openIDs, Start start, QWidget *parent
)
    : QDialog(parent), m_saved(saved), m_openIDs(openIDs)
{
    setWindowTitle(tr("Connections"));
    setMinimumWidth(600);

    auto *root = new QVBoxLayout(this);

    // --- list page --------------------------------------------------------
    m_listPage = new QWidget;
    m_listLayout = new QVBoxLayout(m_listPage);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(6);
    root->addWidget(m_listPage);

    // --- editor page ------------------------------------------------------
    auto *editor = new QWidget;
    auto *form = new QFormLayout(editor);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_form = form;

    m_name = new QLineEdit;
    m_name->setPlaceholderText(tr("auto — derived from the connection details"));
    form->addRow(tr("Name"), m_name);

    // --- accent colour ----------------------------------------------------
    // A swatch that opens the platform picker, the hex for exactness, and a
    // reset back to the automatic per-connection hue.
    m_color = new QLineEdit;
    m_color->setPlaceholderText(tr("auto"));
    m_color->setMaximumWidth(110);

    auto *colorRow = new QWidget;
    auto *colorLayout = new QHBoxLayout(colorRow);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    colorLayout->setSpacing(6);

    m_swatch = new QPushButton;
    // Width only: the height comes from the shared control rule in the
    // stylesheet, so the swatch lines up with the hex field beside it.
    m_swatch->setFixedWidth(theme::scaledPx(3.5));
    m_swatch->setToolTip(tr("Pick an accent colour"));
    connect(m_swatch, &QPushButton::clicked, this, &ConnectionsDialog::pickColor);

    auto *autoColor = new QPushButton(tr("Auto"));
    autoColor->setProperty("variant", "ghost");
    autoColor->setToolTip(tr("Use the automatic colour derived from the connection id"));
    connect(
        autoColor, &QPushButton::clicked, this,
        [this]()
        {
            m_color->clear();
            updateSwatch();
        }
    );
    connect(m_color, &QLineEdit::textChanged, this, &ConnectionsDialog::updateSwatch);

    colorLayout->addWidget(m_swatch);
    colorLayout->addWidget(m_color);
    colorLayout->addWidget(autoColor);
    colorLayout->addStretch();
    form->addRow(tr("Color"), colorRow);

    m_method = new QComboBox;
    for (const auto &m : methods())
    {
        m_method->addItem(m.second, m.first);
    }
    form->addRow(tr("Method"), m_method);

    // --- SSH rows (hidden unless the method needs them) --------------------
    m_sshHost = new QLineEdit;
    m_sshPort = portBox(22);
    m_sshUser = new QLineEdit;
    m_sshKeyFile = new QLineEdit;

    auto *keyRow = new QWidget;
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);

    auto *browseKey = new QPushButton(tr("Browse"));
    // Native file dialog: the web build had to hand-roll a browser over an
    // RPC (conn.SSHBrowse); here the platform provides one.
    connect(
        browseKey, &QPushButton::clicked, this,
        [this]()
        {
            const QString start = m_sshKeyFile->text().isEmpty()
                                      ? QDir::homePath() + "/.ssh"
                                      : QFileInfo(m_sshKeyFile->text()).absolutePath();
            const QString f = QFileDialog::getOpenFileName(this, tr("SSH Private Key"), start);
            if (!f.isEmpty())
            {
                m_sshKeyFile->setText(f);
            }
        }
    );

    keyLayout->addWidget(m_sshKeyFile, 1);
    keyLayout->addWidget(browseKey);

    m_agentNote = note(tr("checking ssh-agent…"));

    // The row indices are taken as the rows go in; applyMethod() shows and
    // hides them by index, so these must stay interleaved with the addRow
    // calls they describe.
    m_rows.ssh = {form->rowCount()};
    form->addRow(tr("SSH Host"), m_sshHost);
    m_rows.ssh << form->rowCount();
    form->addRow(tr("SSH Port"), m_sshPort);
    m_rows.ssh << form->rowCount();
    form->addRow(tr("SSH User"), m_sshUser);
    m_rows.ssh << form->rowCount();
    form->addRow(tr("SSH Key File"), keyRow);
    m_rows.ssh << form->rowCount();
    form->addRow(QString(), m_agentNote);

    // --- Teleport rows ----------------------------------------------------
    m_teleportDb = new QLineEdit;
    m_teleportDb->setPlaceholderText(tr("teleport db resource"));

    auto *resourceRow = new QWidget;
    auto *resourceLayout = new QHBoxLayout(resourceRow);
    resourceLayout->setContentsMargins(0, 0, 0, 0);

    auto *browseResource = new QPushButton(tr("Browse"));
    connect(browseResource, &QPushButton::clicked, this, &ConnectionsDialog::browseTeleport);

    resourceLayout->addWidget(m_teleportDb, 1);
    resourceLayout->addWidget(browseResource);

    m_teleportNote = note(tr("checking tsh…"));

    m_rows.teleport = {form->rowCount()};
    form->addRow(tr("Resource"), resourceRow);
    m_rows.teleport << form->rowCount();
    form->addRow(QString(), m_teleportNote);

    // --- MySQL endpoint ---------------------------------------------------
    m_host = new QLineEdit;
    m_port = portBox(3306);
    m_rows.tcp = {form->rowCount()};
    form->addRow(tr("Host"), m_host);
    m_rows.tcp << form->rowCount();
    form->addRow(tr("Port"), m_port);

    m_user = new QLineEdit;
    form->addRow(tr("User"), m_user);
    m_database = new QLineEdit;
    m_database->setPlaceholderText(tr("optional default schema"));
    form->addRow(tr("Database"), m_database);

    // --- password ---------------------------------------------------------
    m_password = new QLineEdit;
    m_password->setEchoMode(QLineEdit::Password);
    connect(m_password, &QLineEdit::textEdited, this, [this]() { m_passwordEdited = true; });

    auto *passwordRow = new QWidget;
    auto *passwordLayout = new QHBoxLayout(passwordRow);
    passwordLayout->setContentsMargins(0, 0, 0, 0);

    m_removePassword = new QPushButton(tr("Remove"));
    m_removePassword->setToolTip(tr("Delete the stored password for this connection"));
    connect(m_removePassword, &QPushButton::clicked, this, &ConnectionsDialog::removePassword);

    passwordLayout->addWidget(m_password, 1);
    passwordLayout->addWidget(m_removePassword);
    form->addRow(tr("Password"), passwordRow);

    m_tls = new QComboBox;
    m_tls->addItems({"disabled", "preferred", "required"});
    m_tls->setCurrentText("preferred");
    form->addRow(tr("TLS"), m_tls);

    // --- Cancel / Save, as the last form row ------------------------------
    auto *box = new QDialogButtonBox;
    auto *cancel = box->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    auto *saveBtn = box->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    saveBtn->setProperty("variant", "primary");
    connect(cancel, &QPushButton::clicked, this, [this]() { showPage(Page::List); });
    connect(saveBtn, &QPushButton::clicked, this, &ConnectionsDialog::save);
    form->addRow(box);

    m_editorPage = editor;
    m_editorPage->setVisible(false);
    root->addWidget(editor);

    buildQuickPage(root);

    // Wired after the rows exist: applyMethod() reaches for every one of them.
    connect(
        m_method, &QComboBox::currentIndexChanged, this,
        [this]() { applyMethod(m_method->currentData().toString()); }
    );

    // Single keys for the list page's two actions, mirrored by the mnemonic
    // underlines on the buttons. showPage() disables them off the list page —
    // active window-wide they would swallow N and Q typed into the other
    // pages' fields.
    m_newKey = new QShortcut(QKeySequence(Qt::Key_N), this);
    connect(m_newKey, &QShortcut::activated, this, [this]() { editProfile({}); });
    m_quickKey = new QShortcut(QKeySequence(Qt::Key_Q), this);
    connect(m_quickKey, &QShortcut::activated, this, &ConnectionsDialog::openQuick);

    setSaved(saved, openIDs);
    adjustSize();

    if (start == Start::QuickConnect)
    {
        openQuick();
    }
}

void ConnectionsDialog::reject()
{
    // On the quick page Escape backs out one step at a time, like Back does.
    if (m_quickPage->isVisible() && m_quickStep == QuickStep::Users)
    {
        listQuickResources();
        return;
    }
    if (!m_listPage->isVisible())
    {
        showPage(Page::List);
        return;
    }
    QDialog::reject();
}

void ConnectionsDialog::setSaved(const QVector<QJsonObject> &saved, const QVector<QString> &openIDs)
{
    // Ephemeral (quick-connect) profiles exist so the shell can label tabs;
    // the dialog manages saved profiles only, so they never show here.
    m_saved.clear();
    for (const QJsonObject &c : saved)
    {
        if (!c.value("ephemeral").toBool())
        {
            m_saved.append(c);
        }
    }
    m_openIDs = openIDs;
    buildList();
    showPage(Page::List);
}

// Only one of the pages is ever visible; hiding the others keeps the dialog
// sized to what is on screen.
void ConnectionsDialog::showPage(Page page)
{
    m_listPage->setVisible(page == Page::List);
    m_editorPage->setVisible(page == Page::Editor);
    m_quickPage->setVisible(page == Page::Quick);
    m_newKey->setEnabled(page == Page::List);
    m_quickKey->setEnabled(page == Page::List);
    layout()->activate();
    adjustSize();
}

namespace
{

// The accent shown in the list: the saved override, else the same stable hue
// the server tabs derive from the id.
QColor accentFor(const QJsonObject &c)
{
    const QString custom = c.value("color").toString();
    if (!custom.isEmpty())
    {
        const QColor picked(custom);
        if (picked.isValid())
        {
            return picked;
        }
    }
    return theme::connAccent(c.value("id").toString());
}

} // namespace

void ConnectionsDialog::buildList()
{
    while (QLayoutItem *item = m_listLayout->takeAt(0))
    {
        if (QWidget *w = item->widget())
        {
            w->deleteLater();
        }
        delete item;
    }

    bool first = true;
    for (int at = 0; at < m_saved.size(); ++at)
    {
        const QJsonObject c = m_saved.at(at);
        const QString id = c.value("id").toString();
        const bool open = m_openIDs.contains(id);

        // A rule between entries: name over details on one line each was still
        // hard to scan when four connections ran together.
        if (!first)
        {
            m_listLayout->addWidget(hairline());
        }
        first = false;

        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(2, 6, 2, 6);
        rowLayout->setSpacing(10);

        // The accent, so the colour is visible where you pick connections
        // rather than only once a server tab exists.
        auto *dot = new QLabel;
        dot->setFixedSize(10, 10);
        dot->setStyleSheet(
            QString("QLabel { background: %1; border-radius: 5px; }").arg(accentFor(c).name())
        );
        rowLayout->addWidget(dot, 0, Qt::AlignVCenter);

        // Second line: how this connection actually reaches the server.
        const QString method = c.value("method").toString();
        QString detail;
        if (method == "teleport")
        {
            detail = tr("teleport · %1").arg(c.value("teleportDb").toString());
        }
        else if (method == "ssh")
        {
            detail = tr("%1@%2 via ssh %3@%4")
                         .arg(
                             c.value("user").toString(), c.value("host").toString(),
                             c.value("sshUser").toString(), c.value("sshHost").toString()
                         );
        }
        else
        {
            detail = QString("%1@%2:%3")
                         .arg(c.value("user").toString(), c.value("host").toString())
                         .arg(c.value("port").toInt());
        }
        if (!c.value("database").toString().isEmpty())
        {
            detail += " · " + c.value("database").toString();
        }

        auto *name = weightedLabel(c.value("name").toString(), QFont::Medium);
        auto *where = note(detail);
        where->setObjectName("smallText"); // sized by the sheet; see theme.cpp

        auto *text = new QWidget;
        auto *textLayout = new QVBoxLayout(text);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(1);
        textLayout->addWidget(name);
        textLayout->addWidget(where);
        rowLayout->addWidget(text, 1);

        // Connect or Disconnect depending on what this profile is doing now,
        // then the two actions that apply either way.
        if (open)
        {
            auto *dis = new QPushButton(tr("Disconnect"));
            setButtonIcon(dis, "x", theme::current().foreground);
            connect(
                dis, &QPushButton::clicked, this,
                [this, id]()
                {
                    emit disconnectRequested(id);
                    m_openIDs.removeAll(id);
                    buildList();
                }
            );
            rowLayout->addWidget(dis, 0, Qt::AlignVCenter);
        }
        else
        {
            auto *conn = new QPushButton(tr("Connect"));
            conn->setProperty("variant", "primary");
            setButtonIcon(conn, "play", theme::current().primaryFg);
            connect(
                conn, &QPushButton::clicked, this,
                [this, id]()
                {
                    emit connectRequested(id);
                    accept();
                }
            );
            rowLayout->addWidget(conn, 0, Qt::AlignVCenter);
        }

        auto *edit = new QPushButton(tr("Edit"));
        edit->setProperty("variant", "ghost");
        setButtonIcon(edit, "pencil", theme::current().mutedFg);
        connect(edit, &QPushButton::clicked, this, [this, c]() { editProfile(c); });
        rowLayout->addWidget(edit, 0, Qt::AlignVCenter);

        auto *del = new QPushButton(tr("Delete"));
        del->setProperty("variant", "ghost");
        setButtonIcon(del, "trash-2", theme::current().mutedFg);
        connect(
            del, &QPushButton::clicked, this,
            [this, id, c]()
            {
                if (QMessageBox::warning(
                        this, tr("Delete connection"),
                        tr("Delete the saved profile \"%1\"? The stored password goes "
                           "with it.")
                            .arg(c.value("name").toString()),
                        QMessageBox::Cancel | QMessageBox::Yes
                    ) != QMessageBox::Yes)
                {
                    return;
                }
                api()->call(
                    "conn", "Delete", {id}, this,
                    [this](const QJsonValue &, const QString &err)
                    {
                        if (!err.isEmpty())
                        {
                            QMessageBox::warning(this, tr("Delete"), err);
                            return;
                        }
                        emit savedChanged();
                    }
                );
            }
        );
        rowLayout->addWidget(del, 0, Qt::AlignVCenter);

        // Reorder arrows: the list's order is the stored order, so moving a
        // row persists. The inapplicable arrow on the first and last row is
        // hidden, not disabled — its slot keeps its size so the button
        // columns stay lined up.
        const auto arrow = [](const char *icon, const QString &tip)
        {
            auto *b = new QPushButton;
            b->setProperty("variant", "ghost");
            const int px = theme::scaledPx(1.2);
            b->setIcon(icons::icon(icon, theme::current().mutedFg, px));
            b->setIconSize(QSize(px, px));
            b->setFixedWidth(theme::scaledPx(2.2));
            b->setToolTip(tip);
            QSizePolicy sp = b->sizePolicy();
            sp.setRetainSizeWhenHidden(true);
            b->setSizePolicy(sp);
            return b;
        };
        QPushButton *up = arrow("move-up", tr("Move Up"));
        up->setVisible(at > 0);
        connect(up, &QPushButton::clicked, this, [this, at]() { moveProfile(at, at - 1); });
        rowLayout->addWidget(up, 0, Qt::AlignVCenter);

        QPushButton *down = arrow("move-down", tr("Move Down"));
        down->setVisible(at < m_saved.size() - 1);
        connect(down, &QPushButton::clicked, this, [this, at]() { moveProfile(at, at + 1); });
        rowLayout->addWidget(down, 0, Qt::AlignVCenter);

        m_listLayout->addWidget(row);
    }

    // The parenthesised letter names the single-key shortcut (m_newKey /
    // m_quickKey) — spelled out because mnemonic underlines are invisible on
    // styles that only draw them while Alt is held. The ampersand still gives
    // Alt+N / Alt+Q and, where the style does draw it, the underline.
    auto *newBtn = new QPushButton(tr("New Connection (&N)"));
    setButtonIcon(newBtn, "plus", theme::current().foreground);
    connect(newBtn, &QPushButton::clicked, this, [this]() { editProfile({}); });

    // Quick connect opens a Teleport resource ad hoc — nothing is saved, so
    // it lives beside (not inside) the saved-profile list.
    auto *quickBtn = new QPushButton(tr("Quick Connect (&Q)"));
    setButtonIcon(quickBtn, "database-zap", theme::current().foreground);
    quickBtn->setToolTip(tr("Open a Teleport database without saving a profile"));
    connect(quickBtn, &QPushButton::clicked, this, &ConnectionsDialog::openQuick);

    auto *actions = new QWidget;
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(6);
    actionsLayout->addWidget(newBtn, 1);
    actionsLayout->addWidget(quickBtn, 1);
    m_listLayout->addWidget(actions);
}

void ConnectionsDialog::moveProfile(int from, int to)
{
    if (from < 0 || from >= m_saved.size() || to < 0 || to >= m_saved.size())
    {
        return;
    }
    m_saved.move(from, to);
    buildList(); // optimistic: the row moves now, the store catches up
    QJsonArray ids;
    for (const QJsonObject &c : m_saved)
    {
        ids.append(c.value("id").toString());
    }
    // One positional argument holding the array. A braced {ids} copy-inits
    // the outer QJsonArray from ids instead of nesting it, and the RPC then
    // counts every id as its own argument.
    QJsonArray args;
    args.append(ids);
    api()->call(
        "conn", "Reorder", args, this,
        [this](const QJsonValue &, const QString &err)
        {
            if (!err.isEmpty())
            {
                QMessageBox::warning(this, tr("Reorder"), err);
            }
            // Either way, resync from the store — on failure this snaps the
            // optimistic move back.
            emit savedChanged();
        }
    );
}

void ConnectionsDialog::applyMethod(const QString &method)
{
    const bool ssh = method == "ssh";
    const bool teleport = method == "teleport";
    for (int row : m_rows.ssh)
    {
        m_form->setRowVisible(row, ssh);
    }
    for (int row : m_rows.teleport)
    {
        m_form->setRowVisible(row, teleport);
    }
    // Over SSH the endpoint is still needed — it just describes MySQL as seen
    // from the SSH host.
    for (int row : m_rows.tcp)
    {
        m_form->setRowVisible(row, !teleport);
    }

    m_host->setPlaceholderText(ssh ? tr("127.0.0.1 — as seen from the SSH host") : QString());
    if (ssh)
    {
        checkSshAgent();
    }
    if (teleport)
    {
        m_database->setPlaceholderText(tr("required by most teleport db configs"));
        api()->call(
            "conn", "TeleportStatus", {}, this,
            [this](const QJsonValue &res, const QString &err)
            {
                if (!err.isEmpty())
                {
                    m_teleportNote->setText(err);
                    return;
                }
                const QJsonObject o = res.toObject();
                m_teleportNote->setText(
                    o.value("loggedIn").toBool()
                        ? tr("tsh: logged in as %1").arg(o.value("user").toString())
                        : o.value("detail").toString()
                );
            }
        );
    }
    else
    {
        m_database->setPlaceholderText(tr("optional default schema"));
    }
    adjustSize();
}

void ConnectionsDialog::pickColor()
{
    const QColor current =
        m_color->text().isEmpty() ? theme::current().info : QColor(m_color->text());
    const QColor picked = QColorDialog::getColor(current, this, tr("Accent Colour"));
    if (picked.isValid())
    {
        m_color->setText(picked.name());
    }
}

void ConnectionsDialog::updateSwatch()
{
    const QString hex = m_color->text().trimmed();
    const QColor c = hex.isEmpty() ? QColor() : QColor(hex);
    const QString fill = c.isValid() ? c.name() : QStringLiteral("transparent");
    m_swatch->setStyleSheet(QString("QPushButton { background: %1; border: 1px solid %2; "
                                    "border-radius: 6px; }")
                                .arg(fill, theme::current().input.name()));
    m_swatch->setText(c.isValid() ? QString() : QStringLiteral("?"));
}

void ConnectionsDialog::checkSshAgent()
{
    api()->call(
        "conn", "SSHAgentStatus", {}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                m_agentNote->setText(err);
                return;
            }
            const QJsonObject o = res.toObject();
            if (!o.value("found").toBool())
            {
                m_agentNote->setText(o.value("detail").toString());
                return;
            }
            const int keys = o.value("keys").toInt();
            m_agentNote->setText(
                keys > 0
                    ? tr("ssh-agent: %1 key(s) (%2)").arg(keys).arg(o.value("socket").toString())
                    : o.value("detail").toString()
            );
            m_sshKeyFile->setPlaceholderText(
                keys > 0 ? tr("optional — the agent holds a usable key")
                         : tr("path to a private key")
            );
        }
    );
}

void ConnectionsDialog::browseTeleport()
{
    api()->call(
        "conn", "TeleportDatabases", {}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                QMessageBox::warning(this, tr("Teleport"), err);
                return;
            }
            QMenu menu(this);
            const QJsonArray arr = res.toArray();
            if (arr.isEmpty())
            {
                menu.addAction(tr("No database resources visible."))->setEnabled(false);
            }
            for (const auto &v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString name = o.value("name").toString();
                const QString protocol = o.value("protocol").toString();
                QAction *a = menu.addAction(name + "   " + protocol);
                a->setEnabled(protocol == "mysql");
                a->setToolTip(
                    protocol == "mysql" ? o.value("description").toString()
                                        : tr("%1 — not MySQL").arg(protocol)
                );
                connect(
                    a, &QAction::triggered, this, [this, name]() { m_teleportDb->setText(name); }
                );
            }
            menu.exec(m_teleportDb->mapToGlobal(m_teleportDb->rect().bottomLeft()));
        }
    );
}

void ConnectionsDialog::refreshPasswordState(const QString &id)
{
    if (id.isEmpty())
    {
        m_password->setPlaceholderText(tr("required"));
        m_removePassword->setVisible(false);
        return;
    }
    m_removePassword->setVisible(false);
    api()->call(
        "conn", "HasPassword", {id}, this,
        [this, id](const QJsonValue &res, const QString &err)
        {
            const bool stored = err.isEmpty() && res.toBool();
            m_password->setPlaceholderText(
                stored ? tr("stored — type to replace") : tr("none stored")
            );
            m_removePassword->setVisible(stored);
        }
    );
}

void ConnectionsDialog::removePassword()
{
    const QString id = m_editing.value("id").toString();
    if (id.isEmpty())
    {
        return;
    }
    if (QMessageBox::warning(
            this, tr("Remove password"),
            tr("Delete the stored password for \"%1\"? The connection will not "
               "open again until you set a new one.")
                .arg(m_editing.value("name").toString()),
            QMessageBox::Cancel | QMessageBox::Yes
        ) != QMessageBox::Yes)
    {
        return;
    }
    api()->call(
        "conn", "SetPassword", {id, QString()}, this,
        [this, id](const QJsonValue &, const QString &err)
        {
            if (!err.isEmpty())
            {
                QMessageBox::warning(this, tr("Remove password"), err);
                return;
            }
            m_password->clear();
            m_passwordEdited = false;
            refreshPasswordState(id);
        }
    );
}

void ConnectionsDialog::editProfile(const QJsonObject &profile)
{
    m_editing = profile;
    m_name->setText(profile.value("name").toString());
    m_color->setText(profile.value("color").toString());
    QString method = profile.value("method").toString();
    if (method.isEmpty())
    {
        method = profile.value("teleport").toBool() ? "teleport" : "tcp";
    }
    const int mix = m_method->findData(method);
    m_method->setCurrentIndex(mix >= 0 ? mix : 0);
    m_host->setText(profile.value("host").toString());
    m_port->setValue(profile.value("port").toInt(3306));
    m_user->setText(profile.value("user").toString());
    m_database->setText(profile.value("database").toString());
    m_password->clear(); // never round-tripped through the UI
    m_passwordEdited = false;
    refreshPasswordState(profile.value("id").toString());
    const QString tls = profile.value("tlsMode").toString();
    m_tls->setCurrentText(tls.isEmpty() ? "preferred" : tls);
    m_sshHost->setText(profile.value("sshHost").toString());
    m_sshPort->setValue(profile.value("sshPort").toInt(22));
    m_sshUser->setText(profile.value("sshUser").toString());
    m_sshKeyFile->setText(profile.value("sshKeyFile").toString());
    m_teleportDb->setText(profile.value("teleportDb").toString());

    // A fresh profile starts with the local-dev defaults instead of blanks.
    if (profile.isEmpty())
    {
        m_host->setText(QStringLiteral("127.0.0.1"));
        m_user->setText(QStringLiteral("root"));
    }

    updateSwatch();
    applyMethod(method);
    showPage(Page::Editor);
}

void ConnectionsDialog::save()
{
    const QString method = m_method->currentData().toString();
    QJsonObject c = m_editing;
    c.insert("name", m_name->text().trimmed());
    c.insert("color", m_color->text().trimmed());
    c.insert("method", method);
    c.insert("host", m_host->text().trimmed());
    c.insert("port", m_port->value());
    c.insert("user", m_user->text().trimmed());
    c.insert("database", m_database->text().trimmed());
    c.insert("tlsMode", m_tls->currentText());
    c.insert("sshHost", m_sshHost->text().trimmed());
    c.insert("sshPort", m_sshPort->value());
    c.insert("sshUser", m_sshUser->text().trimmed());
    c.insert("sshKeyFile", m_sshKeyFile->text().trimmed());
    c.insert("teleport", method == "teleport");
    c.insert("teleportDb", m_teleportDb->text().trimmed());
    if (!c.contains("id"))
    {
        c.insert("id", QString());
    }

    // No name check: the backend derives one from the connection details.

    // Two calls on purpose: Save cannot express "clear the password" (it reads
    // an empty string as "leave it alone"), so the password goes through
    // SetPassword, and only when the user actually typed in the field.
    const QString typed = m_password->text();
    const bool edited = m_passwordEdited;
    // The context is api() — an object that outlives any dialog — because the
    // password write is chained behind the Save reply and must fire even if
    // the dialog is closed in between; a dialog context would drop the whole
    // callback and lose the typed password without saying so. The QPointer
    // guards only the UI updates.
    QPointer<ConnectionsDialog> self(this);
    api()->call(
        "conn", "Save", {c, QString()}, api(),
        [self, typed, edited](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                if (self)
                {
                    QMessageBox::warning(self, tr("Save"), err);
                }
                return;
            }
            const QString id = res.toObject().value("id").toString();
            if (!edited || id.isEmpty())
            {
                if (self)
                {
                    self->m_password->clear();
                    self->m_passwordEdited = false;
                    emit self->savedChanged();
                }
                return;
            }
            // Fires whether or not the dialog survived; only the UI
            // update is conditional.
            api()->call(
                "conn", "SetPassword", {id, typed}, api(),
                [self](const QJsonValue &, const QString &pwErr)
                {
                    if (!self)
                    {
                        return;
                    }
                    if (!pwErr.isEmpty())
                    {
                        QMessageBox::warning(self, tr("Save"), pwErr);
                    }
                    self->m_password->clear();
                    self->m_passwordEdited = false;
                    emit self->savedChanged();
                }
            );
        }
    );
}

// ---------------------------------------------------------- quick connect ---

void ConnectionsDialog::buildQuickPage(QVBoxLayout *root)
{
    m_quickPage = new QWidget;
    auto *lay = new QVBoxLayout(m_quickPage);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    lay->addWidget(weightedLabel(tr("Quick Connect"), QFont::Medium));
    m_quickStatus = note(tr("checking tsh…"));
    lay->addWidget(m_quickStatus);
    m_quickNote = note();
    lay->addWidget(m_quickNote);

    m_quickList = new QListWidget;
    m_quickList->setSelectionMode(QAbstractItemView::SingleSelection);
    // Scroll past this height rather than grow the dialog unbounded; the
    // 1-9/a-z keys keep long lists reachable without the mouse.
    m_quickList->setMinimumHeight(theme::scaledPx(12.0));
    m_quickList->setMaximumHeight(theme::scaledPx(24.0));
    // No focus: the list must never swallow the key presses (type-to-search),
    // and a single click acts immediately anyway.
    m_quickList->setFocusPolicy(Qt::NoFocus);
    handCursorOnRows(m_quickList);
    connect(m_quickList, &QListWidget::itemClicked, this, &ConnectionsDialog::pickQuick);
    lay->addWidget(m_quickList, 1);

    m_quickForm = new QFormLayout;
    m_quickForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_quickForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // Free-text user, shown only when the resource allows any user ("*").
    // Enter connects with what was typed. Click focus, like the database
    // field below: with tab/auto focus the pick keys would land in a field
    // instead of the list.
    m_quickUserEdit = new QLineEdit;
    m_quickUserEdit->setPlaceholderText(tr("any user allowed — type one, Enter connects"));
    m_quickUserEdit->setFocusPolicy(Qt::ClickFocus);
    connect(
        m_quickUserEdit, &QLineEdit::returnPressed, this,
        [this]() { connectQuick(m_quickUserEdit->text().trimmed()); }
    );
    m_quickUserRow = m_quickForm->rowCount();
    m_quickForm->addRow(tr("User"), m_quickUserEdit);

    m_quickDatabase = new QLineEdit;
    m_quickDatabase->setPlaceholderText(tr("required by most teleport db configs"));
    m_quickDatabase->setFocusPolicy(Qt::ClickFocus);
    m_quickForm->addRow(tr("Database"), m_quickDatabase);
    lay->addLayout(m_quickForm);

    auto *box = new QDialogButtonBox;
    auto *back = box->addButton(tr("Back"), QDialogButtonBox::RejectRole);
    connect(
        back, &QPushButton::clicked, this,
        [this]()
        {
            if (m_quickStep == QuickStep::Users)
            {
                listQuickResources();
            }
            else
            {
                showPage(Page::List);
            }
        }
    );
    lay->addWidget(box);

    m_quickPage->setVisible(false);
    root->addWidget(m_quickPage);
}

void ConnectionsDialog::openQuick()
{
    m_quickDBs.clear();
    m_quickTraitUsers.clear();
    m_quickList->clear();
    m_quickStep = QuickStep::Resources;
    m_quickForm->setRowVisible(m_quickUserRow, false);
    m_quickStatus->setText(tr("checking tsh…"));
    m_quickNote->clear();
    m_quickUserEdit->clear();
    showPage(Page::Quick);
    // Focus on the dialog itself, so the pick keys reach keyPressEvent
    // instead of a field.
    setFocus();

    // Status first, like the editor's Teleport method does: a missing login
    // should say "run tsh login", not fail inside the resource listing.
    api()->call(
        "conn", "TeleportStatus", {}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!m_quickPage->isVisible())
            {
                return; // user backed out before the reply
            }
            if (!err.isEmpty())
            {
                m_quickStatus->setText(err);
                return;
            }
            const QJsonObject o = res.toObject();
            if (!o.value("loggedIn").toBool())
            {
                m_quickStatus->setText(o.value("detail").toString());
                return;
            }
            m_quickStatus->setText(tr("tsh: logged in as %1").arg(o.value("username").toString()));
            for (const auto &u : o.value("dbUsers").toArray())
            {
                m_quickTraitUsers << u.toString();
            }
            fetchQuickResources();
        }
    );
}

void ConnectionsDialog::fetchQuickResources()
{
    m_quickNote->setText(tr("loading resources…"));
    api()->call(
        "conn", "TeleportDatabases", {}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!m_quickPage->isVisible())
            {
                return;
            }
            if (!err.isEmpty())
            {
                m_quickNote->setText(err);
                return;
            }
            m_quickDBs.clear();
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                QuickDB db;
                db.name = o.value("name").toString();
                db.protocol = o.value("protocol").toString();
                for (const auto &u : o.value("users").toArray())
                {
                    db.users << u.toString();
                }
                m_quickDBs.append(db);
            }
            listQuickResources();
        }
    );
}

namespace
{

// The pick key for the nth selectable item: 1-9, then a-z; items beyond that
// are click-only.
QString quickKeyLabel(int index)
{
    if (index < quickDigitKeys)
    {
        return QString::number(index + 1);
    }
    if (index < quickDigitKeys + quickLetterKeys)
    {
        return QString(QChar('a' + index - quickDigitKeys));
    }
    return {};
}

} // namespace

void ConnectionsDialog::listQuickResources()
{
    m_quickStep = QuickStep::Resources;
    m_quickResource.clear();
    m_quickList->clear();
    m_quickForm->setRowVisible(m_quickUserRow, false);
    setFocus();

    const AppPalette &pal = theme::current();
    int keyed = 0;
    for (const QuickDB &db : m_quickDBs)
    {
        auto *item = new QListWidgetItem(m_quickList);
        if (db.protocol != "mysql")
        {
            item->setText(tr("%1 — %2, not MySQL").arg(db.name, db.protocol));
            item->setFlags(Qt::NoItemFlags);
            continue;
        }
        const QString key = quickKeyLabel(keyed);
        item->setText(key.isEmpty() ? db.name : key + "   " + db.name);
        item->setIcon(icons::icon("database", pal.mutedFg, theme::scaledPx(1.0)));
        item->setData(quickNameRole, db.name);
        item->setData(quickKeyRole, keyed);
        ++keyed;
    }

    m_quickNote->setText(
        keyed == 0 ? tr("No MySQL resources visible — check `tsh db ls`.")
                   : tr("Press an item's key (or click it) to pick the resource.")
    );
    layout()->activate();
    adjustSize();
}

void ConnectionsDialog::listQuickUsers()
{
    m_quickStep = QuickStep::Users;
    m_quickList->clear();
    setFocus();

    QStringList users;
    for (const QuickDB &db : m_quickDBs)
    {
        if (db.name == m_quickResource)
        {
            users = db.users;
            break;
        }
    }
    const bool anyUser = users.removeAll("*") > 0;
    if (anyUser)
    {
        // "*" names nobody — the trait users on the tsh certs are the best
        // candidates, and the free-text field covers the rest.
        for (const QString &u : m_quickTraitUsers)
        {
            if (!users.contains(u))
            {
                users << u;
            }
        }
    }

    const AppPalette &pal = theme::current();
    for (int i = 0; i < users.size(); ++i)
    {
        auto *item = new QListWidgetItem(m_quickList);
        const QString key = quickKeyLabel(i);
        item->setText(key.isEmpty() ? users.at(i) : key + "   " + users.at(i));
        item->setIcon(icons::icon("users", pal.mutedFg, theme::scaledPx(1.0)));
        item->setData(quickNameRole, users.at(i));
        item->setData(quickKeyRole, i);
    }

    m_quickForm->setRowVisible(m_quickUserRow, anyUser);
    if (anyUser && users.isEmpty())
    {
        // The field is the only path here — focus it so typing works at once.
        m_quickUserEdit->setFocus();
    }

    m_quickNote->setText(
        users.isEmpty() && !anyUser
            ? tr("%1: no users on the resource — check its Teleport config.").arg(m_quickResource)
            : tr("%1: press a user's key (or click) to connect.").arg(m_quickResource)
    );
    layout()->activate();
    adjustSize();
}

void ConnectionsDialog::pickQuick(QListWidgetItem *item)
{
    if (item == nullptr)
    {
        return;
    }
    const QString value = item->data(quickNameRole).toString();
    if (value.isEmpty())
    {
        return;
    }
    if (m_quickStep == QuickStep::Resources)
    {
        m_quickResource = value;
        listQuickUsers();
        return;
    }
    connectQuick(value);
}

bool ConnectionsDialog::handleQuickKey(int key)
{
    int index = -1;
    if (key >= Qt::Key_1 && key <= Qt::Key_9)
    {
        index = key - Qt::Key_1;
    }
    else if (key >= Qt::Key_A && key <= Qt::Key_Z)
    {
        index = quickDigitKeys + (key - Qt::Key_A);
    }
    else
    {
        return false;
    }

    for (int i = 0; i < m_quickList->count(); ++i)
    {
        QListWidgetItem *item = m_quickList->item(i);
        if (item->data(quickKeyRole).isValid() && item->data(quickKeyRole).toInt() == index)
        {
            pickQuick(item);
            return true;
        }
    }
    return false;
}

void ConnectionsDialog::keyPressEvent(QKeyEvent *event)
{
    // Only unmodified keys pick items; a focused text field never lets its
    // keys bubble up here, so typing a database name stays typing.
    if (m_quickPage->isVisible() && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier &&
        handleQuickKey(event->key()))
    {
        return;
    }
    QDialog::keyPressEvent(event);
}

void ConnectionsDialog::connectQuick(const QString &user)
{
    if (m_quickResource.isEmpty() || !m_quickList->isEnabled())
    {
        return; // no resource picked, or a connect is already in flight
    }
    m_quickList->setEnabled(false);
    m_quickNote->setText(tr("connecting to %1…").arg(m_quickResource));
    api()->call(
        "conn", "SaveQuick", {m_quickResource, user, m_quickDatabase->text().trimmed()}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            m_quickList->setEnabled(true);
            if (!err.isEmpty())
            {
                m_quickNote->setText(err);
                return;
            }
            const QString id = res.toObject().value("id").toString();
            // The shell refreshes its list first so the ephemeral profile can
            // label the server tab, then opens the connection.
            emit savedChanged();
            emit connectRequested(id);
            accept();
        }
    );
}
