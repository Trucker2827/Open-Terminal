#include "services/dinero/DineroRpcClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace openmarketterminal::services::dinero {

QString DineroRpcClient::rpcUrl() {
    return QStringLiteral("https://rpc.realmoneyforfreepeople.org");
}

QString DineroRpcClient::explorerUrl() {
    return QStringLiteral("https://explorer.realmoneyforfreepeople.org");
}

QString DineroRpcClient::siteUrl() {
    return QStringLiteral("https://dinerolabs.org");
}

DineroRpcClient::DineroRpcClient(QObject* parent) : QObject(parent) {
    net_ = new QNetworkAccessManager(this);
    connect(net_, &QNetworkAccessManager::finished, this, &DineroRpcClient::handle_reply);
}

void DineroRpcClient::refresh() {
    QNetworkRequest req((QUrl(rpcUrl())));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    const QByteArray body =
        R"({"jsonrpc":"2.0","id":1,"method":"economics.getinfo","params":[]})";
    net_->post(req, body);
}

void DineroRpcClient::handle_reply(QNetworkReply* reply) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit failed(tr("Dinero network unavailable"));
        return;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        emit failed(tr("Bad response from Dinero RPC"));
        return;
    }
    const QJsonObject r = doc.object().value("result").toObject();
    if (r.isEmpty()) {
        emit failed(tr("Dinero network unavailable"));
        return;
    }

    DineroStats s;
    s.valid = true;
    s.height = static_cast<qlonglong>(r.value("current_height").toDouble());
    s.supply_din = r.value("current_supply_din").toString().toDouble();
    // NOTE: economics.getinfo reports block_time_seconds=180, but the real on-chain
    // interval is 120s (verified empirically over 500 blocks, 2026-06-11). Block time
    // is a fixed protocol constant, so we use the correct 120s rather than the buggy
    // RPC field. (Fix the chain's economics RPC and this can read the field again.)
    s.block_time_sec = 120;
    s.difficulty = r.value("next_block_difficulty").toDouble();
    s.reward_din = r.value("next_block_reward_din").toString().toDouble();
    s.monetary_policy = r.value("monetary_policy").toString();
    emit statsReady(s);
}

} // namespace openmarketterminal::services::dinero
