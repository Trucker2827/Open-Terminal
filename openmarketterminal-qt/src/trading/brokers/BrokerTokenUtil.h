#pragma once
// BrokerTokenUtil — shared helpers for broker access-token lifetime tracking.
//
// Some brokers expire their access token on a fixed daily wall-clock boundary or
// on a rolling N-hour window. Brokers record the computed expiry epoch in their
// TokenExchangeResponse::additional_data JSON under "token_expires_at" so that
// AccountManager can show an accurate connection state on startup *before* the
// live validation sweep runs. The stored value is only a hint — the
// authoritative check is IBroker::validate_session() — so it does not need to be
// exact, only conservative enough to avoid showing a long-dead token as live.

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace openmarketterminal::trading {

// Merge "token_expires_at" into an additional_data JSON string, preserving any
// existing keys. `base_json` may be empty.
inline QString with_token_expiry(const QString& base_json, qint64 expires_at_epoch) {
    QJsonObject obj = QJsonDocument::fromJson(base_json.toUtf8()).object();
    obj.insert(QStringLiteral("token_expires_at"), static_cast<double>(expires_at_epoch));
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// Read "token_expires_at" (epoch seconds) from an additional_data JSON string.
// Returns 0 when absent/unparseable.
inline qint64 token_expires_at_of(const QString& additional_json) {
    if (additional_json.isEmpty())
        return 0;
    const auto obj = QJsonDocument::fromJson(additional_json.toUtf8()).object();
    return static_cast<qint64>(obj.value(QStringLiteral("token_expires_at")).toDouble(0));
}

} // namespace openmarketterminal::trading
