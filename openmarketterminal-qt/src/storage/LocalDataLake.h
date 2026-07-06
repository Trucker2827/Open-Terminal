#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace openmarketterminal {
struct EdgePredictionMarketSnapshot;
struct TradeAuditRow;
}

namespace openmarketterminal::storage {

class LocalDataLake {
  public:
    static LocalDataLake& instance();

    QString root_dir() const;
    QString dataset_dir(const QString& dataset) const;
    QString dataset_file(const QString& dataset, const QString& partition = {}) const;
    QString manifest_path() const;

    bool ensure(QString* error = nullptr) const;
    QJsonObject status() const;
    QJsonObject manifest() const;

    bool append_jsonl(const QString& dataset, const QJsonObject& row, QString* error = nullptr);
    int append_jsonl(const QString& dataset, const QJsonArray& rows, QString* error = nullptr);
    bool replace_jsonl(const QString& dataset,
                       const QJsonArray& rows,
                       const QString& partition = {},
                       QString* error = nullptr);
    bool append_market_snapshot(const EdgePredictionMarketSnapshot& snapshot,
                                const QJsonObject& extra = {},
                                QString* error = nullptr);
    bool append_broker_event(const TradeAuditRow& row, QString* error = nullptr);

    int mirror_edge_prediction_store(const QString& symbol, QString* error = nullptr);

  private:
    LocalDataLake() = default;
    LocalDataLake(const LocalDataLake&) = delete;
    LocalDataLake& operator=(const LocalDataLake&) = delete;
};

} // namespace openmarketterminal::storage
