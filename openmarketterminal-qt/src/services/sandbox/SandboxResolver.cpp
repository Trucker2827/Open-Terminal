#include "services/sandbox/SandboxResolver.h"

#include "services/sandbox/PaperFillModel.h"
#include "services/sandbox/TickTail.h"
#include "storage/sqlite/Database.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QUuid>
#include <QVariant>

namespace openmarketterminal::services::sandbox {

namespace {

QString new_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject parse_object(const QString& json) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &pe);
    return pe.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}

// Same default (coinbase_advanced non-VIP maker/taker) and param keys as
// PaperExecutor.cpp's fee_model_from_params -- duplicated rather than shared
// because PaperExecutor.cpp keeps it in its own anonymous namespace.
FeeModel fee_model_from_params(const QJsonObject& params) {
    FeeModel fm;
    fm.maker_bps = params.contains(QStringLiteral("maker_bps"))
                       ? params.value(QStringLiteral("maker_bps")).toDouble()
                       : 40.0;
    fm.taker_bps = params.contains(QStringLiteral("taker_bps"))
                       ? params.value(QStringLiteral("taker_bps")).toDouble()
                       : 60.0;
    return fm;
}

bool is_recognized_side(const QString& side) {
    return side == QLatin1String("buy") || side == QLatin1String("sell") ||
           side == QLatin1String("long") || side == QLatin1String("short");
}

Result<void> insert_fill(const QString& position_id, qint64 ts, const QString& kind, double price, double fee,
                          const QString& note) {
    auto r = Database::instance().execute(
        "INSERT INTO sandbox_fill (fill_id, position_id, ts, kind, price, fee, note) VALUES (?,?,?,?,?,?,?)",
        {new_id(), position_id, ts, kind, price, fee, note});
    if (r.is_err())
        return Result<void>::err(r.error());
    return Result<void>::ok();
}

struct PredictionRow {
    QString position_id, decision_id;
    double limit_price = 0, entry_fee = 0, qty = 0;
    qint64 expires_at = 0;
};

// Prediction books (side 'yes' or 'no', hypothetical=0): settle against
// edge_decision_journal.outcome. See SandboxResolver.h for the full contract
// and the outcome-semantics citation.
Result<void> resolve_predictions(qint64 now_ms, ResolveReport& report) {
    auto& db = Database::instance();
    auto sel = db.execute(
        "SELECT position_id, decision_id, limit_price, entry_fee, qty, expires_at"
        " FROM sandbox_position WHERE state = 'open' AND side IN ('yes','no') AND hypothetical = 0");
    if (sel.is_err())
        return Result<void>::err(sel.error());

    QList<PredictionRow> rows;
    {
        auto& q = sel.value();
        while (q.next()) {
            PredictionRow row;
            row.position_id = q.value(0).toString();
            row.decision_id = q.value(1).toString();
            row.limit_price = q.value(2).toDouble();
            row.entry_fee = q.value(3).toDouble();
            row.qty = q.value(4).toDouble();
            row.expires_at = q.value(5).toLongLong();
            rows.append(row);
        }
    }

    for (const auto& row : rows) {
        auto jr = db.execute("SELECT outcome FROM edge_decision_journal WHERE id = ?", {row.decision_id});
        if (jr.is_err())
            return Result<void>::err(jr.error());
        bool found = false;
        int outcome = -1;
        if (jr.value().next()) {
            found = true;
            outcome = jr.value().value(0).toInt();
        }

        if (found && (outcome == 0 || outcome == 1)) {
            const double payout = outcome == 1 ? 1.0 : 0.0;
            // No exit fee: settlement is not a trade (controller decision --
            // see SandboxResolver.h).
            const double pnl = (payout - row.limit_price) * row.qty - row.entry_fee;
            auto upd = db.execute(
                "UPDATE sandbox_position SET state='closed', closed_at=?, exit_fee=0, realized_pnl=?,"
                " close_reason='resolved' WHERE position_id=? AND state='open'",
                {now_ms, pnl, row.position_id});
            if (upd.is_err())
                return Result<void>::err(upd.error());
            if (upd.value().numRowsAffected() == 0) {
                report.pending++;
                continue;
            }
            auto f = insert_fill(row.position_id, now_ms, QStringLiteral("resolved"), payout, 0.0, QStringLiteral(""));
            if (f.is_err())
                return Result<void>::err(f.error());
            report.resolved++;
            continue;
        }

        // outcome == -1, or no journal row at all (defensive: nothing to
        // settle against, treated the same) -- give the journal a grace
        // window past expires_at before giving up as 'never resolved'.
        if (now_ms >= row.expires_at + kResolveGraceMs) {
            auto upd = db.execute(
                "UPDATE sandbox_position SET state='closed', closed_at=?, exit_fee=0, realized_pnl=NULL,"
                " close_reason='expiry', data_quality='degraded' WHERE position_id=? AND state='open'",
                {now_ms, row.position_id});
            if (upd.is_err())
                return Result<void>::err(upd.error());
            if (upd.value().numRowsAffected() == 0) {
                report.pending++;
                continue;
            }
            auto f = insert_fill(row.position_id, now_ms, QStringLiteral("expiry"), 0.0, 0.0,
                                  QStringLiteral("never resolved"));
            if (f.is_err())
                return Result<void>::err(f.error());
            report.resolved++;
        } else {
            report.pending++;
        }
    }
    return Result<void>::ok();
}

struct HypotheticalRow {
    QString position_id, symbol, side;
    double limit_price = 0, entry_fee = 0, notional_usd = 0, qty = 0;
    qint64 expires_at = 0, opened_at = 0;
    bool has_target = false, has_stop = false;
    double target_price = 0, stop_price = 0;
    QJsonObject params;
};

// Hypothetical (long_short) books (hypothetical=1): tick-resolve against
// target/stop/expiry exactly like PaperExecutor::advance_open_positions does
// for concrete positions -- see SandboxResolver.h for why this resolver, not
// run_cycle, is the one to do it.
Result<void> resolve_hypotheticals(const QString& ticks_path, qint64 now_ms, ResolveReport& report) {
    auto& db = Database::instance();
    auto sel = db.execute(
        "SELECT p.position_id, p.symbol, p.side, p.limit_price, p.target_price, p.stop_price, p.expires_at,"
        " p.opened_at, p.entry_fee, p.notional_usd, p.qty, s.params_json"
        " FROM sandbox_position p JOIN sandbox_strategy s ON s.strategy_id = p.strategy_id"
        " WHERE p.state = 'open' AND p.hypothetical = 1");
    if (sel.is_err())
        return Result<void>::err(sel.error());

    QList<HypotheticalRow> rows;
    {
        auto& q = sel.value();
        while (q.next()) {
            HypotheticalRow row;
            row.position_id = q.value(0).toString();
            row.symbol = q.value(1).toString();
            row.side = q.value(2).toString();
            row.limit_price = q.value(3).toDouble();
            row.has_target = !q.value(4).isNull();
            row.target_price = q.value(4).toDouble();
            row.has_stop = !q.value(5).isNull();
            row.stop_price = q.value(5).toDouble();
            row.expires_at = q.value(6).toLongLong();
            row.opened_at = q.value(7).toLongLong();
            row.entry_fee = q.value(8).toDouble();
            row.notional_usd = q.value(9).toDouble();
            row.qty = q.value(10).toDouble();
            row.params = parse_object(q.value(11).toString());
            rows.append(row);
        }
    }

    for (const auto& row : rows) {
        if (!is_recognized_side(row.side)) {
            qWarning("SandboxResolver: unrecognized side '%s' for hypothetical position %s; skipping",
                      qUtf8Printable(row.side), qUtf8Printable(row.position_id));
            continue;
        }

        // NULL target/stop maps to a per-side never-trigger sentinel, same
        // as advance_open_positions (task-5 review fix 2).
        const bool short_side = row.side == QLatin1String("short") || row.side == QLatin1String("sell");
        constexpr double kNever = 1e308;
        const double target_price = row.has_target ? row.target_price : (short_side ? 0.0 : kNever);
        const double stop_price = row.has_stop ? row.stop_price : (short_side ? kNever : 0.0);

        const auto ticks = ticks_since(ticks_path, row.symbol, row.opened_at);
        const ExitResult exit = check_exit(row.side, target_price, stop_price, row.expires_at, ticks, now_ms);
        if (!exit.exited) {
            // Not yet past its own expiry -- still normally trading, not
            // stuck awaiting resolution. Nothing to do this cycle.
            continue;
        }

        if (exit.reason == QLatin1String("expiry") && exit.price <= 0.0) {
            // Data gap: no pre-expiry tick. Ticks are bounded at expires_at
            // (PaperFillModel::check_exit's own contract), so re-checking
            // sooner cannot change today's verdict -- but a lagging tick
            // writer may still backfill pre-expiry ticks by the time the
            // grace window elapses, so don't give up immediately.
            if (now_ms < row.expires_at + kResolveGraceMs) {
                report.pending++;
                continue;
            }
            auto upd = db.execute(
                "UPDATE sandbox_position SET state='closed', closed_at=?, exit_fee=0, realized_pnl=NULL,"
                " close_reason='expiry', data_quality='degraded' WHERE position_id=? AND state='open'",
                {exit.ts_ms, row.position_id});
            if (upd.is_err())
                return Result<void>::err(upd.error());
            if (upd.value().numRowsAffected() == 0) {
                report.pending++;
                continue;
            }
            auto f = insert_fill(row.position_id, exit.ts_ms, QStringLiteral("expiry"), 0.0, 0.0,
                                  QStringLiteral("never resolved"));
            if (f.is_err())
                return Result<void>::err(f.error());
            report.resolved++;
            continue;
        }

        // Real target/stop/expiry-with-a-tick-price exit.
        const FeeModel fees = fee_model_from_params(row.params);
        const double exit_fee = fee_for(row.notional_usd, fees.taker_bps);
        const double pnl = realized_pnl(row.side, row.limit_price, exit.price, row.qty, row.entry_fee, exit_fee);

        auto upd = db.execute(
            "UPDATE sandbox_position SET state='closed', closed_at=?, exit_fee=?, realized_pnl=?, close_reason=?"
            " WHERE position_id=? AND state='open'",
            {exit.ts_ms, exit_fee, pnl, exit.reason, row.position_id});
        if (upd.is_err())
            return Result<void>::err(upd.error());
        if (upd.value().numRowsAffected() == 0) {
            report.pending++;
            continue;
        }
        auto f = insert_fill(row.position_id, exit.ts_ms, exit.reason, exit.price, exit_fee, QStringLiteral(""));
        if (f.is_err())
            return Result<void>::err(f.error());
        report.resolved++;
    }
    return Result<void>::ok();
}

} // namespace

Result<ResolveReport> resolve_pending(const QString& profile, const QString& ticks_path, qint64 now_ms) {
    Q_UNUSED(profile); // the DB is a process-wide singleton, same as run_cycle.
    ResolveReport report;

    auto pred = resolve_predictions(now_ms, report);
    if (pred.is_err())
        return Result<ResolveReport>::err(pred.error());

    auto hyp = resolve_hypotheticals(ticks_path, now_ms, report);
    if (hyp.is_err())
        return Result<ResolveReport>::err(hyp.error());

    return Result<ResolveReport>::ok(report);
}

} // namespace openmarketterminal::services::sandbox
