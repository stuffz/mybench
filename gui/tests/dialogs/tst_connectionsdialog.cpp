#include "dialogs/connectionsdialog.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"

#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLatin1String>
#include <QLayoutItem>
#include <QLineEdit>
#include <QList>
#include <QObject>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QString>
#include <QTest>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <functional>

namespace
{

constexpr auto SavePath = "/rpc/conn/Save";
constexpr auto SetPasswordPath = "/rpc/conn/SetPassword";
constexpr auto HasPasswordPath = "/rpc/conn/HasPassword";
constexpr auto AgentStatusPath = "/rpc/conn/SSHAgentStatus";
constexpr auto TeleportStatusPath = "/rpc/conn/TeleportStatus";
constexpr auto ReorderPath = "/rpc/conn/Reorder";

// Obvious nonsense on purpose: this file drives the one dialog that moves a
// password to the keyring, so nothing that would open a real server may be
// written down here.
constexpr auto Placeholder = "placeholder-not-a-password";

constexpr int DefaultPort = 3306;
constexpr int DefaultSshPort = 22;

// Spins the event loop until the condition holds, or gives up. A loop rather
// than QTRY_VERIFY, which cannot be used from a helper: it returns from its
// own function on failure, which here would only skip the wait.
bool waitUntil(const std::function<bool()> &done)
{
    for (int waited = 0; waited < 5000; waited += 10)
    {
        if (done())
        {
            return true;
        }
        QTest::qWait(10);
    }
    return false;
}

// The calls that went to one RPC method. Opening the editor asks after the
// stored password and, for two of the three methods, after ssh-agent or tsh,
// so an assertion about the save picks out its own conversation.
QList<StubBackend::Request> callsTo(const StubBackend &backend, const char *path)
{
    QList<StubBackend::Request> out;
    for (const StubBackend::Request &req : backend.requests())
    {
        if (req.path == QLatin1String(path))
        {
            out.append(req);
        }
    }
    return out;
}

// The editor's form. The quick-connect page has one too, with rows named User
// and Database just like this one, so the two are told apart by what they hang
// off: the editor's form is installed on a widget, the quick one on a layout.
QFormLayout *editorForm(const ConnectionsDialog &dlg)
{
    for (QFormLayout *form : dlg.findChildren<QFormLayout *>())
    {
        if (qobject_cast<QWidget *>(form->parent()))
        {
            return form;
        }
    }
    return nullptr;
}

int rowOf(QFormLayout *form, const QString &label)
{
    if (!form)
    {
        return -1;
    }
    for (int row = 0; row < form->rowCount(); ++row)
    {
        QLayoutItem *item = form->itemAt(row, QFormLayout::LabelRole);
        auto *text = item ? qobject_cast<QLabel *>(item->widget()) : nullptr;
        if (text && text->text() == label)
        {
            return row;
        }
    }
    return -1;
}

QWidget *fieldAt(const ConnectionsDialog &dlg, const QString &label)
{
    QFormLayout *form = editorForm(dlg);
    const int row = rowOf(form, label);
    if (row < 0)
    {
        return nullptr;
    }
    QLayoutItem *item = form->itemAt(row, QFormLayout::FieldRole);
    return item ? item->widget() : nullptr;
}

// The rows that pair a field with a button wrap both in a plain widget, so the
// field is dug out rather than cast.
QLineEdit *edit(const ConnectionsDialog &dlg, const QString &label)
{
    QWidget *field = fieldAt(dlg, label);
    if (!field)
    {
        return nullptr;
    }
    if (auto *line = qobject_cast<QLineEdit *>(field))
    {
        return line;
    }
    return field->findChild<QLineEdit *>();
}

QSpinBox *spin(const ConnectionsDialog &dlg, const QString &label)
{
    return qobject_cast<QSpinBox *>(fieldAt(dlg, label));
}

QComboBox *combo(const ConnectionsDialog &dlg, const QString &label)
{
    return qobject_cast<QComboBox *>(fieldAt(dlg, label));
}

// Whether the method in force offers this field at all.
bool rowShown(const ConnectionsDialog &dlg, const QString &label)
{
    QFormLayout *form = editorForm(dlg);
    const int row = rowOf(form, label);
    return row >= 0 && form->isRowVisible(row);
}

// In list order: the profile rows are built top to bottom, so the nth Edit or
// Connect button belongs to the nth profile.
QList<QPushButton *> buttons(const ConnectionsDialog &dlg, const QString &text)
{
    QList<QPushButton *> out;
    for (QPushButton *b : dlg.findChildren<QPushButton *>())
    {
        if (b->text() == text)
        {
            out.append(b);
        }
    }
    return out;
}

QPushButton *button(const ConnectionsDialog &dlg, const QString &text)
{
    const QList<QPushButton *> found = buttons(dlg, text);
    return found.isEmpty() ? nullptr : found.first();
}

// The reorder arrows carry an icon and no text, so the tooltip names them.
QList<QPushButton *> arrows(const ConnectionsDialog &dlg, const QString &tip)
{
    QList<QPushButton *> out;
    for (QPushButton *b : dlg.findChildren<QPushButton *>())
    {
        if (b->toolTip() == tip)
        {
            out.append(b);
        }
    }
    return out;
}

// Every field conn.Save is handed, on a form that has been given nothing. Each
// slot overwrites the ones it fills in, so a field that stops being sent, or
// that arrives under a new name, fails the comparison instead of quietly
// costing someone their saved connection.
QJsonObject savePayload()
{
    return QJsonObject{
        {"id", QString()},         {"name", QString()},         {"color", QString()},
        {"method", "tcp"},         {"host", QString()},         {"port", DefaultPort},
        {"user", QString()},       {"database", QString()},     {"tlsMode", "preferred"},
        {"sshHost", QString()},    {"sshPort", DefaultSshPort}, {"sshUser", QString()},
        {"sshKeyFile", QString()}, {"teleport", false},         {"teleportDb", QString()},
    };
}

// A profile with every field the dialog knows filled in, so anything it fails
// to overwrite turns up under the profile shown next.
QJsonObject sshProfile()
{
    return QJsonObject{
        {"id", "c1"},
        {"name", "Reporting"},
        {"color", "#3366ff"},
        {"method", "ssh"},
        {"host", "10.0.0.9"},
        {"port", 3307},
        {"user", "reporter"},
        {"database", "reports"},
        {"tlsMode", "required"},
        {"sshHost", "bastion.example.invalid"},
        {"sshPort", 2222},
        {"sshUser", "ops"},
        {"sshKeyFile", "/home/ops/.ssh/id_ed25519"},
        {"teleport", false},
        {"teleportDb", "left-over-resource"},
    };
}

// The other half of the leak hunt: everything the form would have to default
// is simply missing from the stored JSON.
QJsonObject sparseProfile()
{
    return QJsonObject{{"id", "c2"}, {"name", "Scratch"}, {"database", "scratch"}};
}

// Quick-connect writes one of these so the shell can label a tab; the dialog
// manages saved profiles, so it must not be listed here.
QJsonObject ephemeralProfile()
{
    return QJsonObject{
        {"id", "q1"}, {"name", "prod-mysql"}, {"method", "teleport"}, {"ephemeral", true}
    };
}

} // namespace

// This dialog owns every stored credential and every way of reaching a server,
// so what is pinned here is the conn.Save payload field by field, and the rule
// that opening a second profile leaves nothing of the first one in the form.
// Delete, Remove password and both Browse buttons run a modal of their own and
// are unreachable under offscreen; so is the failure branch of Save, which
// answers an error with a QMessageBox.
class TestConnectionsDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void theListShowsTheSavedProfilesAndSkipsEphemeralOnes();
    void connectingAsksTheShellForTheProfileAndCloses();
    void aNewProfileOpensOnTheLocalDevDefaults();
    void savingANewProfileSendsEveryFieldOfTheForm();
    void aTypedPasswordFollowsTheSaveAsItsOwnCall();
    void aStoredProfileFillsTheFormAndDefaultsWhatIsMissing();
    void aLegacyTeleportFlagStillPicksTheTeleportMethod();
    void switchingProfilesLeavesNothingOfThePreviousOne();
    void anEmptyHostAndNameAreSavedWithoutComplaint();
    void thePortFieldClampsAndRollsBackNonsense();
    void theSshMethodSendsItsFieldsAndChecksTheAgent();
    void theSshFieldsStayInThePayloadAfterSwitchingToTcp();
    void theTeleportMethodSetsTheFlagAndHidesTheEndpoint();
    void theRemoveButtonFollowsWhetherAPasswordIsStored();
    void reorderingSendsEveryIdAsOneArgument();

private:
    StubBackend m_backend;
};

void TestConnectionsDialog::initTestCase()
{
    // The list rows are built from theme::current() as the dialog is
    // constructed, so a palette has to be installed before the first one.
    theme::apply(theme::defaultApp, 13);
}

void TestConnectionsDialog::init()
{
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
    m_backend.clearRequests();
    // Stands in for "no stored password", "no ssh-agent" and "tsh not logged
    // in" at once: every status the editor asks after reads false. A slot that
    // wants another reply sets it before the call it cares about.
    m_backend.replyWithResult(QJsonValue(false));
}

void TestConnectionsDialog::cleanup()
{
    // A dialog destroyed as its slot returns can still have a reply in flight;
    // draining it here keeps it out of the next slot's requests.
    api()->flush(2000);
}

void TestConnectionsDialog::theListShowsTheSavedProfilesAndSkipsEphemeralOnes()
{
    ConnectionsDialog dlg({sshProfile(), sparseProfile(), ephemeralProfile()}, {"c2"});

    QCOMPARE(buttons(dlg, QStringLiteral("Edit")).size(), 2);

    // The open profile offers the way out of the connection it already has.
    QCOMPARE(buttons(dlg, QStringLiteral("Connect")).size(), 1);
    QCOMPARE(buttons(dlg, QStringLiteral("Disconnect")).size(), 1);

    // The list page reads only what it was handed: nothing here costs a round
    // trip until a profile is opened for editing.
    api()->flush(2000);
    QTest::qWait(50);
    QVERIFY(m_backend.requests().isEmpty());
}

void TestConnectionsDialog::connectingAsksTheShellForTheProfileAndCloses()
{
    ConnectionsDialog dlg({sshProfile(), sparseProfile()}, {});
    QSignalSpy asked(&dlg, &ConnectionsDialog::connectRequested);

    QPushButton *open = buttons(dlg, QStringLiteral("Connect")).value(1);
    QVERIFY(open);
    open->click();

    // The id, not the row, is what the shell is given: the list reorders.
    QCOMPARE(asked.size(), 1);
    QCOMPARE(asked.at(0).at(0).toString(), QStringLiteral("c2"));
    QCOMPARE(dlg.result(), int(QDialog::Accepted));
}

void TestConnectionsDialog::aNewProfileOpensOnTheLocalDevDefaults()
{
    ConnectionsDialog dlg({}, {});
    QPushButton *fresh = button(dlg, QStringLiteral("New Connection (&N)"));
    QVERIFY(fresh);
    fresh->click();

    QCOMPARE(edit(dlg, QStringLiteral("Host"))->text(), QStringLiteral("127.0.0.1"));
    QCOMPARE(edit(dlg, QStringLiteral("User"))->text(), QStringLiteral("root"));
    QCOMPARE(spin(dlg, QStringLiteral("Port"))->value(), DefaultPort);
    QCOMPARE(spin(dlg, QStringLiteral("SSH Port"))->value(), DefaultSshPort);
    QCOMPARE(combo(dlg, QStringLiteral("TLS"))->currentText(), QStringLiteral("preferred"));
    QCOMPARE(combo(dlg, QStringLiteral("Method"))->currentData().toString(), QStringLiteral("tcp"));

    // TCP is what a new profile opens on, so only the endpoint rows are up.
    QVERIFY(rowShown(dlg, QStringLiteral("Host")));
    QVERIFY(!rowShown(dlg, QStringLiteral("SSH Host")));
    QVERIFY(!rowShown(dlg, QStringLiteral("Resource")));

    QLineEdit *password = edit(dlg, QStringLiteral("Password"));
    QVERIFY(password);
    QVERIFY(password->text().isEmpty());
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    QCOMPARE(password->placeholderText(), QStringLiteral("required"));

    // Nothing is stored yet, so there is nothing to remove and nobody to ask.
    QVERIFY(button(dlg, QStringLiteral("Remove"))->isHidden());
    api()->flush(2000);
    QTest::qWait(50);
    QVERIFY(callsTo(m_backend, HasPasswordPath).isEmpty());
}

void TestConnectionsDialog::savingANewProfileSendsEveryFieldOfTheForm()
{
    ConnectionsDialog dlg({}, {});
    button(dlg, QStringLiteral("New Connection (&N)"))->click();

    edit(dlg, QStringLiteral("Name"))->setText(QStringLiteral("Staging"));
    edit(dlg, QStringLiteral("Color"))->setText(QStringLiteral("#112233"));
    edit(dlg, QStringLiteral("Host"))->setText(QStringLiteral("db.example.invalid"));
    edit(dlg, QStringLiteral("User"))->setText(QStringLiteral("app_ro"));
    edit(dlg, QStringLiteral("Database"))->setText(QStringLiteral("shop"));
    spin(dlg, QStringLiteral("Port"))->setValue(3307);
    combo(dlg, QStringLiteral("TLS"))->setCurrentText(QStringLiteral("required"));

    QSignalSpy changed(&dlg, &ConnectionsDialog::savedChanged);
    m_backend.replyWithResult(QJsonObject{{"id", "c9"}});
    m_backend.clearRequests();
    button(dlg, QStringLiteral("Save"))->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, SavePath).size() == 1; }));
    const StubBackend::Request req = callsTo(m_backend, SavePath).at(0);
    QCOMPARE(req.verb, QStringLiteral("POST"));

    QJsonObject want = savePayload();
    want.insert("name", "Staging");
    want.insert("color", "#112233");
    want.insert("host", "db.example.invalid");
    want.insert("port", 3307);
    want.insert("user", "app_ro");
    want.insert("database", "shop");
    want.insert("tlsMode", "required");

    // The second argument is Save's password parameter, and it is always
    // empty: an empty string there means "leave the stored one alone", which
    // is why a typed password needs the separate call below.
    QCOMPARE(req.args, QJsonArray({want, QString()}));

    // Nobody touched the password field, so the stored secret is not rewritten
    // even though the reply named an id to write it against.
    QVERIFY(waitUntil([&changed] { return changed.size() == 1; }));
    api()->flush(2000);
    QTest::qWait(50);
    QVERIFY(callsTo(m_backend, SetPasswordPath).isEmpty());
}

void TestConnectionsDialog::aTypedPasswordFollowsTheSaveAsItsOwnCall()
{
    ConnectionsDialog dlg({}, {});
    button(dlg, QStringLiteral("New Connection (&N)"))->click();

    QLineEdit *password = edit(dlg, QStringLiteral("Password"));
    QVERIFY(password);
    // insert() is what typing does; setText() would leave the field looking
    // filled while the dialog still counted it untouched.
    password->insert(QString::fromLatin1(Placeholder));

    QSignalSpy changed(&dlg, &ConnectionsDialog::savedChanged);
    m_backend.replyWithResult(QJsonObject{{"id", "c9"}});
    m_backend.clearRequests();
    button(dlg, QStringLiteral("Save"))->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, SetPasswordPath).size() == 1; }));
    // Against the id the reply named, not against the empty one the form was
    // opened on: a new profile has no id until Save answers with one.
    QCOMPARE(
        callsTo(m_backend, SetPasswordPath).at(0).args,
        QJsonArray({QStringLiteral("c9"), QString::fromLatin1(Placeholder)})
    );

    // The profile itself never carries the secret, in either argument.
    const StubBackend::Request saved = callsTo(m_backend, SavePath).at(0);
    QVERIFY(!saved.args.at(0).toObject().contains(QStringLiteral("password")));
    QCOMPARE(saved.args.at(1).toString(), QString());

    // And the field does not keep it once it has been handed over.
    QVERIFY(waitUntil([password] { return password->text().isEmpty(); }));
    QCOMPARE(changed.size(), 1);
}

void TestConnectionsDialog::aStoredProfileFillsTheFormAndDefaultsWhatIsMissing()
{
    ConnectionsDialog dlg({sparseProfile()}, {});
    buttons(dlg, QStringLiteral("Edit")).at(0)->click();

    QCOMPARE(edit(dlg, QStringLiteral("Name"))->text(), QStringLiteral("Scratch"));
    QCOMPARE(edit(dlg, QStringLiteral("Database"))->text(), QStringLiteral("scratch"));

    // Absent from the stored JSON, so what shows is the default rather than a
    // blank the next Save would write back over a working connection.
    QCOMPARE(spin(dlg, QStringLiteral("Port"))->value(), DefaultPort);
    QCOMPARE(spin(dlg, QStringLiteral("SSH Port"))->value(), DefaultSshPort);
    QCOMPARE(combo(dlg, QStringLiteral("TLS"))->currentText(), QStringLiteral("preferred"));
    QCOMPARE(combo(dlg, QStringLiteral("Method"))->currentData().toString(), QStringLiteral("tcp"));

    // The 127.0.0.1 / root pair belongs to a brand-new profile only; a stored
    // one keeps its own emptiness and lets the backend fill it in.
    QVERIFY(edit(dlg, QStringLiteral("Host"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("User"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("Color"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("SSH Host"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("SSH User"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("SSH Key File"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("Resource"))->text().isEmpty());

    // A password is never round-tripped through the UI, only asked after.
    QVERIFY(edit(dlg, QStringLiteral("Password"))->text().isEmpty());
    QVERIFY(waitUntil([this] { return callsTo(m_backend, HasPasswordPath).size() == 1; }));
    QCOMPARE(callsTo(m_backend, HasPasswordPath).at(0).args, QJsonArray({QStringLiteral("c2")}));
}

void TestConnectionsDialog::aLegacyTeleportFlagStillPicksTheTeleportMethod()
{
    // Written before Method existed: the bare flag is all these profiles have.
    const QJsonObject legacy{
        {"id", "c3"}, {"name", "Prod"}, {"teleport", true}, {"teleportDb", "prod-mysql"}
    };
    ConnectionsDialog dlg({legacy}, {});
    buttons(dlg, QStringLiteral("Edit")).at(0)->click();

    QCOMPARE(
        combo(dlg, QStringLiteral("Method"))->currentData().toString(), QStringLiteral("teleport")
    );
    QCOMPARE(edit(dlg, QStringLiteral("Resource"))->text(), QStringLiteral("prod-mysql"));
    QVERIFY(rowShown(dlg, QStringLiteral("Resource")));
    QVERIFY(!rowShown(dlg, QStringLiteral("Host")));

    // Picking teleport asks tsh whether there is a session to ride. The count
    // is not pinned: the method change and the form fill each apply the
    // method, so the status goes out twice for one opened profile.
    QVERIFY(waitUntil([this] { return !callsTo(m_backend, TeleportStatusPath).isEmpty(); }));
    QCOMPARE(callsTo(m_backend, TeleportStatusPath).at(0).args, QJsonArray());
}

void TestConnectionsDialog::switchingProfilesLeavesNothingOfThePreviousOne()
{
    ConnectionsDialog dlg({sshProfile(), sparseProfile()}, {});
    buttons(dlg, QStringLiteral("Edit")).at(0)->click();

    // Edits that are abandoned, including a password. Migrating any of these
    // to the profile opened next would save them against the wrong server.
    edit(dlg, QStringLiteral("Name"))->setText(QStringLiteral("edited"));
    edit(dlg, QStringLiteral("Color"))->setText(QStringLiteral("#00ff00"));
    edit(dlg, QStringLiteral("Host"))->setText(QStringLiteral("edited.example.invalid"));
    edit(dlg, QStringLiteral("User"))->setText(QStringLiteral("edited"));
    edit(dlg, QStringLiteral("Database"))->setText(QStringLiteral("edited"));
    edit(dlg, QStringLiteral("SSH Host"))->setText(QStringLiteral("edited.bastion"));
    edit(dlg, QStringLiteral("SSH User"))->setText(QStringLiteral("edited"));
    edit(dlg, QStringLiteral("SSH Key File"))->setText(QStringLiteral("/edited/key"));
    edit(dlg, QStringLiteral("Resource"))->setText(QStringLiteral("edited-resource"));
    edit(dlg, QStringLiteral("Password"))->insert(QString::fromLatin1(Placeholder));
    spin(dlg, QStringLiteral("Port"))->setValue(4000);
    spin(dlg, QStringLiteral("SSH Port"))->setValue(4022);
    combo(dlg, QStringLiteral("TLS"))->setCurrentText(QStringLiteral("disabled"));

    button(dlg, QStringLiteral("Cancel"))->click();
    buttons(dlg, QStringLiteral("Edit")).at(1)->click();

    QVERIFY(edit(dlg, QStringLiteral("Password"))->text().isEmpty());
    QCOMPARE(edit(dlg, QStringLiteral("Name"))->text(), QStringLiteral("Scratch"));
    QCOMPARE(edit(dlg, QStringLiteral("Database"))->text(), QStringLiteral("scratch"));
    QVERIFY(edit(dlg, QStringLiteral("Color"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("Host"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("User"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("SSH Host"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("SSH User"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("SSH Key File"))->text().isEmpty());
    QVERIFY(edit(dlg, QStringLiteral("Resource"))->text().isEmpty());
    QCOMPARE(spin(dlg, QStringLiteral("Port"))->value(), DefaultPort);
    QCOMPARE(spin(dlg, QStringLiteral("SSH Port"))->value(), DefaultSshPort);
    QCOMPARE(combo(dlg, QStringLiteral("TLS"))->currentText(), QStringLiteral("preferred"));
    QCOMPARE(combo(dlg, QStringLiteral("Method"))->currentData().toString(), QStringLiteral("tcp"));

    m_backend.replyWithResult(QJsonObject{{"id", "c2"}});
    m_backend.clearRequests();
    button(dlg, QStringLiteral("Save"))->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, SavePath).size() == 1; }));
    QJsonObject want = savePayload();
    want.insert("id", "c2");
    want.insert("name", "Scratch");
    want.insert("database", "scratch");

    // Saved under its own id, with none of the abandoned values on board.
    QCOMPARE(callsTo(m_backend, SavePath).at(0).args, QJsonArray({want, QString()}));

    // And the password typed under the first profile does not get written
    // against the second one's keyring entry.
    api()->flush(2000);
    QTest::qWait(50);
    QVERIFY(callsTo(m_backend, SetPasswordPath).isEmpty());
}

void TestConnectionsDialog::anEmptyHostAndNameAreSavedWithoutComplaint()
{
    ConnectionsDialog dlg({}, {});
    button(dlg, QStringLiteral("New Connection (&N)"))->click();
    edit(dlg, QStringLiteral("Host"))->setText(QString());
    edit(dlg, QStringLiteral("User"))->setText(QStringLiteral("  padded  "));

    m_backend.replyWithResult(QJsonObject{{"id", "c9"}});
    m_backend.clearRequests();
    button(dlg, QStringLiteral("Save"))->click();

    // Nothing is refused here. An empty name and an empty host are both legal
    // on the wire: the backend derives the name and falls back to 127.0.0.1,
    // and it is the one that rejects an ssh profile with no SSH host. Every
    // text field is trimmed on the way out.
    QVERIFY(waitUntil([this] { return callsTo(m_backend, SavePath).size() == 1; }));
    QJsonObject want = savePayload();
    want.insert("user", "padded");
    QCOMPARE(callsTo(m_backend, SavePath).at(0).args.at(0).toObject(), want);
}

void TestConnectionsDialog::thePortFieldClampsAndRollsBackNonsense()
{
    ConnectionsDialog dlg({}, {});
    button(dlg, QStringLiteral("New Connection (&N)"))->click();
    QSpinBox *port = spin(dlg, QStringLiteral("Port"));
    QVERIFY(port);

    QCOMPARE(port->minimum(), 0);
    QCOMPARE(port->maximum(), 65535);
    port->setValue(70000);
    QCOMPARE(port->value(), 65535);
    port->setValue(-1);
    QCOMPARE(port->value(), 0);

    // Letters cannot reach the profile: the field rolls back to the last good
    // value rather than reading as a zero, which the backend would turn into
    // 3306 against a server that is not on 3306.
    port->setValue(3307);
    QLineEdit *typed = port->findChild<QLineEdit *>();
    QVERIFY(typed);
    typed->setText(QStringLiteral("abc"));
    port->interpretText();
    QCOMPARE(port->value(), 3307);

    m_backend.replyWithResult(QJsonObject{{"id", "c9"}});
    m_backend.clearRequests();
    button(dlg, QStringLiteral("Save"))->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, SavePath).size() == 1; }));
    QJsonObject want = savePayload();
    want.insert("host", "127.0.0.1");
    want.insert("user", "root");
    want.insert("port", 3307);
    QCOMPARE(callsTo(m_backend, SavePath).at(0).args.at(0).toObject(), want);
}

void TestConnectionsDialog::theSshMethodSendsItsFieldsAndChecksTheAgent()
{
    ConnectionsDialog dlg({}, {});
    button(dlg, QStringLiteral("New Connection (&N)"))->click();
    m_backend.clearRequests();

    QComboBox *method = combo(dlg, QStringLiteral("Method"));
    QVERIFY(method);
    method->setCurrentIndex(method->findData(QStringLiteral("ssh")));

    QVERIFY(rowShown(dlg, QStringLiteral("SSH Host")));
    QVERIFY(rowShown(dlg, QStringLiteral("SSH Key File")));
    QVERIFY(!rowShown(dlg, QStringLiteral("Resource")));
    // The endpoint stays up: over SSH it describes MySQL as the SSH host sees
    // it, which is a different address, not an absent one.
    QVERIFY(rowShown(dlg, QStringLiteral("Host")));

    QVERIFY(waitUntil([this] { return callsTo(m_backend, AgentStatusPath).size() == 1; }));
    QCOMPARE(callsTo(m_backend, AgentStatusPath).at(0).args, QJsonArray());

    edit(dlg, QStringLiteral("SSH Host"))->setText(QStringLiteral("bastion.example.invalid"));
    edit(dlg, QStringLiteral("SSH User"))->setText(QStringLiteral("ops"));
    edit(dlg, QStringLiteral("SSH Key File"))->setText(QStringLiteral("/home/ops/.ssh/id_ed25519"));
    spin(dlg, QStringLiteral("SSH Port"))->setValue(2222);
    combo(dlg, QStringLiteral("TLS"))->setCurrentText(QStringLiteral("required"));

    m_backend.replyWithResult(QJsonObject{{"id", "c9"}});
    m_backend.clearRequests();
    button(dlg, QStringLiteral("Save"))->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, SavePath).size() == 1; }));
    QJsonObject want = savePayload();
    want.insert("method", "ssh");
    want.insert("host", "127.0.0.1");
    want.insert("user", "root");
    want.insert("sshHost", "bastion.example.invalid");
    want.insert("sshPort", 2222);
    want.insert("sshUser", "ops");
    want.insert("sshKeyFile", "/home/ops/.ssh/id_ed25519");
    want.insert("tlsMode", "required");
    QCOMPARE(callsTo(m_backend, SavePath).at(0).args.at(0).toObject(), want);
}

void TestConnectionsDialog::theSshFieldsStayInThePayloadAfterSwitchingToTcp()
{
    ConnectionsDialog dlg({}, {});
    button(dlg, QStringLiteral("New Connection (&N)"))->click();

    QComboBox *method = combo(dlg, QStringLiteral("Method"));
    method->setCurrentIndex(method->findData(QStringLiteral("ssh")));
    edit(dlg, QStringLiteral("SSH Host"))->setText(QStringLiteral("bastion.example.invalid"));
    edit(dlg, QStringLiteral("SSH User"))->setText(QStringLiteral("ops"));
    spin(dlg, QStringLiteral("SSH Port"))->setValue(2222);
    edit(dlg, QStringLiteral("Resource"))->setText(QStringLiteral("prod-mysql"));
    method->setCurrentIndex(method->findData(QStringLiteral("tcp")));
    QVERIFY(!rowShown(dlg, QStringLiteral("SSH Host")));

    m_backend.replyWithResult(QJsonObject{{"id", "c9"}});
    m_backend.clearRequests();
    button(dlg, QStringLiteral("Save"))->click();

    // There is no toggle that strips fields from the payload: every method's
    // fields go out every time, carrying whatever was last typed into them.
    // Only "method" and "teleport" say which of them the backend is to use, so
    // hidden rows persist as inert settings rather than being cleared.
    QVERIFY(waitUntil([this] { return callsTo(m_backend, SavePath).size() == 1; }));
    QJsonObject want = savePayload();
    want.insert("host", "127.0.0.1");
    want.insert("user", "root");
    want.insert("sshHost", "bastion.example.invalid");
    want.insert("sshPort", 2222);
    want.insert("sshUser", "ops");
    want.insert("teleportDb", "prod-mysql");
    QCOMPARE(callsTo(m_backend, SavePath).at(0).args.at(0).toObject(), want);
}

void TestConnectionsDialog::theTeleportMethodSetsTheFlagAndHidesTheEndpoint()
{
    ConnectionsDialog dlg({}, {});
    button(dlg, QStringLiteral("New Connection (&N)"))->click();

    QComboBox *method = combo(dlg, QStringLiteral("Method"));
    method->setCurrentIndex(method->findData(QStringLiteral("teleport")));

    QVERIFY(rowShown(dlg, QStringLiteral("Resource")));
    QVERIFY(!rowShown(dlg, QStringLiteral("Host")));
    QVERIFY(!rowShown(dlg, QStringLiteral("SSH Host")));
    // Teleport hands out a database-scoped certificate, so the schema stops
    // being optional.
    QCOMPARE(
        edit(dlg, QStringLiteral("Database"))->placeholderText(),
        QStringLiteral("required by most teleport db configs")
    );

    edit(dlg, QStringLiteral("Resource"))->setText(QStringLiteral("prod-mysql"));
    m_backend.replyWithResult(QJsonObject{{"id", "c9"}});
    m_backend.clearRequests();
    button(dlg, QStringLiteral("Save"))->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, SavePath).size() == 1; }));
    QJsonObject want = savePayload();
    want.insert("method", "teleport");
    want.insert("teleport", true);
    want.insert("teleportDb", "prod-mysql");
    // The endpoint rows are hidden, not cleared: the defaults they were opened
    // on still travel.
    want.insert("host", "127.0.0.1");
    want.insert("user", "root");

    // Both spellings go out, so a build reading either one agrees on teleport.
    QCOMPARE(callsTo(m_backend, SavePath).at(0).args.at(0).toObject(), want);
}

void TestConnectionsDialog::theRemoveButtonFollowsWhetherAPasswordIsStored()
{
    m_backend.replyWithResult(QJsonValue(true));
    ConnectionsDialog stored({sparseProfile()}, {});
    buttons(stored, QStringLiteral("Edit")).at(0)->click();

    QPushButton *remove = button(stored, QStringLiteral("Remove"));
    QVERIFY(remove);
    // Hidden until the answer comes back, so a profile with no secret never
    // shows a control that would delete somebody else's.
    QVERIFY(remove->isHidden());
    QVERIFY(waitUntil([remove] { return !remove->isHidden(); }));
    QCOMPARE(
        edit(stored, QStringLiteral("Password"))->placeholderText(),
        QStringLiteral("stored — type to replace")
    );

    m_backend.replyWithResult(QJsonValue(false));
    ConnectionsDialog bare({sparseProfile()}, {});
    buttons(bare, QStringLiteral("Edit")).at(0)->click();

    QLineEdit *password = edit(bare, QStringLiteral("Password"));
    // The placeholder starts empty rather than "required" on a fresh dialog, so
    // wait for the answer itself to land instead of for any change.
    QVERIFY(waitUntil([password]
                      { return password->placeholderText() == QStringLiteral("none stored"); }));
    QCOMPARE(password->placeholderText(), QStringLiteral("none stored"));
    QVERIFY(button(bare, QStringLiteral("Remove"))->isHidden());
}

void TestConnectionsDialog::reorderingSendsEveryIdAsOneArgument()
{
    ConnectionsDialog dlg({sshProfile(), sparseProfile()}, {});
    const QList<QPushButton *> down = arrows(dlg, QStringLiteral("Move Down"));
    QCOMPARE(down.size(), 2);

    // The arrow that has nowhere to go is hidden rather than disabled, so the
    // button columns of the two rows still line up.
    QVERIFY(!down.at(0)->isHidden());
    QVERIFY(down.at(1)->isHidden());

    m_backend.clearRequests();
    down.at(0)->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, ReorderPath).size() == 1; }));
    // One argument holding the whole list. Flattened, the RPC would read every
    // id as an argument of its own and the order would be lost.
    QCOMPARE(
        callsTo(m_backend, ReorderPath).at(0).args,
        QJsonArray({QJsonArray({QStringLiteral("c2"), QStringLiteral("c1")})})
    );
}

QTEST_MAIN(TestConnectionsDialog)

#include "tst_connectionsdialog.moc"
