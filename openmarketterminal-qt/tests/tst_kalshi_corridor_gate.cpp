#include "services/prediction/kalshi/KalshiCorridorGate.h"

#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

using openmarketterminal::services::prediction::kalshi_ns::KalshiCorridorGate;

namespace {
constexpr qint64 kNow = 1'786'000'000'000LL;

QJsonObject params() {
    return QJsonObject{{"min_scans", 300},
                       {"min_distinct_events", 3},
                       {"min_opportunity_scans", 10},
                       {"min_opportunity_events", 3},
                       {"min_best_net_edge_usd", 0.01},
                       {"max_unavailable_rate", 0.10}};
}

QJsonObject scan(int index, bool opportunity = false, bool unavailable = false) {
    QJsonObject result{{"state", opportunity ? "opportunity" : "not_profitable"},
                       {"net_edge_per_bundle", opportunity ? "0.025" : "0.000"}};
    QJsonObject evaluation{{"state", unavailable ? "unavailable"
                                                   : opportunity ? "opportunity"
                                                                 : "not_profitable"},
                           {"reason", unavailable ? "book unavailable" : "measured"},
                           {"pairs_evaluated", unavailable ? 0 : 3},
                           {"opportunities", opportunity ? 1 : 0},
                           {"pairs", unavailable
                                         ? QJsonArray{}
                                         : QJsonArray{QJsonObject{{"evaluation", result}}}}};
    return QJsonObject{{"event", KalshiCorridorGate::kScanEvent},
                       {"family", KalshiCorridorGate::kFamily},
                       {"received_at", "2026-08-13T12:00:01.000Z"},
                       {"certificate", QJsonObject{{"event_ticker",
                                                     QStringLiteral("KXBTCD-E%1")
                                                         .arg(index % 3)}}},
                       {"certificate_sha256", QStringLiteral("reviewed")},
                       {"evaluation", evaluation}};
}
} // namespace

class TstKalshiCorridorGate : public QObject {
    Q_OBJECT
  private slots:
    void sealed_record_passes_only_its_own_paper_gate() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;
        const QJsonObject sealed = KalshiCorridorGate::preregister(
            dir.filePath(KalshiCorridorGate::kParamsFile), params(), kNow, &error);
        QVERIFY2(!sealed.isEmpty(), qPrintable(error));
        QVERIFY(KalshiCorridorGate::seal_valid(sealed));

        QJsonArray rows;
        for (int i = 0; i < 300; ++i) rows.append(scan(i, i < 10));
        const QJsonObject verdict = KalshiCorridorGate::evaluate(sealed, rows, kNow + 1);
        QCOMPARE(verdict.value("verdict").toString(), QStringLiteral("PASS"));
        QVERIFY(verdict.value("paper_bids_authorized").toBool());
        QVERIFY(!verdict.value("live_orders_authorized").toBool());
        QString reason;
        QVERIFY2(KalshiCorridorGate::permits_paper_bid(verdict, &reason), qPrintable(reason));

        // A directional gate can say PASS and still has zero corridor authority.
        const QJsonObject directional{{"verdict", "PASS"},
                                      {"evaluated", true},
                                      {"strategy_family", "KXBTCD"},
                                      {"authority", "live_if_armed"}};
        QVERIFY(!KalshiCorridorGate::permits_paper_bid(directional, &reason));
        QVERIFY(reason.contains(QStringLiteral("not the BTC corridor")));
    }

    void insufficient_or_unavailable_record_fails() {
        QTemporaryDir dir;
        QString error;
        const QJsonObject sealed = KalshiCorridorGate::preregister(
            dir.filePath(KalshiCorridorGate::kParamsFile), params(), kNow, &error);
        QJsonArray rows;
        for (int i = 0; i < 300; ++i) rows.append(scan(i, i < 10, i < 31));
        const QJsonObject verdict = KalshiCorridorGate::evaluate(sealed, rows, kNow + 1);
        QCOMPARE(verdict.value("verdict").toString(), QStringLiteral("FAIL"));
        QVERIFY(!KalshiCorridorGate::permits_paper_bid(verdict));
    }

    void tamper_and_missing_params_fail_closed() {
        QTemporaryDir dir;
        QString error;
        QJsonObject sealed = KalshiCorridorGate::preregister(
            dir.filePath(KalshiCorridorGate::kParamsFile), params(), kNow, &error);
        sealed["gate_id"] = QStringLiteral("changed");
        QCOMPARE(KalshiCorridorGate::evaluate(sealed, {}, kNow + 1).value("verdict").toString(),
                 QStringLiteral("TAMPERED"));
        QCOMPARE(KalshiCorridorGate::evaluate(QJsonValue(QJsonValue::Undefined), {}, kNow + 1)
                     .value("verdict").toString(),
                 QStringLiteral("NOT_PREREGISTERED"));
    }

    void evidence_seen_before_preregistration_never_counts() {
        QTemporaryDir dir;
        QString error;
        const QJsonObject sealed = KalshiCorridorGate::preregister(
            dir.filePath(KalshiCorridorGate::kParamsFile), params(), kNow, &error);
        QJsonArray rows;
        for (int i = 0; i < 300; ++i) {
            QJsonObject old = scan(i, true);
            old["received_at"] = QStringLiteral("2026-01-01T00:00:00.000Z");
            rows.append(old);
        }
        const QJsonObject out = KalshiCorridorGate::evaluate(sealed, rows, kNow + 1);
        QCOMPARE(out.value("verdict").toString(), QStringLiteral("FAIL"));
        QCOMPARE(out.value("evidence").toObject().value("scans").toInt(), 0);
        QCOMPARE(out.value("evidence").toObject().value("rows_before_seal").toInt(), 300);
    }
};

QTEST_APPLESS_MAIN(TstKalshiCorridorGate)
#include "tst_kalshi_corridor_gate.moc"
