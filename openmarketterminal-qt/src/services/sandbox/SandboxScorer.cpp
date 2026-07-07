#include "services/sandbox/SandboxScorer.h"

#include "services/sandbox/SandboxRegistry.h"
#include "storage/sqlite/Database.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTimeZone>
#include <QVariant>
#include <algorithm>

namespace openmarketterminal::services::sandbox {

namespace {

QJsonObject parse_object(const QString& json) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &pe);
    return pe.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}

QString utc_day_string(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).date().toString(Qt::ISODate);
}

// One closed_at-day-attributable sandbox_position row, as read for both
// score_all's per-day buckets and leaderboard()'s all-time rollup. Only the
// fields either aggregation path needs.
struct ActivityRow {
    QString state;         // 'closed' | 'unfilled'
    bool has_pnl = false;
    double realized_pnl = 0;
    double notional_usd = 0;
    QString data_quality;
    qint64 closed_at = 0;
};

// Accumulates the per-contract-1/2/3 aggregates over a set of ActivityRow
// (either one UTC day's worth, for score_all, or a strategy's entire
// history, for leaderboard()). Shared so score_all and leaderboard can never
// define "resolved" or "degraded" differently from one another.
struct Bucket {
    int resolved_count = 0;
    int unfilled_count = 0;
    int degraded_count = 0;
    int win_count = 0, loss_count = 0;
    double net_pnl = 0, win_sum = 0, loss_sum = 0, gross_notional = 0;

    void add(const ActivityRow& row) {
        if (row.data_quality == QLatin1String("degraded"))
            ++degraded_count;
        if (row.state == QLatin1String("unfilled")) {
            ++unfilled_count;
            return;
        }
        // row.state == 'closed' from here on.
        if (!row.has_pnl)
            return; // data-gap close (contract 1): excluded, never coalesced to 0.
        ++resolved_count;
        net_pnl += row.realized_pnl;
        gross_notional += row.notional_usd; // contract 3: resolved-scoped only.
        if (row.realized_pnl > 0) {
            ++win_count;
            win_sum += row.realized_pnl;
        } else if (row.realized_pnl < 0) {
            ++loss_count;
            loss_sum += row.realized_pnl;
        }
    }

    double hit_rate() const { return resolved_count > 0 ? static_cast<double>(win_count) / resolved_count : 0.0; }
    double avg_win() const { return win_count > 0 ? win_sum / win_count : 0.0; }
    double avg_loss() const { return loss_count > 0 ? loss_sum / loss_count : 0.0; }
};

// Peak-to-trough max drawdown of the cumulative realized-pnl curve over
// `resolved_ordered_pnls` (already ordered by closed_at ascending). A
// positive number (peak minus trough); 0 if the curve never dips below a
// prior high (including the empty/never-in-the-red case).
double max_drawdown_of(const QList<double>& resolved_ordered_pnls) {
    double cumulative = 0, peak = 0, worst = 0;
    for (double pnl : resolved_ordered_pnls) {
        cumulative += pnl;
        peak = std::max(peak, cumulative);
        worst = std::max(worst, peak - cumulative);
    }
    return worst;
}

struct StrategyActivity {
    QList<ActivityRow> rows;   // every closed/unfilled row, ordered by closed_at ascending.
    QList<double> resolved_pnls_ordered; // subset: resolved rows' pnl, same order.
};

Result<StrategyActivity> load_activity(const QString& strategy_id) {
    auto sel = Database::instance().execute(
        "SELECT state, realized_pnl, notional_usd, data_quality, closed_at FROM sandbox_position"
        " WHERE strategy_id = ? AND closed_at IS NOT NULL ORDER BY closed_at ASC",
        {strategy_id});
    if (sel.is_err())
        return Result<StrategyActivity>::err(sel.error());
    StrategyActivity activity;
    auto& q = sel.value();
    while (q.next()) {
        ActivityRow row;
        row.state = q.value(0).toString();
        row.has_pnl = !q.value(1).isNull();
        row.realized_pnl = q.value(1).toDouble();
        row.notional_usd = q.value(2).toDouble();
        row.data_quality = q.value(3).toString();
        row.closed_at = q.value(4).toLongLong();
        activity.rows.append(row);
        if (row.state == QLatin1String("closed") && row.has_pnl)
            activity.resolved_pnls_ordered.append(row.realized_pnl);
    }
    return Result<StrategyActivity>::ok(activity);
}

Result<int> count_open(const QString& strategy_id) {
    auto r = Database::instance().execute(
        "SELECT COUNT(*) FROM sandbox_position WHERE strategy_id = ? AND state IN ('open','pending_fill')",
        {strategy_id});
    if (r.is_err())
        return Result<int>::err(r.error());
    if (!r.value().next())
        return Result<int>::ok(0);
    return Result<int>::ok(r.value().value(0).toInt());
}

} // namespace

Result<void> score_all(const QString& profile, qint64 now_ms) {
    Q_UNUSED(profile); // the DB is a process-wide singleton, same as run_cycle/resolve_pending.
    auto& db = Database::instance();

    auto strategies = list_strategies();
    if (strategies.is_err())
        return Result<void>::err(strategies.error());

    const QString today = utc_day_string(now_ms);
    const QString yesterday = utc_day_string(now_ms - 24LL * 3600 * 1000);

    for (const auto& strat : strategies.value()) {
        auto activity = load_activity(strat.strategy_id);
        if (activity.is_err())
            return Result<void>::err(activity.error());

        auto open_count_r = count_open(strat.strategy_id);
        if (open_count_r.is_err())
            return Result<void>::err(open_count_r.error());
        const int open_count = open_count_r.value();

        QMap<QString, Bucket> days; // QMap keeps YYYY-MM-DD keys sorted lexicographically == chronologically.
        days[yesterday]; // ensure both are present even with zero activity.
        days[today];
        for (const auto& row : activity.value().rows)
            days[utc_day_string(row.closed_at)].add(row);

        const QString latest_day = days.lastKey();
        const double drawdown = max_drawdown_of(activity.value().resolved_pnls_ordered);

        for (auto it = days.constBegin(); it != days.constEnd(); ++it) {
            const QString& score_date = it.key();
            const Bucket& b = it.value();
            auto upd = db.execute(
                "INSERT OR REPLACE INTO sandbox_score (strategy_id, score_date, resolved_count, open_count,"
                " unfilled_count, net_pnl, hit_rate, avg_win, avg_loss, max_drawdown, degraded_count,"
                " gross_notional) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                {strat.strategy_id, score_date, b.resolved_count, open_count, b.unfilled_count, b.net_pnl,
                 b.hit_rate(), b.avg_win(), b.avg_loss(), score_date == latest_day ? drawdown : 0.0,
                 b.degraded_count, b.gross_notional});
            if (upd.is_err())
                return Result<void>::err(upd.error());
        }
    }
    return Result<void>::ok();
}

Result<QList<LeaderboardRow>> leaderboard(const QString& profile) {
    Q_UNUSED(profile);
    auto strategies = list_strategies();
    if (strategies.is_err())
        return Result<QList<LeaderboardRow>>::err(strategies.error());

    auto& db = Database::instance();
    QList<LeaderboardRow> out;
    for (const auto& strat : strategies.value()) {
        auto activity = load_activity(strat.strategy_id);
        if (activity.is_err())
            return Result<QList<LeaderboardRow>>::err(activity.error());

        Bucket b;
        for (const auto& row : activity.value().rows)
            b.add(row);

        // degraded/unknown_count: ALL positions (any state, not just
        // closed_at-day-attributable ones) with that data_quality -- a
        // currently-open position with degraded/unknown freshness still
        // shows up in the book-health signal (see file header; deliberately
        // NOT derived from Bucket, which only sees closed_at-attributable
        // rows). data_gap_count: contract 1's excluded rows, counted
        // explicitly rather than discarded.
        auto degr = db.execute(
            "SELECT COUNT(*) FROM sandbox_position WHERE strategy_id = ? AND data_quality = 'degraded'",
            {strat.strategy_id});
        if (degr.is_err())
            return Result<QList<LeaderboardRow>>::err(degr.error());
        int degraded_count_all = 0;
        if (degr.value().next())
            degraded_count_all = degr.value().value(0).toInt();

        auto unk = db.execute(
            "SELECT COUNT(*) FROM sandbox_position WHERE strategy_id = ? AND data_quality = 'unknown'",
            {strat.strategy_id});
        if (unk.is_err())
            return Result<QList<LeaderboardRow>>::err(unk.error());
        int unknown_count = 0;
        if (unk.value().next())
            unknown_count = unk.value().value(0).toInt();

        auto gap = db.execute(
            "SELECT COUNT(*) FROM sandbox_position WHERE strategy_id = ? AND state = 'closed'"
            " AND realized_pnl IS NULL",
            {strat.strategy_id});
        if (gap.is_err())
            return Result<QList<LeaderboardRow>>::err(gap.error());
        int data_gap_count = 0;
        if (gap.value().next())
            data_gap_count = gap.value().value(0).toInt();

        LeaderboardRow row;
        row.strategy_id = strat.strategy_id;
        row.kind = strat.kind;
        row.status = strat.status;
        row.resolved = b.resolved_count;
        row.net_pnl = b.net_pnl;
        row.hit_rate = b.hit_rate();
        row.max_drawdown = max_drawdown_of(activity.value().resolved_pnls_ordered);
        row.gross_notional = b.gross_notional;
        row.degraded = degraded_count_all;
        row.hypothetical = parse_object(strat.params_json).value(QStringLiteral("hypothetical")).toBool(false);
        row.unknown_count = unknown_count;
        row.data_gap_count = data_gap_count;
        out.append(row);
    }
    return Result<QList<LeaderboardRow>>::ok(out);
}

} // namespace openmarketterminal::services::sandbox
