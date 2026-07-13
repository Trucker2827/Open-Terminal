#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace openmarketterminal::services::news {

struct BtcNewsStory {
    QString id;
    QString headline;
    QString paragraph;
    QString source;
    QString link;
    QString published;
    QString direction;
    QString horizon;
    QStringList catalysts;
    double score = 0.0;
    double relevance = 0.0;
    double source_weight = 0.0;
    qint64 published_ts = 0;
};

struct BtcNewsPulseResult {
    QString verdict;
    QString explanation;
    QString caveat;
    QVector<BtcNewsStory> stories;
    QStringList catalysts;
    int scanned = 0;
    int relevant = 0;
    int duplicates_removed = 0;
    int bullish = 0;
    int bearish = 0;
    int neutral = 0;
    int distinct_sources = 0;
    double score = 0.0;          // -100..100 narrative pressure
    double confidence = 0.0;     // evidence quality/agreement, not win probability
    double source_agreement = 0.0;
    qint64 as_of_ms = 0;
};

class BtcNewsPulse {
  public:
    static BtcNewsPulseResult analyze(const QJsonArray& articles, qint64 now_ms,
                                      int max_stories = 20);
    static QJsonObject to_json(const BtcNewsPulseResult& pulse);
};

} // namespace openmarketterminal::services::news
