#include "app/api.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

Api::Api(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this))
{
    // Everything is localhost; a stale cache or proxy would only cause grief.
    m_net->setTransferTimeout(0);
}

Api *Api::instance()
{
    static Api *inst = new Api;
    return inst;
}

void Api::setEndpoint(const QUrl &base, const QString &token)
{
    m_base = base;
    m_token = token;
}

void Api::call(
    const QString &service, const QString &method, const QJsonArray &args, QObject *context,
    const Cb &cb
)
{
    if (m_base.isEmpty())
    {
        if (cb)
        {
            cb({}, QStringLiteral("backend not ready"));
        }
        return;
    }
    const QPointer<QObject> ctx(context);
    QNetworkRequest req(m_base.resolved(QUrl("/rpc/" + service + "/" + method)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!m_token.isEmpty())
    {
        req.setRawHeader("X-Mybench-Token", m_token.toUtf8());
    }

    QNetworkReply *r = m_net->post(req, QJsonDocument(args).toJson(QJsonDocument::Compact));
    ++m_inFlight;
    connect(
        r, &QNetworkReply::finished, this,
        [this, r, cb, ctx, context, service, method]()
        {
            --m_inFlight;
            r->deleteLater();
            if (!cb)
            {
                return;
            }
            if (context && !ctx)
            {
                return; // caller is gone; its callback would touch freed memory
            }
            const QByteArray body = r->readAll();
            if (r->error() != QNetworkReply::NoError && body.isEmpty())
            {
                cb({}, service + "." + method + ": " + r->errorString());
                return;
            }
            QJsonParseError pe{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject())
            {
                cb({}, service + "." + method + ": " + QString::fromUtf8(body.left(200)));
                return;
            }
            const QJsonObject o = doc.object();
            QString err = o.value("error").toString();
            // A transport-level failure whose body parses as JSON without an
            // "error" key must not read as success.
            if (err.isEmpty() && r->error() != QNetworkReply::NoError)
            {
                err = service + "." + method + ": " + r->errorString();
            }
            cb(o.value("result"), err);
        }
    );
}

void Api::flush(int maxMs)
{
    QDeadlineTimer deadline(maxMs);
    // User input is excluded: this runs on the way out, and a stray click
    // delivered here would act on a window that is already closing.
    while (m_inFlight > 0 && !deadline.hasExpired())
    {
        QCoreApplication::processEvents(
            QEventLoop::ExcludeUserInputEvents | QEventLoop::WaitForMoreEvents, 50
        );
    }
}

void Api::post(const QString &service, const QString &method, const QJsonArray &args)
{
    call(service, method, args, nullptr, nullptr);
}
