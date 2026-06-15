// tst_order_flow.cpp — storage layer for the AI-trading two-phase order flow
// (Phase A, Task 2). Verifies the v049 migration creates the order_drafts and
// trade_audit tables and that OrderDraftRepository / TradeAuditRepository
// round-trip rows against a real opened DB under a QTemporaryDir HOME.
//
// Bring-up mirrors tst_settings_gate.cpp: a HeadlessRuntime init("default")
// registers all migrations (incl. v049) and opens the DB before any repo call.
//
// This file is EXTENDED by Tasks 3-4 (draft prepare/submit tool flow), so slots
// are kept small and named by behavior.

#include <QtTest>
#include <QTemporaryDir>
#include <QJsonObject>

#include "core/headless/HeadlessRuntime.h"
#include "storage/repositories/OrderDraftRepository.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "storage/sqlite/Database.h"

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

class TstOrderFlow : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir home_;
    HeadlessRuntime rt_;

  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());
        auto r = rt_.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));
    }

    // The v049 migration must have created both tables.
    void tables_exist() {
        for (const QString& tbl : {QStringLiteral("order_drafts"), QStringLiteral("trade_audit")}) {
            auto r = Database::instance().execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name=?", {tbl});
            QVERIFY2(r.is_ok(), qPrintable("sqlite_master query failed for " + tbl));
            QVERIFY2(r.value().next(), qPrintable("table missing: " + tbl));
        }
    }

    // A draft round-trips: insert → get returns equal fields → update_status →
    // get reflects the new status. Every field carries a distinctive value so the
    // round-trip can't pass trivially against the column DEFAULTs.
    void draft_round_trip() {
        OrderDraft d;
        d.draft_id = "draft-abc-123";
        d.intent_json = R"({"symbol":"AAPL","side":"BUY","qty":10})";
        d.risk_verdict_json = R"({"verdict":"allow","score":0.42})";
        d.account = "acct-7";
        d.mode_hint = "paper";
        d.status = "prepared";
        d.created_at = "2026-06-14T10:00:00Z";
        d.expires_at = "2026-06-14T10:05:00Z";

        auto ins = OrderDraftRepository::instance().insert(d);
        QVERIFY2(ins.is_ok(), qPrintable("insert failed: " + QString::fromStdString(ins.is_err() ? ins.error() : "")));

        auto got = OrderDraftRepository::instance().get(d.draft_id);
        QVERIFY2(got.is_ok(), qPrintable("get failed: " + QString::fromStdString(got.is_err() ? got.error() : "")));
        const OrderDraft& g = got.value();
        QCOMPARE(g.draft_id, d.draft_id);
        QCOMPARE(g.intent_json, d.intent_json);
        QCOMPARE(g.risk_verdict_json, d.risk_verdict_json);
        QCOMPARE(g.account, d.account);
        QCOMPARE(g.mode_hint, d.mode_hint);
        QCOMPARE(g.status, d.status);
        QCOMPARE(g.created_at, d.created_at);
        QCOMPARE(g.expires_at, d.expires_at);

        auto upd = OrderDraftRepository::instance().update_status(d.draft_id, "submitted");
        QVERIFY2(upd.is_ok(), "update_status failed");

        auto got2 = OrderDraftRepository::instance().get(d.draft_id);
        QVERIFY2(got2.is_ok(), "get after update failed");
        QCOMPARE(got2.value().status, QStringLiteral("submitted"));
        // Other fields untouched by the status update.
        QCOMPARE(got2.value().intent_json, d.intent_json);
    }

    // An audit row appends (append-only) and reads back with the same fields.
    void audit_append_and_read() {
        TradeAuditRow row;
        row.ts = "2026-06-14T11:22:33Z";
        row.phase = "prepare";
        row.tool = "place_order";
        row.account = "acct-7";
        row.mode = "paper";
        row.intent_json = R"({"symbol":"MSFT","side":"SELL","qty":5})";
        row.decision = "allow";
        row.reason = "within risk limits";
        row.risk_snapshot_json = R"({"buying_power":10000})";

        auto ap = TradeAuditRepository::instance().append(row);
        QVERIFY2(ap.is_ok(), qPrintable("append failed: " + QString::fromStdString(ap.is_err() ? ap.error() : "")));

        auto recent = TradeAuditRepository::instance().recent(10);
        QVERIFY2(recent.is_ok(), "recent failed");
        QVERIFY2(!recent.value().isEmpty(), "no audit rows read back");

        // The most-recent row should match what we appended (only row so far).
        bool found = false;
        for (const TradeAuditRow& r : recent.value()) {
            if (r.ts == row.ts && r.tool == row.tool) {
                QCOMPARE(r.phase, row.phase);
                QCOMPARE(r.account, row.account);
                QCOMPARE(r.mode, row.mode);
                QCOMPARE(r.intent_json, row.intent_json);
                QCOMPARE(r.decision, row.decision);
                QCOMPARE(r.reason, row.reason);
                QCOMPARE(r.risk_snapshot_json, row.risk_snapshot_json);
                found = true;
                break;
            }
        }
        QVERIFY2(found, "appended audit row not found in recent()");
    }

    // ── Task 3: prepare_order tool ──────────────────────────────────────────

    // A valid LIMIT intent within the deterministic caps is PREPARED: the tool
    // returns status "prepared" with a draft_id, the draft row exists in the
    // OrderDraftRepository, and a prepare-phase audit row records "prepared".
    void prepare_order_within_caps_prepares_draft() {
        // qty 10 @ 200 = order_value 2000 < 25000 default cap.
        auto res = rt_.call_tool("prepare_order",
                                 QJsonObject{{"symbol", "AAPL"},
                                             {"side", "buy"},
                                             {"quantity", 10},
                                             {"order_type", "limit"},
                                             {"limit_price", 200}});
        QVERIFY2(res.success, qPrintable("prepare_order should succeed: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("prepared"));

        const QString draft_id = data.value("draft_id").toString();
        QVERIFY2(!draft_id.isEmpty(), "prepared result must carry a draft_id");

        // The draft is durably persisted.
        auto got = OrderDraftRepository::instance().get(draft_id);
        QVERIFY2(got.is_ok(), qPrintable("draft row must exist for " + draft_id));
        QCOMPARE(got.value().status, QStringLiteral("prepared"));
        QCOMPARE(got.value().mode_hint, QStringLiteral("paper"));

        // A prepare-phase audit row records the "prepared" decision.
        auto recent = TradeAuditRepository::instance().recent(50);
        QVERIFY2(recent.is_ok(), "audit recent() failed");
        bool found = false;
        for (const TradeAuditRow& r : recent.value()) {
            if (r.tool == "prepare_order" && r.phase == "prepare" && r.decision == "prepared") {
                found = true;
                break;
            }
        }
        QVERIFY2(found, "expected a prepare/prepared audit row for prepare_order");
    }

    // An oversized intent (order_value above the max-order-value cap) is REJECTED
    // by the deterministic risk floor: status "rejected" with a max-order-value
    // reason, NO draft_id in the result, and a "rejected" audit row.
    void prepare_order_oversized_is_rejected() {
        // qty 1000 @ 200 = order_value 200000 > 25000 default cap.
        auto res = rt_.call_tool("prepare_order",
                                 QJsonObject{{"symbol", "AAPL"},
                                             {"side", "buy"},
                                             {"quantity", 1000},
                                             {"order_type", "limit"},
                                             {"limit_price", 200}});
        // A risk rejection is a valid verdict, returned via ok_data.
        QVERIFY2(res.success, qPrintable("risk rejection is a valid verdict: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("max order value", Qt::CaseInsensitive),
                 qPrintable("reason must mention max order value: " + data.value("reason").toString()));
        // No usable draft created — the rejected verdict carries no draft_id.
        QVERIFY2(data.value("draft_id").toString().isEmpty(),
                 "rejected result must NOT carry a draft_id");

        // An audit row records the "rejected" decision for prepare_order.
        auto recent = TradeAuditRepository::instance().recent(50);
        QVERIFY2(recent.is_ok(), "audit recent() failed");
        bool found = false;
        for (const TradeAuditRow& r : recent.value()) {
            if (r.tool == "prepare_order" && r.phase == "prepare" && r.decision == "rejected") {
                found = true;
                break;
            }
        }
        QVERIFY2(found, "expected a prepare/rejected audit row for prepare_order");
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstOrderFlow)
#include "tst_order_flow.moc"
