#pragma once
// Connection manager: the saved-profile list, an editor whose fields follow
// the connection method (TCP / over SSH / Teleport), and a quick-connect page
// that opens a Teleport resource ad hoc without saving a profile. Passwords
// go straight to the backend's Save (keyring, or the dev fallback file) and
// are never held in app state — same rule the web dialog followed.
#include <QDialog>
#include <QJsonObject>
#include <QKeySequence>
#include <QVector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QShortcut;
class QSpinBox;
class QVBoxLayout;
class QWidget;

// The application-wide quick-connect binding, defined once so the shortcut
// and the shortcuts dialog cannot drift. Qt::CTRL means ⌘ on macOS, where
// ⌘Q must stay Quit — the mac binding is the physical Control key instead.
QKeySequence quickConnectShortcut();

class ConnectionsDialog : public QDialog
{
    Q_OBJECT
public:
    // The page the dialog opens on (Ctrl+Q jumps straight to quick connect).
    enum class Start
    {
        List,
        QuickConnect,
    };

    ConnectionsDialog(
        const QVector<QJsonObject> &saved, const QVector<QString> &openIDs,
        Start start = Start::List, QWidget *parent = nullptr
    );

    void setSaved(const QVector<QJsonObject> &saved, const QVector<QString> &openIDs);

signals:
    void connectRequested(const QString &connID);
    void disconnectRequested(const QString &connID);
    void savedChanged();

private:
    enum class Page
    {
        List,
        Editor,
        Quick,
    };

    void buildList();
    void buildQuickPage(QVBoxLayout *root);
    // Swap two rows and persist the new order (conn.Reorder).
    void moveProfile(int from, int to);
    void editProfile(const QJsonObject &profile); // empty object = new
    void applyMethod(const QString &method);
    void pickColor();
    void updateSwatch();
    void showPage(Page page);
    // Escape must back out of the editor and quick pages like Cancel does,
    // and only close the dialog from the list page.
    void reject() override;
    void save();
    void checkSshAgent();
    void browseTeleport();
    void refreshPasswordState(const QString &id);
    void removePassword();
    // Quick connect: two keyed steps — pick a resource, pick a user — each
    // item reachable by its 1-9/a-z key or a click; the user pick connects.
    void openQuick();
    void fetchQuickResources();
    void listQuickResources();
    void listQuickUsers();
    void pickQuick(class QListWidgetItem *item);
    bool handleQuickKey(int key);
    void connectQuick(const QString &user);

    QVector<QJsonObject> m_saved;
    QVector<QString> m_openIDs;

    QWidget *m_listPage;
    QWidget *m_editorPage;
    QVBoxLayout *m_listLayout;

    // Editor
    QJsonObject m_editing;
    QLineEdit *m_name, *m_color, *m_host, *m_user, *m_database, *m_password;
    QSpinBox *m_port;
    QComboBox *m_method, *m_tls;
    QLineEdit *m_sshHost, *m_sshUser, *m_sshKeyFile;
    QSpinBox *m_sshPort;
    QLineEdit *m_teleportDb;
    QLabel *m_agentNote, *m_teleportNote;
    // One form layout for every field; the method-specific rows are shown and
    // hidden by index. Nested per-method forms each had their own label
    // column, which is why the labels did not line up.
    class QFormLayout *m_form = nullptr;

    struct MethodRows
    {
        QVector<int> tcp;      // Host / Port
        QVector<int> ssh;      // SSH host, port, user, key file, agent note
        QVector<int> teleport; // resource, status note
    } m_rows;

    QPushButton *m_swatch = nullptr;
    QPushButton *m_removePassword;
    // An untouched field means "leave the stored password alone"; an emptied
    // one means nothing, which is why removing needs its own control. Tracked
    // rather than inferred so typing a value that happens to match anything
    // is never mistaken for "unchanged".
    bool m_passwordEdited = false;

    // Single-key shortcuts for the list page's actions (N = new, Q = quick).
    // Toggled with the page: enabled window-wide they would swallow the
    // letters while typing in the other pages' fields.
    QShortcut *m_newKey = nullptr;
    QShortcut *m_quickKey = nullptr;

    // Quick connect page
    enum class QuickStep
    {
        Resources,
        Users,
    };

    // One Teleport database resource from the single tsh fetch; kept so the
    // users step and Back need no refetch.
    struct QuickDB
    {
        QString name;
        QString protocol;
        QStringList users;
    };

    QWidget *m_quickPage = nullptr;
    QListWidget *m_quickList = nullptr;
    QLineEdit *m_quickUserEdit = nullptr; // free text, only when "*" is allowed
    QLineEdit *m_quickDatabase = nullptr;
    QLabel *m_quickStatus = nullptr;    // tsh session line
    QLabel *m_quickNote = nullptr;      // step instruction / errors
    QFormLayout *m_quickForm = nullptr; // user + database rows
    int m_quickUserRow = -1;            // form row of the free-text user field
    QuickStep m_quickStep = QuickStep::Resources;
    QString m_quickResource;       // picked on the resources step
    QStringList m_quickTraitUsers; // db users the tsh certs carry
    QVector<QuickDB> m_quickDBs;

protected:
    // Routes 1-9/a-z to the quick page's keyed lists. Text fields keep their
    // keys — a focused QLineEdit consumes them before they get here.
    void keyPressEvent(class QKeyEvent *event) override;
};
