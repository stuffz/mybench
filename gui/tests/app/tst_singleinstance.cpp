#include "app/singleinstance.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLockFile>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

// Both sides of the handshake run in this one process, each SingleInstance
// playing a separate launch. The lock and socket names are the ones
// singleinstance.cpp picks inside the directory it is given.
namespace
{

constexpr auto TokenVar = "XDG_ACTIVATION_TOKEN";
constexpr auto Token = "kwin-token-1";
constexpr int SocketPathLimit = 108;

} // namespace

class TestSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void firstLaunchIsPrimary();
    void secondLaunchForwardsAndPrimaryIsAsked();
    void forwardCarriesTheActivationToken();
    void primaryAnswersEveryLaterLaunch();
    void launchAfterPrimaryExitsIsPrimary();
    void leftoverSocketFileDoesNotBlockPrimary();
    void heldLockWithNoListenerIsUnreachable();
    void listenFailureIsUnguarded();

private:
    QTemporaryDir m_dir;
};

void TestSingleInstance::init()
{
    qunsetenv(TokenVar);
    QVERIFY(m_dir.isValid());
    QFile::remove(m_dir.filePath("mybench.lock"));
    QFile::remove(m_dir.filePath("mybench.sock"));
}

void TestSingleInstance::firstLaunchIsPrimary()
{
    SingleInstance first(m_dir.path());
    QCOMPARE(first.claim(), SingleInstance::Role::Primary);
}

void TestSingleInstance::secondLaunchForwardsAndPrimaryIsAsked()
{
    SingleInstance first(m_dir.path());
    QCOMPARE(first.claim(), SingleInstance::Role::Primary);
    QSignalSpy asked(&first, &SingleInstance::activationRequested);

    SingleInstance second(m_dir.path());
    QCOMPARE(second.claim(), SingleInstance::Role::Forwarded);

    QTRY_COMPARE(asked.count(), 1);
}

void TestSingleInstance::forwardCarriesTheActivationToken()
{
    SingleInstance first(m_dir.path());
    QCOMPARE(first.claim(), SingleInstance::Role::Primary);

    // Qt's Wayland plugin reads this variable in requestActivate(), so it
    // must be in place when the signal fires, not merely afterwards.
    QByteArray seen;
    connect(
        &first, &SingleInstance::activationRequested, this, [&seen]() { seen = qgetenv(TokenVar); }
    );

    // The launcher gives the token to the new process, not the running one.
    // Clearing it once the second launch has sent it proves the value the
    // primary ends up with came over the socket.
    qputenv(TokenVar, Token);
    SingleInstance second(m_dir.path());
    QCOMPARE(second.claim(), SingleInstance::Role::Forwarded);
    qunsetenv(TokenVar);

    QTRY_COMPARE(seen, QByteArray(Token));
}

void TestSingleInstance::primaryAnswersEveryLaterLaunch()
{
    SingleInstance first(m_dir.path());
    QCOMPARE(first.claim(), SingleInstance::Role::Primary);
    QSignalSpy asked(&first, &SingleInstance::activationRequested);

    for (int launch = 1; launch <= 3; ++launch)
    {
        SingleInstance later(m_dir.path());
        QCOMPARE(later.claim(), SingleInstance::Role::Forwarded);
        QTRY_COMPARE(asked.count(), launch);
    }
}

void TestSingleInstance::launchAfterPrimaryExitsIsPrimary()
{
    {
        SingleInstance first(m_dir.path());
        QCOMPARE(first.claim(), SingleInstance::Role::Primary);
    }

    SingleInstance next(m_dir.path());
    QCOMPARE(next.claim(), SingleInstance::Role::Primary);

    SingleInstance after(m_dir.path());
    QCOMPARE(after.claim(), SingleInstance::Role::Forwarded);
}

void TestSingleInstance::leftoverSocketFileDoesNotBlockPrimary()
{
#ifdef Q_OS_WIN
    QSKIP("named pipes leave no file behind");
#endif
    // A crashed primary leaves its socket file behind. A plain bind() onto
    // that path fails with AddressInUse; UserAccessOption's rename replaces it.
    QFile leftover(m_dir.filePath("mybench.sock"));
    QVERIFY(leftover.open(QIODevice::WriteOnly));
    leftover.close();

    SingleInstance first(m_dir.path());
    QCOMPARE(first.claim(), SingleInstance::Role::Primary);

    SingleInstance second(m_dir.path());
    QCOMPARE(second.claim(), SingleInstance::Role::Forwarded);
}

void TestSingleInstance::heldLockWithNoListenerIsUnreachable()
{
    // A live process owns the lock but never listens, as a hung primary
    // would. The launch must give up rather than open a second window.
    QLockFile held(m_dir.filePath("mybench.lock"));
    QVERIFY(held.tryLock(0));

    SingleInstance late(m_dir.path());
    QElapsedTimer waited;
    waited.start();
    QCOMPARE(late.claim(), SingleInstance::Role::Unreachable);
    QVERIFY2(waited.elapsed() < 10000, "claim() must give up in bounded time");
}

void TestSingleInstance::listenFailureIsUnguarded()
{
#ifdef Q_OS_WIN
    QSKIP("pipe names are hashed to a fixed length");
#endif
    // sockaddr_un caps a socket path at 108 bytes on Linux, 104 on macOS; the
    // lock file has no such limit, so only listen() fails.
    const QString longDir = m_dir.filePath(QString(SocketPathLimit, 'x'));
    QVERIFY(QDir().mkpath(longDir));

    SingleInstance first(longDir);
    QCOMPARE(first.claim(), SingleInstance::Role::Unguarded);
    QVERIFY(!first.errorString().isEmpty());

    // A lock held without a listener would make every later launch wait out
    // the connect deadline and report a hung primary.
    SingleInstance second(longDir);
    QCOMPARE(second.claim(), SingleInstance::Role::Unguarded);
}

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "tst_singleinstance.moc"
