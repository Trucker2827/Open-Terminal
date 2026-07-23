#include "services/prediction/kalshi/KalshiSettlementScoreboard.h"

#include <QJsonObject>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

// Matches the win/loss epsilon the closed-bets render loop already uses.
constexpr double kPnlEpsilon = 1e-9;

struct DecidedOutcome {
    QString settled_time;
    int input_order = 0;
    bool win = false;
};

QString signed_money(double value) {
    return QStringLiteral("%1$%2")
        .arg(value < 0.0 ? QStringLiteral("-") : QStringLiteral("+"))
        .arg(std::abs(value), 0, 'f', 2);
}

KalshiSettlementScoreboard::Cohort& cohort_for(
    KalshiSettlementScoreboard::Scoreboard& board, double average_entry_price) {
    const QString key = KalshiSettlementScoreboard::cohort_for_price(average_entry_price);
    if (key == QStringLiteral("longshot")) return board.longshot;
    if (key == QStringLiteral("coinflip")) return board.coinflip;
    return board.favorite;
}

void score_row(KalshiSettlementScoreboard::Scoreboard& board, const QJsonObject& row,
               int input_order, QVector<DecidedOutcome>& decided) {
    const QString status = row.value(QStringLiteral("accounting_status")).toString();
    if (status == QStringLiteral("exact_netted_mixed")) {
        ++board.excluded_mixed;
        return;
    }
    const QJsonValue pnl_value = row.value(QStringLiteral("realized_pnl"));
    const double contracts = row.value(QStringLiteral("yes_count")).toDouble()
        + row.value(QStringLiteral("no_count")).toDouble();
    const double stake = row.value(QStringLiteral("stake")).toDouble();
    if (status != QStringLiteral("exact_one_sided") || !pnl_value.isDouble()
        || contracts <= 0.0 || stake < 0.0) {
        ++board.excluded_incomplete;
        return;
    }
    const double pnl = pnl_value.toDouble();
    auto& cohort = cohort_for(board, stake / contracts);
    cohort.pnl += pnl;
    if (pnl > kPnlEpsilon) ++cohort.wins;
    else if (pnl < -kPnlEpsilon) ++cohort.losses;
    else ++cohort.pushes;
    // Pushes carry no win/loss outcome, so they neither extend nor break a
    // streak.
    if (pnl > kPnlEpsilon || pnl < -kPnlEpsilon)
        decided.push_back({.settled_time = row.value(QStringLiteral("settled_time")).toString(),
                           .input_order = input_order,
                           .win = pnl > kPnlEpsilon});
}

QString cohort_line(const QString& name, const QString& range,
                    const KalshiSettlementScoreboard::Cohort& cohort) {
    if (cohort.sample() == 0)
        return QStringLiteral("%1 %2 · no settlements · n=0").arg(name, range);
    QString record = QStringLiteral("W %1 / L %2").arg(cohort.wins).arg(cohort.losses);
    if (cohort.pushes > 0) record += QStringLiteral(" / P %1").arg(cohort.pushes);
    const QString hit = cohort.decided() > 0
        ? QStringLiteral("%1%").arg(cohort.hit_rate() * 100.0, 0, 'f', 1)
        : QStringLiteral("--");
    return QStringLiteral("%1 %2 · %3 · HIT %4 · P/L %5 · n=%6")
        .arg(name, range, record, hit, signed_money(cohort.pnl))
        .arg(cohort.sample());
}

} // namespace

QString KalshiSettlementScoreboard::cohort_for_price(double average_entry_price) {
    if (average_entry_price <= 0.35) return QStringLiteral("longshot");
    if (average_entry_price < 0.65) return QStringLiteral("coinflip");
    return QStringLiteral("favorite");
}

KalshiSettlementScoreboard::Scoreboard
KalshiSettlementScoreboard::compute(const QJsonArray& settlements) {
    Scoreboard board;
    QVector<DecidedOutcome> decided;
    int input_order = 0;
    for (const auto& value : settlements) {
        const QJsonObject row = value.toObject();
        if (row.isEmpty()) continue;
        score_row(board, row, ++input_order, decided);
    }
    // ISO-8601 UTC settled_time sorts lexicographically — the same ordering
    // contract the settlements bridge relies on. Input order breaks ties.
    std::ranges::sort(decided, [](const DecidedOutcome& a, const DecidedOutcome& b) {
        if (a.settled_time != b.settled_time) return a.settled_time < b.settled_time;
        return a.input_order < b.input_order;
    });
    int run = 0;
    for (const auto& outcome : decided) {
        const int direction = outcome.win ? 1 : -1;
        run = (run != 0 && (run > 0) == outcome.win) ? run + direction : direction;
        board.best_win_streak = std::max(board.best_win_streak, run);
        board.best_loss_streak = std::max(board.best_loss_streak, -run);
    }
    board.current_streak = run;
    return board;
}

QString KalshiSettlementScoreboard::format(const Scoreboard& board) {
    QStringList lines;
    lines << (board.scored() == 0
        ? QStringLiteral("LIVE SCOREBOARD · no exact-accounting settlements yet")
        : QStringLiteral("LIVE SCOREBOARD · %1 exact-accounting settlements")
              .arg(board.scored()));
    lines << cohort_line(QStringLiteral("LONGSHOT"), QStringLiteral("≤35c"), board.longshot);
    lines << cohort_line(QStringLiteral("COIN-FLIP"), QStringLiteral("35–65c"), board.coinflip);
    lines << cohort_line(QStringLiteral("FAVORITE"), QStringLiteral("≥65c"), board.favorite);
    if (board.current_streak == 0) {
        lines << QStringLiteral("STREAK · no decided settlements yet");
    } else {
        lines << QStringLiteral("STREAK now %1%2 · best W%3 / worst L%4")
                     .arg(board.current_streak > 0 ? QStringLiteral("W") : QStringLiteral("L"))
                     .arg(std::abs(board.current_streak))
                     .arg(board.best_win_streak)
                     .arg(board.best_loss_streak);
    }
    if (board.excluded() == 0) {
        lines << QStringLiteral("EXCLUDED 0");
    } else {
        QStringList reasons;
        if (board.excluded_mixed > 0)
            reasons << QStringLiteral("%1 mixed YES/NO turnover").arg(board.excluded_mixed);
        if (board.excluded_incomplete > 0)
            reasons << QStringLiteral("%1 missing exact accounting").arg(board.excluded_incomplete);
        lines << QStringLiteral("EXCLUDED %1: %2")
                     .arg(board.excluded())
                     .arg(reasons.join(QStringLiteral(" · ")));
    }
    return lines.join(QStringLiteral("\n"));
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
