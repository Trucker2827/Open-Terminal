// edge journal command family — compiled as its own TU; see the note in
// CommandDispatch.cpp and EdgeJournalShared.h (MSVC front-end capacity).

#include "cli/EdgeJournalShared.h"

#include "storage/sqlite/Database.h"
#include "services/edge_radar/CryptoMicrostructureRadar.h"
#include <QSqlQuery>
#include "storage/repositories/TradeAuditRepository.h"
#include <QThread>
#include <QTimeZone>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace openmarketterminal::cli {
static services::edge_radar::CryptoMicrostructureWindow
edge_scalp_primary_window(const services::edge_radar::CryptoMicrostructureSnapshot& snapshot,
                          int horizon_sec) {
    services::edge_radar::CryptoMicrostructureWindow best;
    const int preferred = horizon_sec <= 10 ? 5 : (horizon_sec <= 30 ? 15 : 60);
    for (const auto& w : snapshot.windows) {
        if (w.available && w.seconds == preferred)
            return w;
    }
    for (const auto& w : snapshot.windows) {
        if (!w.available)
            continue;
        if (!best.available || std::abs(w.move_pct) > std::abs(best.move_pct))
            best = w;
    }
    return best;
}

static QJsonObject edge_scalp_gate_json(const EdgeScalpGate& gate) {
    return QJsonObject{{"symbol", gate.symbol},
                       {"venue", gate.venue},
                       {"horizon", gate.horizon},
                       {"verdict", gate.verdict},
                       {"action", gate.action},
                       {"journal_id", gate.journal_id},
                       {"direction", gate.decision.direction},
                       {"call", gate.decision.call},
                       {"reference_price", gate.decision.reference_price},
                       {"confidence", gate.decision.confidence},
                       {"expected_capture_bps", gate.expected_capture_bps},
                       {"observed_move_bps", gate.observed_move_bps},
                       {"round_trip_cost_bps", gate.round_trip_cost_bps},
                       {"net_after_cost_bps", gate.net_after_cost_bps},
                       {"fee_bps", gate.fee_bps},
                       {"slippage_bps", gate.slippage_bps},
                       {"safety_bps", gate.safety_bps},
                       {"capture_ratio", gate.capture_ratio},
                       {"min_net_bps", gate.min_net_bps},
                       {"trust", QJsonObject{{"score", gate.trust.trust},
                                             {"status", gate.trust.status},
                                             {"resolved", gate.trust.resolved},
                                             {"decisions", gate.trust.decisions},
                                             {"win_rate", edge_rate(gate.trust.wins, gate.trust.resolved)},
                                             {"profitable_buy_rate", edge_rate(gate.trust.buy_wins, gate.trust.buy_resolved)}}},
                       {"blockers", QJsonArray::fromStringList(gate.blockers)},
                       {"rationale", gate.rationale},
                       {"microstructure", services::edge_radar::CryptoMicrostructureRadar::to_json(gate.snapshot)}};
}

static int edge_emit_scalp_gate(const GlobalOpts& opts, const EdgeScalpGate& gate) {
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(edge_scalp_gate_json(gate)).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("SCALP / INTRADAY GATE\n");
    std::printf("verdict      %s  action=%s\n", qUtf8Printable(gate.verdict), qUtf8Printable(gate.action));
    std::printf("symbol       %s venue=%s horizon=%s journal=%s\n",
                qUtf8Printable(gate.symbol),
                qUtf8Printable(gate.venue),
                qUtf8Printable(gate.horizon),
                qUtf8Printable(gate.journal_id.isEmpty() ? QStringLiteral("-") : gate.journal_id));
    std::printf("price        %s direction=%s confidence=%s freshest=%s age=%lldms live_sources=%d\n",
                qUtf8Printable(edge_price_or_dash(gate.decision.reference_price)),
                qUtf8Printable(gate.decision.direction),
                qUtf8Printable(edge_pct(gate.decision.confidence)),
                qUtf8Printable(gate.snapshot.freshest_source.isEmpty() ? QStringLiteral("-") : gate.snapshot.freshest_source),
                static_cast<long long>(gate.snapshot.freshest_age_ms),
                gate.snapshot.live_sources);
    std::printf("breakeven    round_trip=%.2fbps fee=%.2fbps slippage=%.2fbps spread=%.2fbps safety=%.2fbps\n",
                gate.round_trip_cost_bps,
                gate.fee_bps * 2.0,
                gate.slippage_bps,
                gate.snapshot.cross_source_spread_bps,
                gate.safety_bps);
    std::printf("move         observed=%.2fbps capture=%.2fbps net=%.2fbps min_net=%.2fbps capture_ratio=%.2f\n",
                gate.observed_move_bps,
                gate.expected_capture_bps,
                gate.net_after_cost_bps,
                gate.min_net_bps,
                gate.capture_ratio);
    std::printf("trust        score=%.1f status=%s resolved=%d buy_profit_rate=%s\n",
                gate.trust.trust,
                qUtf8Printable(gate.trust.status.isEmpty() ? QStringLiteral("none") : gate.trust.status),
                gate.trust.resolved,
                qUtf8Printable(gate.trust.buy_resolved ? edge_pct(edge_rate(gate.trust.buy_wins, gate.trust.buy_resolved))
                                                        : QStringLiteral("-")));
    std::printf("flow         tick_direction=%+.2f top_book_imbalance=%+.2f book_sources=%d ticks=%d divergence=%.2fbps\n",
                gate.snapshot.tape_pressure,
                gate.snapshot.book_pressure,
                gate.snapshot.top_book_sources,
                gate.snapshot.tick_count,
                gate.snapshot.cross_source_spread_bps);
    if (!gate.blockers.isEmpty())
        std::printf("blockers     %s\n", qUtf8Printable(gate.blockers.join(QStringLiteral("; "))));
    std::printf("rationale    %s\n", qUtf8Printable(gate.rationale));
    std::printf("action       no order placed; use this as a pre-trade gate, then score the journal later\n");
    return 0;
}

int edge_scalp_gate_command(const GlobalOpts& opts, QStringList args) {
    QString duration_raw;
    QString sources_raw;
    QString venue_raw;
    QString horizon_raw;
    QString fee_raw;
    QString slippage_raw;
    QString safety_raw;
    QString min_net_raw;
    QString capture_raw;
    QString max_spread_raw;
    QString max_age_raw;
    QString min_sources_raw;
    QString min_samples_raw;
    QString min_trust_raw;
    QString interval_raw;
    QString iterations_raw;
    const bool maker = take_bool_flag(args, QStringLiteral("--maker")) ||
                       take_bool_flag(args, QStringLiteral("--post-only"));
    const bool allow_warmup = take_bool_flag(args, QStringLiteral("--allow-warmup"));
    const bool no_journal = take_bool_flag(args, QStringLiteral("--no-journal"));
    const bool watch = take_bool_flag(args, QStringLiteral("--watch"));
    if (!take_string_option(args, QStringLiteral("--duration-ms"), duration_raw) ||
        !take_string_option(args, QStringLiteral("--sources"), sources_raw) ||
        !take_string_option(args, QStringLiteral("--venue"), venue_raw) ||
        !take_string_option(args, QStringLiteral("--horizon-sec"), horizon_raw) ||
        !take_string_option(args, QStringLiteral("--fee-bps"), fee_raw) ||
        !take_string_option(args, QStringLiteral("--slippage-bps"), slippage_raw) ||
        !take_string_option(args, QStringLiteral("--safety-bps"), safety_raw) ||
        !take_string_option(args, QStringLiteral("--min-net-bps"), min_net_raw) ||
        !take_string_option(args, QStringLiteral("--capture-ratio"), capture_raw) ||
        !take_string_option(args, QStringLiteral("--max-spread-bps"), max_spread_raw) ||
        !take_string_option(args, QStringLiteral("--max-age-ms"), max_age_raw) ||
        !take_string_option(args, QStringLiteral("--min-live-sources"), min_sources_raw) ||
        !take_string_option(args, QStringLiteral("--min-samples"), min_samples_raw) ||
        !take_string_option(args, QStringLiteral("--min-trust"), min_trust_raw) ||
        !take_string_option(args, QStringLiteral("--interval-sec"), interval_raw) ||
        !take_string_option(args, QStringLiteral("--iterations"), iterations_raw))
        return 2;

    int duration_ms = 9000;
    if (!edge_parse_duration_ms(duration_raw, duration_ms))
        return 2;
    int horizon_sec = 15;
    if (!horizon_raw.isEmpty()) {
        bool ok = false;
        horizon_sec = horizon_raw.toInt(&ok);
        if (!ok || horizon_sec < 5 || horizon_sec > 3600) {
            std::fprintf(stderr, "--horizon-sec must be 5..3600 for scalp-gate\n");
            return 2;
        }
    }
    double capture_ratio = capture_raw.isEmpty() ? 0.35 : capture_raw.toDouble();
    if (capture_ratio > 1.0)
        capture_ratio /= 100.0;
    double max_spread_bps = max_spread_raw.isEmpty() ? 8.0 : max_spread_raw.toDouble();
    double max_age_ms = max_age_raw.isEmpty() ? 2500.0 : max_age_raw.toDouble();
    int min_live_sources = min_sources_raw.isEmpty() ? 2 : min_sources_raw.toInt();
    int min_samples = min_samples_raw.isEmpty() ? 30 : min_samples_raw.toInt();
    double min_trust = min_trust_raw.isEmpty() ? 45.0 : min_trust_raw.toDouble();
    double safety_bps = safety_raw.isEmpty() ? 5.0 : safety_raw.toDouble();
    double min_net_bps = min_net_raw.isEmpty() ? 5.0 : min_net_raw.toDouble();
    int interval_sec = interval_raw.isEmpty() ? 5 : interval_raw.toInt();
    int iterations = iterations_raw.isEmpty() ? (watch ? 0 : 1) : iterations_raw.toInt();
    if (capture_ratio <= 0.0 || capture_ratio > 1.0 || max_spread_bps < 0.0 || max_age_ms < 100.0 ||
        min_live_sources < 1 || min_live_sources > 10 || min_samples < 0 || min_samples > 10000 ||
        min_trust < 0.0 || min_trust > 100.0 || safety_bps < 0.0 || min_net_bps < 0.0 ||
        interval_sec < 1 || interval_sec > 3600 || iterations < 0 || iterations > 100000) {
        std::fprintf(stderr, "invalid scalp-gate threshold\n");
        return 2;
    }

    const QString symbol_raw = args.isEmpty() ? QStringLiteral("BTC-USD") : args.takeFirst();
    if (!args.isEmpty()) {
        std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
        return 2;
    }

    const QString venue = venue_raw.trimmed().isEmpty() ? QStringLiteral("coinbase") : venue_raw.trimmed();
    const CryptoFeeProfile fee_profile = crypto_fee_profile_for_venue(venue);
    double fee_bps = maker ? fee_profile.maker_bps : fee_profile.taker_bps;
    double slippage_bps = fee_profile.slippage_bps;
    if (!edge_parse_bps_text(fee_raw, fee_bps, "--fee-bps") ||
        !edge_parse_bps_text(slippage_raw, slippage_bps, "--slippage-bps"))
        return 2;

    int code = 0;
    if (!init_headless_for_cli(opts, code))
        return code;

    const QString symbol = services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol_raw);
    auto build_gate = [&]() -> Result<EdgeScalpGate> {
        EdgeScalpGate gate;
        gate.symbol = symbol;
        gate.venue = crypto_fee_venue_key(venue);
        gate.horizon_sec = horizon_sec;
        gate.horizon = QStringLiteral("%1s").arg(horizon_sec);
        gate.fee_bps = fee_bps;
        gate.slippage_bps = slippage_bps;
        gate.safety_bps = safety_bps;
        gate.capture_ratio = capture_ratio;
        gate.min_net_bps = min_net_bps;
        gate.snapshot = edge_capture_microstructure(gate.symbol,
                                                    edge_safe_latency_sources_for_symbol(sources_raw, gate.symbol),
                                                    duration_ms,
                                                    true);
        gate.decision = edge_score_crypto_recommendation(gate.symbol, venue, gate.snapshot,
                                                         horizon_sec, fee_bps * 2.0 + safety_bps,
                                                         slippage_bps, min_net_bps);
        gate.trust = edge_crypto_trust_for_symbol(gate.symbol, gate.horizon, 24 * 14);

        const auto primary = edge_scalp_primary_window(gate.snapshot, horizon_sec);
        gate.observed_move_bps = primary.available ? primary.move_pct * 100.0 : 0.0;
        const double directional_move_bps = gate.decision.direction == QLatin1String("up")
                                                ? std::max(0.0, gate.observed_move_bps)
                                                : 0.0;
        gate.expected_capture_bps = directional_move_bps * gate.capture_ratio * gate.decision.confidence;
        gate.round_trip_cost_bps = (fee_bps * 2.0) + slippage_bps +
                                   std::max(0.0, gate.snapshot.cross_source_spread_bps) + safety_bps;
        gate.net_after_cost_bps = gate.expected_capture_bps - gate.round_trip_cost_bps;

        if (gate.snapshot.freshest_age_ms < 0 || gate.snapshot.freshest_age_ms > static_cast<qint64>(max_age_ms))
            gate.blockers << QStringLiteral("freshest tick too stale");
        if (gate.snapshot.live_sources < min_live_sources)
            gate.blockers << QStringLiteral("not enough live sources");
        if (gate.snapshot.cross_source_spread_bps > max_spread_bps)
            gate.blockers << QStringLiteral("spread/divergence too wide");
        if (!primary.available)
            gate.blockers << QStringLiteral("not enough window history");
        if (gate.decision.direction != QLatin1String("up"))
            gate.blockers << QStringLiteral("spot scalp only supports long/up candidates");
        if (gate.net_after_cost_bps < min_net_bps)
            gate.blockers << QStringLiteral("estimated captured move does not clear round-trip cost");
        if (!allow_warmup && gate.trust.resolved < min_samples)
            gate.blockers << QStringLiteral("not enough resolved local samples");
        if (!allow_warmup && gate.trust.trust < min_trust)
            gate.blockers << QStringLiteral("local trust score below gate");

        if (gate.blockers.isEmpty()) {
            gate.verdict = QStringLiteral("TRADE CANDIDATE");
            gate.action = QStringLiteral("LIMIT_BUY_ONLY");
        } else if (gate.decision.direction == QLatin1String("up") && gate.snapshot.live_sources >= min_live_sources &&
                   gate.snapshot.freshest_age_ms >= 0 && gate.snapshot.freshest_age_ms <= static_cast<qint64>(max_age_ms)) {
            gate.verdict = QStringLiteral("WATCH");
            gate.action = QStringLiteral("NO_ORDER");
        } else {
            gate.verdict = QStringLiteral("NO TRADE");
            gate.action = QStringLiteral("NO_ORDER");
        }
        gate.rationale = QStringLiteral("scalp gate: expected capture %1bps vs round-trip %2bps; trust %3/%4 resolved")
                             .arg(gate.expected_capture_bps, 0, 'f', 2)
                             .arg(gate.round_trip_cost_bps, 0, 'f', 2)
                             .arg(gate.trust.trust, 0, 'f', 1)
                             .arg(gate.trust.resolved);

        if (!no_journal) {
            auto inserted = edge_journal_insert_crypto_recommendation(gate.decision, gate.snapshot,
                                                                      horizon_sec, fee_bps * 2.0 + safety_bps,
                                                                      slippage_bps, min_net_bps);
            if (inserted.is_err())
                return Result<EdgeScalpGate>::err(inserted.error());
            gate.journal_id = inserted.value();
        }
        return Result<EdgeScalpGate>::ok(gate);
    };

    if (opts.json && iterations != 1) {
        QJsonArray rows;
        int rendered = 0;
        while (iterations == 0 || rendered < iterations) {
            auto r = build_gate();
            if (r.is_err()) {
                std::fprintf(stderr, "failed to journal scalp gate: %s\n", r.error().c_str());
                return 5;
            }
            rows.append(edge_scalp_gate_json(r.value()));
            ++rendered;
            if (iterations > 0 && rendered >= iterations)
                break;
            QThread::sleep(static_cast<unsigned long>(interval_sec));
        }
        std::printf("%s\n", QJsonDocument(QJsonObject{{"rows", rows}}).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    int rendered = 0;
    while (iterations == 0 || rendered < iterations) {
        if (watch && !opts.json)
            std::printf("\033[2J\033[H");
        auto r = build_gate();
        if (r.is_err()) {
            std::fprintf(stderr, "failed to journal scalp gate: %s\n", r.error().c_str());
            return 5;
        }
        const int rc = edge_emit_scalp_gate(opts, r.value());
        std::fflush(stdout);
        if (rc != 0 || opts.json)
            return rc;
        ++rendered;
        if (iterations > 0 && rendered >= iterations)
            break;
        if (watch)
            std::printf("\nsession      refreshing every %ds; press Ctrl-C to stop\n", interval_sec);
        QThread::sleep(static_cast<unsigned long>(interval_sec));
    }
    return 0;
}

int edge_journal_regimes_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString horizon;
    QString max_age_raw;
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--horizon"), horizon) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal regimes [--symbol BTC-USD] [--horizon 60s] [--max-age-hours N]\n");
        return 2;
    }
    const int max_age_hours = max_age_raw.isEmpty() ? 24 * 7 : max_age_raw.toInt();
    QString where = QStringLiteral(" WHERE source='edge crypto-recommend' AND created_at>=?");
    QVariantList params{QDateTime::currentMSecsSinceEpoch() -
                        static_cast<qint64>(max_age_hours) * 3600000LL};
    if (!symbol.trimmed().isEmpty()) {
        where += QStringLiteral(" AND symbol=?");
        params << services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
    }
    horizon = edge_normalize_stats_horizon(horizon);
    if (!horizon.isEmpty() && horizon != QLatin1String("all")) {
        where += QStringLiteral(" AND horizon=?");
        params << horizon;
    }
    auto r = Database::instance().execute(
        QStringLiteral("SELECT %1 FROM edge_decision_journal%2 ORDER BY created_at DESC")
            .arg(edge_journal_cols(), where),
        params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    struct RegimeStats { int total = 0; int resolved = 0; int wins = 0; int buys = 0; double edge = 0.0; };
    QMap<QString, RegimeStats> stats;
    auto& q = r.value();
    while (q.next()) {
        const QJsonObject features = QJsonDocument::fromJson(q.value(24).toString().toUtf8()).object();
        const QString regime = edge_crypto_regime_from_features(features);
        auto s = stats.value(regime);
        ++s.total;
        s.edge += q.value(15).toDouble();
        if (q.value(26).toInt() >= 0) {
            ++s.resolved;
            if (q.value(26).toInt() == 1)
                ++s.wins;
        }
        if (edge_crypto_is_buy_call(q.value(10).toString(), q.value(9).toString()))
            ++s.buys;
        stats.insert(regime, s);
    }
    QJsonArray rows;
    if (!opts.json)
        std::printf("%-14s %9s %9s %8s %7s %9s\n", "REGIME", "DECISIONS", "RESOLVED", "WIN%", "BUYS", "AVG_NET");
    for (auto it = stats.cbegin(); it != stats.cend(); ++it) {
        const auto s = it.value();
        const double avg_edge = s.total > 0 ? s.edge / s.total : 0.0;
        if (opts.json) {
            rows.append(QJsonObject{{"regime", it.key()}, {"decisions", s.total}, {"resolved", s.resolved},
                                    {"wins", s.wins}, {"win_rate", edge_rate(s.wins, s.resolved)},
                                    {"buy_candidates", s.buys}, {"average_edge_after_cost", avg_edge}});
        } else {
            std::printf("%-14s %9d %9d %8s %7d %9s\n",
                        qUtf8Printable(it.key()), s.total, s.resolved,
                        qUtf8Printable(s.resolved ? edge_pct(edge_rate(s.wins, s.resolved)) : QStringLiteral("-")),
                        s.buys, qUtf8Printable(edge_pct(avg_edge)));
        }
    }
    if (opts.json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"rows", rows}}).toJson(QJsonDocument::Compact).constData());
    return 0;
}

int edge_journal_no_trade_command(const GlobalOpts& opts, QStringList args) {
    QString limit_raw;
    QString max_age_raw;
    if (!take_string_option(args, QStringLiteral("--limit"), limit_raw) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal no-trade [--limit N] [--max-age-hours N]\n");
        return 2;
    }
    int limit = limit_raw.isEmpty() ? 20 : limit_raw.toInt();
    int max_age_hours = max_age_raw.isEmpty() ? 24 : max_age_raw.toInt();
    if (limit < 1 || limit > 200 || max_age_hours < 1 || max_age_hours > 8760) {
        std::fprintf(stderr, "--limit must be 1..200 and --max-age-hours 1..8760\n");
        return 2;
    }
    auto r = Database::instance().execute(
        QStringLiteral("SELECT %1 FROM edge_decision_journal"
                       " WHERE source='edge crypto-recommend'"
                       " AND call!='BUY CANDIDATE' AND created_at>=?"
                       " ORDER BY created_at DESC LIMIT ?").arg(edge_journal_cols()),
        {QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(max_age_hours) * 3600000LL, limit});
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    // Serial MSVC C1001 site (v0.3.25/26/29): the in-loop multi-arg printf of
    // Qt temporaries here has crashed the compiler across THREE workaround
    // generations (raw temporaries, hoisted QByteArrays, own-TU). Current
    // shape: no printf in the query loop and no varargs formatting at all —
    // rows are collected first, then each display line is composed with
    // QString::arg and emitted via a single fputs. Do not "simplify" this
    // back into printf without a green Windows release build.
    QJsonArray rows;
    auto& q = r.value();
    while (q.next())
        rows.append(edge_journal_row_to_json(q));
    if (opts.json) {
        QJsonObject out;
        out.insert(QStringLiteral("rows"), rows);
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::fputs("TIME                 SYMBOL    CALL           CONF     NET      REASON\n", stdout);
    for (const auto& v : rows) {
        const QJsonObject row = v.toObject();
        const QString line = QStringLiteral("%1 %2 %3 %4 %5 %6\n")
                                 .arg(row.value(QStringLiteral("created_at")).toString().left(19), -20)
                                 .arg(row.value(QStringLiteral("symbol")).toString(), -9)
                                 .arg(elide_text(row.value(QStringLiteral("call")).toString(), 14), -14)
                                 .arg(edge_pct(row.value(QStringLiteral("confidence")).toDouble()), -8)
                                 .arg(edge_pct(row.value(QStringLiteral("edge_after_cost")).toDouble()), -8)
                                 .arg(elide_text(row.value(QStringLiteral("reasons")).toString(), 110));
        std::fputs(line.toUtf8().constData(), stdout);
    }
    return 0;
}

} // namespace openmarketterminal::cli
