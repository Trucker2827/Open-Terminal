#include "services/prediction/kalshi/KalshiCorridorGate.h"

#include <QtTest>

#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>

using openmarketterminal::services::prediction::kalshi_ns::KalshiCorridorGate;

namespace {
constexpr qint64 kNow = 1'786'000'000'000LL;

QJsonObject params() {
    return QJsonObject{{"max_bundles_per_opportunity", 2},
                       {"max_cost_per_opportunity_usd", 2.0},
                       {"max_scan_age_ms", 60'000}};
}

QJsonObject scan(int index, bool opportunity = true, bool unavailable = false,
                 qint64 received_ms = kNow + 1) {
    const QJsonObject certificate{{"event_ticker", QStringLiteral("KXBTCD-E%1").arg(index % 3)},
                                  {"family", KalshiCorridorGate::kFamily}};
    QJsonObject sorted_certificate;
    QStringList certificate_keys = certificate.keys();
    certificate_keys.sort();
    for (const QString& key : certificate_keys)
        sorted_certificate.insert(key, certificate.value(key));
    const QString certificate_sha = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(sorted_certificate).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
    QJsonObject result{{"state", opportunity ? "opportunity" : "not_profitable"},
                       {"certificate_sha256", "pair-digest-is-distinct"},
                       {"quantity", "1"},
                       {"acquisition_cost", "0.80"},
                       {"fees", "0.05"},
                       {"execution_buffer", "0.02"},
                       {"net_edge_per_bundle", opportunity ? "0.15" : "0.000"},
                       {"legs", QJsonArray{
                           QJsonObject{{"ticker", "KXBTCD-E-T64000"}, {"side", "yes"},
                                       {"contracts", "1"}, {"cost", "0.42"},
                                       {"fee", "0.01"},
                                       {"fills", QJsonArray{QJsonObject{{"price", "0.42"},
                                                                          {"contracts", "1"}}}}},
                           QJsonObject{{"ticker", "KXBTCD-E-T65000"}, {"side", "no"},
                                       {"contracts", "1"}, {"cost", "0.53"},
                                       {"fee", "0.01"},
                                       {"fills", QJsonArray{QJsonObject{{"price", "0.53"},
                                                                          {"contracts", "1"}}}}}}}};
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
                       {"received_at", QDateTime::fromMSecsSinceEpoch(
                           received_ms, QTimeZone::UTC).toString(Qt::ISODateWithMs)},
                       {"quantity", "1"},
                       {"certificate", certificate},
                       {"certificate_sha256", certificate_sha},
                       {"evaluation", evaluation}};
}
} // namespace

class TstKalshiCorridorGate : public QObject {
    Q_OBJECT
  private slots:
    void sealed_risk_envelope_enables_paper_collection_without_history() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;
        const QJsonObject sealed = KalshiCorridorGate::preregister(
            dir.filePath(KalshiCorridorGate::kParamsFile), params(), kNow, &error);
        QVERIFY2(!sealed.isEmpty(), qPrintable(error));
        QVERIFY(KalshiCorridorGate::seal_valid(sealed));

        // Zero scans and zero historical opportunities are expected at the
        // start of a paper experiment. They must not deadlock its collection.
        const QJsonObject verdict = KalshiCorridorGate::evaluate(sealed, {}, kNow + 2);
        QCOMPARE(verdict.value("verdict").toString(), QStringLiteral("PASS"));
        QVERIFY(verdict.value("paper_bids_authorized").toBool());
        QVERIFY(!verdict.value("live_orders_authorized").toBool());
        QCOMPARE(verdict.value("evidence").toObject().value("scans").toInt(), 0);
        QString reason;
        QVERIFY2(KalshiCorridorGate::permits_paper_bid(
                     verdict, scan(0), 0, 1, kNow + 2, &reason), qPrintable(reason));

        // A directional gate can say PASS and still has zero corridor authority.
        const QJsonObject directional{{"verdict", "PASS"},
                                      {"evaluated", true},
                                      {"strategy_family", "KXBTCD"},
                                      {"authority", "live_if_armed"}};
        QVERIFY(!KalshiCorridorGate::permits_paper_bid(
            directional, scan(0), 0, 1, kNow + 2, &reason));
        QVERIFY(reason.contains(QStringLiteral("not the BTC corridor")));
    }

    void each_paper_proposal_must_be_fresh_profitable_and_inside_risk_limits() {
        QTemporaryDir dir;
        QString error;
        const QJsonObject sealed = KalshiCorridorGate::preregister(
            dir.filePath(KalshiCorridorGate::kParamsFile), params(), kNow, &error);
        const QJsonObject verdict = KalshiCorridorGate::evaluate(sealed, {}, kNow + 1);
        QVERIFY(!KalshiCorridorGate::permits_paper_bid(
            verdict, scan(0, false), 0, 1, kNow + 2));
        QVERIFY(!KalshiCorridorGate::permits_paper_bid(
            verdict, scan(0, true, false, kNow - 60'001), 0, 1, kNow + 2));
        QVERIFY(!KalshiCorridorGate::permits_paper_bid(
            verdict, scan(0), 0, 2, kNow + 2)); // scan was quoted at quantity 1

        QJsonObject tampered_scan = scan(0);
        QJsonObject tampered_certificate = tampered_scan.value("certificate").toObject();
        tampered_certificate["event_ticker"] = QStringLiteral("changed");
        tampered_scan["certificate"] = tampered_certificate;
        QVERIFY(!KalshiCorridorGate::permits_paper_bid(
            verdict, tampered_scan, 0, 1, kNow + 2));

        QJsonObject loosened = verdict;
        QJsonObject loosened_params = loosened.value("params").toObject();
        loosened_params["max_cost_per_opportunity_usd"] = 50.0;
        loosened["params"] = loosened_params;
        QVERIFY(!KalshiCorridorGate::permits_paper_bid(
            loosened, scan(0), 0, 1, kNow + 2));

        QJsonObject expensive = scan(0);
        QJsonObject evaluation = expensive.value("evaluation").toObject();
        QJsonArray pairs = evaluation.value("pairs").toArray();
        QJsonObject pair = pairs[0].toObject();
        QJsonObject result = pair.value("evaluation").toObject();
        result["acquisition_cost"] = QStringLiteral("2.01");
        pair["evaluation"] = result;
        pairs[0] = pair;
        evaluation["pairs"] = pairs;
        expensive["evaluation"] = evaluation;
        QVERIFY(!KalshiCorridorGate::permits_paper_bid(
            verdict, expensive, 0, 1, kNow + 2));
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

    void separate_micro_live_seal_enforces_two_dollars_on_each_leg() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QJsonObject micro_params{{"max_bundles_per_opportunity", 2},
                                       {"max_all_in_per_leg_usd", 2.0},
                                       {"max_scan_age_ms", 60'000},
                                       {"max_executions_per_hour", 3}};
        QString error;
        const QJsonObject sealed = KalshiCorridorGate::preregister_micro_live(
            dir.filePath(KalshiCorridorGate::kMicroLiveParamsFile), micro_params,
            kNow, &error);
        QVERIFY2(!sealed.isEmpty(), qPrintable(error));
        QVERIFY(KalshiCorridorGate::seal_valid(sealed));
        QCOMPARE(sealed.value("authority").toString(), QStringLiteral("micro_live_only"));
        QVERIFY(!sealed.value("production_live_authorized").toBool());
        QString reason;
        QVERIFY2(KalshiCorridorGate::permits_micro_live(
                     sealed, scan(0), 0, 1, kNow + 2, &reason), qPrintable(reason));

        QJsonObject expensive = scan(0);
        QJsonObject evaluation = expensive.value("evaluation").toObject();
        QJsonArray pairs = evaluation.value("pairs").toArray();
        QJsonObject pair = pairs[0].toObject();
        QJsonObject result = pair.value("evaluation").toObject();
        QJsonArray legs = result.value("legs").toArray();
        QJsonObject first = legs[0].toObject();
        first["cost"] = QStringLiteral("2.00"); // + fee + half buffer > $2
        legs[0] = first;
        result["legs"] = legs;
        pair["evaluation"] = result;
        pairs[0] = pair;
        evaluation["pairs"] = pairs;
        expensive["evaluation"] = evaluation;
        QVERIFY(!KalshiCorridorGate::permits_micro_live(
            sealed, expensive, 0, 1, kNow + 2, &reason));
        QVERIFY(reason.contains(QStringLiteral("$2")));
    }

    void paper_gate_never_substitutes_for_micro_live_authority() {
        QTemporaryDir dir;
        QString error;
        const QJsonObject paper = KalshiCorridorGate::preregister(
            dir.filePath(KalshiCorridorGate::kParamsFile), params(), kNow, &error);
        QVERIFY(!KalshiCorridorGate::permits_micro_live(
            paper, scan(0), 0, 1, kNow + 2, &error));
        QVERIFY(error.contains(QStringLiteral("micro-live-only")));
    }

    void evidence_is_diagnostic_and_never_a_paper_prerequisite() {
        QTemporaryDir dir;
        QString error;
        const QJsonObject sealed = KalshiCorridorGate::preregister(
            dir.filePath(KalshiCorridorGate::kParamsFile), params(), kNow, &error);
        QJsonArray rows{scan(0, false, true, kNow - 1)};
        const QJsonObject out = KalshiCorridorGate::evaluate(sealed, rows, kNow + 1);
        QCOMPARE(out.value("verdict").toString(), QStringLiteral("PASS"));
        QVERIFY(out.value("paper_bids_authorized").toBool());
        QCOMPARE(out.value("evidence").toObject().value("scans").toInt(), 0);
        QCOMPARE(out.value("evidence").toObject().value("rows_before_seal").toInt(), 1);
    }

    void old_profitability_gate_shape_is_rejected() {
        QJsonObject old{{"min_scans", 300}, {"min_opportunity_scans", 10}};
        QString error;
        QVERIFY(KalshiCorridorGate::parse_params(old, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("unknown corridor gate params")));
    }
};

QTEST_APPLESS_MAIN(TstKalshiCorridorGate)
#include "tst_kalshi_corridor_gate.moc"
