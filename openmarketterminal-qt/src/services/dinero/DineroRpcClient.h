#pragma once
// DineroRpcClient — shared read-only client for the public Dinero RPC.
//
// Fetches public chain stats from rpc.realmoneyforfreepeople.org over HTTP and
// emits a parsed snapshot. Used by both the Dinero dashboard widget and the
// Dinero screen so the fetch/parse lives in one place. Read-only, no auth, no
// user data — purely informational ("get familiar with the project").

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace openmarketterminal::services::dinero {

struct DineroStats {
    bool valid = false;
    qlonglong height = 0;
    double supply_din = 0.0;
    int block_time_sec = 0;
    double difficulty = 0.0;
    double reward_din = 0.0;
    QString monetary_policy;
};

class DineroRpcClient : public QObject {
    Q_OBJECT
  public:
    explicit DineroRpcClient(QObject* parent = nullptr);

    /// Kick off an async fetch of `economics.getinfo`. Emits statsReady or failed.
    void refresh();

    static QString rpcUrl();
    static QString explorerUrl();
    static QString siteUrl(); // dinerolabs.org — landing/download site

  signals:
    void statsReady(const openmarketterminal::services::dinero::DineroStats& stats);
    void failed(const QString& message);

  private:
    void handle_reply(QNetworkReply* reply);
    QNetworkAccessManager* net_ = nullptr;
};

} // namespace openmarketterminal::services::dinero
