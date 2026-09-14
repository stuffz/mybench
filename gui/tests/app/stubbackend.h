#pragma once
// A loopback stand-in for the Go backend's /rpc endpoint, so the client half
// of the RPC can be tested without a server process or a database.
//
// It speaks only as much HTTP as QNetworkAccessManager needs: read the request
// line, the headers and a Content-Length body, then write one canned reply and
// close. Every reply carries Connection: close, so each call is its own socket
// and no keep-alive state has to be tracked.
//
// The wire shape it imitates is internal/rpc's:
//     POST /rpc/{service}/{method}   body: positional JSON array
//     X-Mybench-Token: <token>
//     200 {"result": ..., "error": ...}    both keys omitempty on the Go side
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

class StubBackend
{
public:
    struct Request
    {
        QString verb;
        QString path;
        QString token;
        QJsonArray args;
    };

    StubBackend()
    {
        m_server.listen(QHostAddress::LocalHost);
        QObject::connect(
            &m_server, &QTcpServer::newConnection, &m_server, [this]() { accept(); }
        );
    }

    QUrl base() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()));
    }

    bool listening() const { return m_server.isListening(); }

    // The reply every subsequent call receives. status and body are sent as
    // given, so a test can post a malformed body or a 500 on purpose.
    void replyWith(const QByteArray &body, int status = 200)
    {
        m_body = body;
        m_status = status;
    }

    void replyWithResult(const QJsonValue &result)
    {
        QJsonObject o;
        o.insert(QStringLiteral("result"), result);
        replyWith(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }

    void replyWithError(const QString &message)
    {
        QJsonObject o;
        o.insert(QStringLiteral("error"), message);
        replyWith(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }

    const QList<Request> &requests() const { return m_requests; }
    void clearRequests() { m_requests.clear(); }

private:
    void accept()
    {
        QTcpSocket *sock = m_server.nextPendingConnection();
        if (!sock)
        {
            return;
        }
        auto *buffer = new QByteArray;
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        QObject::connect(
            sock, &QTcpSocket::readyRead, sock,
            [this, sock, buffer]()
            {
                buffer->append(sock->readAll());
                if (!complete(*buffer))
                {
                    return;
                }
                record(*buffer);
                sock->write(response());
                sock->disconnectFromHost();
                delete buffer;
            }
        );
    }

    // Headers ended and the whole declared body has arrived.
    static bool complete(const QByteArray &raw)
    {
        const qsizetype headEnd = raw.indexOf("\r\n\r\n");
        if (headEnd < 0)
        {
            return false;
        }
        return raw.size() - (headEnd + 4) >= contentLength(raw.left(headEnd));
    }

    static int contentLength(const QByteArray &head)
    {
        for (const QByteArray &line : head.split('\n'))
        {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.toLower().startsWith("content-length:"))
            {
                return trimmed.mid(trimmed.indexOf(':') + 1).trimmed().toInt();
            }
        }
        return 0;
    }

    void record(const QByteArray &raw)
    {
        const qsizetype headEnd = raw.indexOf("\r\n\r\n");
        const QByteArray head = raw.left(headEnd);
        const QByteArray body = raw.mid(headEnd + 4);
        const QList<QByteArray> lines = head.split('\n');

        Request req;
        const QList<QByteArray> start = lines.value(0).trimmed().split(' ');
        req.verb = QString::fromUtf8(start.value(0));
        req.path = QString::fromUtf8(start.value(1));
        for (const QByteArray &line : lines)
        {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.toLower().startsWith("x-mybench-token:"))
            {
                req.token = QString::fromUtf8(trimmed.mid(trimmed.indexOf(':') + 1).trimmed());
            }
        }
        req.args = QJsonDocument::fromJson(body).array();
        m_requests.append(req);
    }

    QByteArray response() const
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(m_status) + " X\r\n";
        out += "Content-Type: application/json\r\n";
        out += "Content-Length: " + QByteArray::number(m_body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += m_body;
        return out;
    }

    QTcpServer m_server;
    QList<Request> m_requests;
    QByteArray m_body{"{}"};
    int m_status = 200;
};
