#pragma once
// Client for internal/rpc: POST /rpc/{service}/{method} with a positional
// JSON array. One seam for the whole app, same role lib/api.ts had.
#include <QJsonArray>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;

class Api : public QObject
{
    Q_OBJECT
public:
    // result is the decoded "result" field; error is empty on success.
    using Cb = std::function<void(const QJsonValue &result, const QString &error)>;

    static Api *instance();

    void setEndpoint(const QUrl &base, const QString &token);

    bool ready() const { return !m_base.isEmpty(); }

    // context is the object the callback touches — when it is destroyed before
    // the reply arrives, the callback is dropped. Closing a tab or a dialog
    // mid-request is normal, so this is required rather than optional.
    void call(
        const QString &service, const QString &method, const QJsonArray &args, QObject *context,
        const Cb &cb
    );
    // Fire-and-forget: for the calls whose failure the UI does not surface
    // (CloseResult, CloseTab, SetHistoryLimit).
    void post(const QString &service, const QString &method, const QJsonArray &args);

    // Pump the event loop until every in-flight request has finished or maxMs
    // elapsed. For shutdown paths (the final workspace save) where returning
    // to the event loop would let the application die with the POST unsent.
    void flush(int maxMs);

private:
    explicit Api(QObject *parent = nullptr);
    QNetworkAccessManager *m_net;
    QUrl m_base;
    QString m_token;
    int m_inFlight = 0;
};

// Shorthands — these read like the TS `QueryService.Run(...)` call sites.
inline Api *api()
{
    return Api::instance();
}
