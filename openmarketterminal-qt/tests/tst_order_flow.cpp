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
#include "storage/repositories/SettingsRepository.h"
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

    // ── Task 4: submit_order tool ───────────────────────────────────────────

    // Helpers: write the GUI-owned gates/caps directly (simulating the GUI
    // toggle). The tool path can't write these — the keystone blocks it — but a
    // direct repo write models the human flipping the switch in GUI Settings.
    void set_gate(const QString& key, const QString& value) {
        auto r = SettingsRepository::instance().set(key, value, "cli");
        QVERIFY2(r.is_ok(), qPrintable("set " + key + " failed"));
    }
    QString prepare_valid_draft() {
        // qty 10 @ 200 = order_value 2000 < 25000 default cap.
        auto res = rt_.call_tool("prepare_order",
                                 QJsonObject{{"symbol", "AAPL"},
                                             {"side", "buy"},
                                             {"quantity", 10},
                                             {"order_type", "limit"},
                                             {"limit_price", 200}});
        if (!res.success)
            return QString();
        return res.data.toObject().value("draft_id").toString();
    }

    // PAPER happy path: with the paper gate ON, a prepared draft submits and
    // executes on the paper rail. Draft walks prepared → submitted; a submit
    // audit row is recorded.
    void submit_paper_executes_when_enabled() {
        set_gate("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_valid_draft();
        QVERIFY2(!draft_id.isEmpty(), "prepare should yield a draft_id");

        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        QVERIFY2(res.success, qPrintable("submit_order paper must reach the handler: " + res.error));
        const QJsonObject data = res.data.toObject();
        const QString status = data.value("status").toString();
        // Prefer "filled"; the paper engine should fill a simple limit order.
        QCOMPARE(status, QStringLiteral("filled"));

        // The draft was consumed (status walked to "submitted").
        auto got = OrderDraftRepository::instance().get(draft_id);
        QVERIFY2(got.is_ok(), "draft must still exist after submit");
        QCOMPARE(got.value().status, QStringLiteral("submitted"));

        // A submit-phase audit row records the decision.
        auto recent = TradeAuditRepository::instance().recent(50);
        QVERIFY2(recent.is_ok(), "audit recent() failed");
        bool found = false;
        for (const TradeAuditRow& r : recent.value())
            if (r.tool == "submit_order" && r.phase == "submit" && r.mode == "paper") {
                found = true;
                break;
            }
        QVERIFY2(found, "expected a submit/paper audit row for submit_order");
    }

    // Double-submit of the same draft must be rejected: the atomic reservation
    // (compare-and-set prepared→submitting) means only the first submit wins;
    // the second sees a non-prepared draft and is refused — a fill cannot be
    // replayed off one draft.
    void submit_paper_double_submit_is_rejected() {
        set_gate("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_valid_draft();
        QVERIFY2(!draft_id.isEmpty(), "prepare should yield a draft_id");

        auto first = rt_.call_tool("submit_order",
                                   QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        QVERIFY2(first.success, qPrintable("first submit must reach the handler: " + first.error));
        QCOMPARE(first.data.toObject().value("status").toString(), QStringLiteral("filled"));

        auto second = rt_.call_tool("submit_order",
                                    QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        QVERIFY2(second.success, qPrintable("second submit is a decision (ok_data): " + second.error));
        QCOMPARE(second.data.toObject().value("status").toString(), QStringLiteral("rejected"));

        // Still exactly one consumed draft; status did not bounce back to prepared.
        auto got = OrderDraftRepository::instance().get(draft_id);
        QVERIFY2(got.is_ok(), "draft must still exist after double submit");
        QCOMPARE(got.value().status, QStringLiteral("submitted"));
    }

    // PAPER with the gate OFF: the handler is the final authority — it re-checks
    // the toggle LIVE and refuses, NEVER executing. Draft stays "prepared".
    void submit_paper_rejected_when_disabled() {
        set_gate("cli.allow_paper_trading", "false");
        const QString draft_id = prepare_valid_draft();
        QVERIFY2(!draft_id.isEmpty(), "prepare should yield a draft_id");

        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        QVERIFY2(res.success, qPrintable("paper-disabled is a decision (ok_data): " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("paper trading disabled", Qt::CaseInsensitive),
                 qPrintable("reason must mention paper trading disabled: " + data.value("reason").toString()));

        // Not executed: the draft is still prepared, not submitted.
        auto got = OrderDraftRepository::instance().get(draft_id);
        QVERIFY2(got.is_ok(), "draft must still exist");
        QCOMPARE(got.value().status, QStringLiteral("prepared"));

        // Audit decision is "denied".
        auto recent = TradeAuditRepository::instance().recent(50);
        QVERIFY2(recent.is_ok(), "audit recent() failed");
        bool found = false;
        for (const TradeAuditRow& r : recent.value())
            if (r.tool == "submit_order" && r.decision == "denied" && r.mode == "paper") {
                found = true;
                break;
            }
        QVERIFY2(found, "expected a submit/denied audit row");

        set_gate("cli.allow_paper_trading", "false");
    }

    // LIVE default-deny (Phase C, Task 3): even with BOTH live gates ARMED (so
    // the checker carve-out lets the call through), the HANDLER refuses when no
    // allowed account is configured — the default-deny live floor. Live NEVER
    // reaches a broker and the draft is untouched. (The full gated live path —
    // FakeBroker fill, daily-loss, revocability — is exercised in
    // tst_live_trading; this pins the equity handler's allowed-account floor.)
    void submit_live_default_deny_no_allowed_account() {
        // Arm the live gates so the checker passes and the handler is the refuser.
        // cli.allowed_account is left UNSET (default-deny).
        set_gate("cli.allow_trading", "true");
        set_gate("cli.live_trading_armed", "true");
        const QString draft_id = prepare_valid_draft();
        QVERIFY2(!draft_id.isEmpty(), "prepare should yield a draft_id");

        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        QVERIFY2(res.success, qPrintable("live default-deny is a decision (ok_data): " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("no allowed account", Qt::CaseInsensitive),
                 qPrintable("reason must be the allowed-account floor: " + data.value("reason").toString()));

        // NEVER executed: the floor is BEFORE the reserve, so the draft remains
        // prepared (no broker call, no reservation).
        auto got = OrderDraftRepository::instance().get(draft_id);
        QVERIFY2(got.is_ok(), "draft must still exist");
        QCOMPARE(got.value().status, QStringLiteral("prepared"));

        // Defaults-off invariant too: with the gates reset, the checker itself
        // denies live (call returns !success) — proving "never executes" holds
        // independent of the toggles.
        set_gate("cli.allow_trading", "false");
        set_gate("cli.live_trading_armed", "false");
        auto res2 = rt_.call_tool("submit_order",
                                  QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        QVERIFY2(!res2.success, "live must be denied at the checker when not armed");
        QCOMPARE(OrderDraftRepository::instance().get(draft_id).value().status,
                 QStringLiteral("prepared"));
    }

    // A missing draft id is a rejected decision, not a crash/execution.
    void submit_missing_draft_rejected() {
        auto res = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", "nonexistent-draft"}, {"mode", "paper"}});
        QVERIFY2(res.success, qPrintable("missing draft is a decision (ok_data): " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("draft not found", Qt::CaseInsensitive),
                 qPrintable("reason must mention draft not found: " + data.value("reason").toString()));
    }

    // REVOCABLE re-check: a draft prepared within caps is REJECTED at submit
    // after the cap is lowered below its value — proving submit re-reads FRESH
    // caps and re-runs the floor (it does NOT trust the draft's stored verdict).
    void submit_revocable_risk_recheck() {
        set_gate("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_valid_draft(); // value 2000, passes at 25000
        QVERIFY2(!draft_id.isEmpty(), "prepare should yield a draft_id");

        // Lower the cap below the order value AFTER prepare.
        set_gate("cli.risk.max_order_value", "100");
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        // Restore the cap regardless of outcome.
        set_gate("cli.risk.max_order_value", "25000");

        QVERIFY2(res.success, qPrintable("risk rejection is a decision (ok_data): " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("max order value", Qt::CaseInsensitive),
                 qPrintable("reason must mention max order value: " + data.value("reason").toString()));

        // Not executed: draft remains prepared (re-check fired before submit).
        auto got = OrderDraftRepository::instance().get(draft_id);
        QVERIFY2(got.is_ok(), "draft must still exist");
        QCOMPARE(got.value().status, QStringLiteral("prepared"));
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstOrderFlow)
#include "tst_order_flow.moc"
