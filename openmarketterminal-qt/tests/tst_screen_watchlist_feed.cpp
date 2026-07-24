// tests/tst_screen_watchlist_feed.cpp
//
// Issue #102 — the screen → watchlist feed mapping must carry the model's IC
// evidence. The watchlist description records model id, as-of date and the
// measured IC + window; when IC is unavailable it records the explicit
// "IC unmeasured — treat ranks as opinion" disclaimer, never nothing. Each
// stock's notes record its score. These tests fail if that evidence is dropped.
#include "services/ai_quant_lab/ScreenWatchlistFeed.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest/QtTest>

using namespace openmarketterminal::services::quant;

namespace {

QJsonObject sample_screen_payload() {
    QJsonObject payload;
    payload["success"] = true;
    payload["model_id"] = "lightgbm_1a2b3c";
    payload["as_of"] = "2026-07-17";
    payload["universe_size"] = 87;
    QJsonObject row;
    row["symbol"] = "AAPL";
    row["score"] = 0.042317;
    payload["top"] = QJsonArray{row};
    payload["caveat"] = "Candidate list only — verify predictive power first.";
    return payload;
}

QJsonObject sample_ic_payload() {
    QJsonObject results;
    results["IC_mean"] = 0.0123;
    results["Rank_IC_mean"] = 0.0110;
    results["days"] = 42;
    QJsonObject period;
    period["start"] = "2026-04-18";
    period["end"] = "2026-07-17";
    QJsonObject payload;
    payload["success"] = true;
    payload["model_id"] = "lightgbm_1a2b3c";
    payload["results"] = results;
    payload["period"] = period;
    return payload;
}

} // namespace

class TstScreenWatchlistFeed : public QObject {
    Q_OBJECT
  private slots:
    void name_is_derived_from_model_id();
    void description_records_ic_evidence();
    void description_without_ic_carries_disclaimer();
    void failed_ic_payload_counts_as_unmeasured();
    void stock_note_records_score();
};

void TstScreenWatchlistFeed::name_is_derived_from_model_id() {
    const QString name = screen_watchlist_name("lightgbm_1a2b3c");
    QVERIFY(name.contains("lightgbm_1a2b3c"));
}

void TstScreenWatchlistFeed::description_records_ic_evidence() {
    const QString desc = screen_watchlist_description(sample_screen_payload(), sample_ic_payload());
    // Model id + as-of date.
    QVERIFY(desc.contains("lightgbm_1a2b3c"));
    QVERIFY(desc.contains("2026-07-17"));
    // The IC value AND its window — the evidence the issue forbids dropping.
    QVERIFY(desc.contains("0.0123"));
    QVERIFY(desc.contains("2026-04-18"));
    QVERIFY(desc.contains("42"));
    // A measured IC must not be presented as unmeasured.
    QVERIFY(!desc.contains(screen_ic_unmeasured_disclaimer()));
}

void TstScreenWatchlistFeed::description_without_ic_carries_disclaimer() {
    const QString desc = screen_watchlist_description(sample_screen_payload(), QJsonObject());
    QVERIFY(desc.contains("lightgbm_1a2b3c"));
    QVERIFY(desc.contains("2026-07-17"));
    QVERIFY(desc.contains(screen_ic_unmeasured_disclaimer()));
}

void TstScreenWatchlistFeed::failed_ic_payload_counts_as_unmeasured() {
    QJsonObject failed_ic;
    failed_ic["success"] = false;
    failed_ic["error"] = "Too few instruments per day to compute IC";
    QVERIFY(screen_ic_evidence(failed_ic) == screen_ic_unmeasured_disclaimer());
    // success:true but no IC_mean is still unmeasured — no fabricated numbers.
    QJsonObject empty_results;
    empty_results["success"] = true;
    empty_results["results"] = QJsonObject();
    QVERIFY(screen_ic_evidence(empty_results) == screen_ic_unmeasured_disclaimer());
}

void TstScreenWatchlistFeed::stock_note_records_score() {
    const QJsonObject payload = sample_screen_payload();
    const QJsonObject row = payload.value("top").toArray().first().toObject();
    const QString note = screen_stock_note(row, payload);
    QVERIFY(note.contains("0.042317"));
    QVERIFY(note.contains("lightgbm_1a2b3c"));
    QVERIFY(note.contains("2026-07-17"));
}

QTEST_APPLESS_MAIN(TstScreenWatchlistFeed)
#include "tst_screen_watchlist_feed.moc"
