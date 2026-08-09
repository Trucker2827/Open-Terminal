#pragma once

// Pure presentation for the Crypto SPOT / SCALP automation cockpit.
// Pattern mirrors Kalshi's present_bot_cockpit: scene in, paint/bind out.
// Inspect-only — never arms canary, never places orders. Vocabulary is
// PAPER SCALP / CANARY ON (not LIVE ARMED).

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace openmarketterminal::screens::crypto {

inline constexpr int kCryptoCockpitMaxTapeRows = 12;
inline constexpr qint64 kCryptoQualificationFreshMs = 120'000;

/// Security gates the canary still needs (Settings → Security). Passed in so
/// unit tests never touch SettingsRepository / mcp::SettingsGate.
struct CryptoCockpitSecurityInputs {
    bool kill_switch = false;
    bool cli_trading_allowed = false;
    bool cli_live_armed = false;
    bool cli_fast_live_armed = false;
    bool venue_allowed = false;
    QStringList allowed_venues;
};

struct CryptoCockpitInputs {
    QString exchange_id = QStringLiteral("coinbase");
    bool is_paper = true;
    QJsonObject scalp_state;
    QJsonObject scalp_engine;
    QJsonObject live_guard;
    QJsonObject qualification;       ///< scalp_qualification_v1.json body
    qint64 qualification_age_ms = -1; ///< file age; <0 means missing/unknown
    CryptoCockpitSecurityInputs security;
    qint64 now_ms = 0;
};

struct CryptoCockpitTapeRow {
    QString symbol;
    QString verdict;
    QString direction;
    double net_bps = 0.0;
    bool net_known = false;
    double required_bps = 0.0;
    bool required_known = false;
    QString liquidity; ///< "maker" | "taker"
    QString selected_venue;
    QStringList blockers;
};

/// Shadow proof row bound to crypto-scalp-qualification-v1 (not SQLite recommend).
struct CryptoCockpitProofRow {
    QString scope;
    QString verdict;      ///< QUALIFIED / SHADOW / AWAITING / NO DATA / …
    QString verdict_role; ///< green | red | amber | grey
    QString sample;       ///< "resolved/candidates · …"
    QString mean_net;     ///< bps text
    QString win_rate;     ///< percent text
    QString coverage;     ///< coverage or CI summary
    bool has_metrics = false;
};

struct CryptoCockpitScene {
    // Mood / authority (read-only)
    QString mood;              ///< "PAPER SCALP" | "CANARY ON"
    QString mood_role;         ///< "cyan" | "amber" | "green" | "red" | "grey"
    QString mood_detail;       ///< expiry and/or blocker summary
    QStringList blockers;      ///< security / canary blockers (empty ⇒ clear)
    bool canary_on = false;
    QString canary_expires_at;

    // Venue / engine
    QString venue_line;
    QString venue_role;
    QString style; ///< "SCALP" | "SPOT"
    QString engine_line;
    QString engine_role;
    bool engine_running = false;
    QString heartbeat;
    QString heartbeat_role;
    QString hurdle_line;

    // DECIDE (latest decision)
    QString decide_symbol;
    QString decide_verdict;
    QString decide_direction;
    QString decide_liquidity; ///< "MAKER" | "TAKER" | "--"
    QString decide_reference;
    QString decide_required;
    QString decide_net;
    QString decide_blockers;
    bool decide_is_candidate = false;
    QString decide_role;

    // Opportunity tape (newest first; not Kalshi rain)
    QList<CryptoCockpitTapeRow> tape;
    QString tape_census;

    // Feeds
    QString feeds_line;

    // Qualification (crypto-scalp-qualification-v1)
    QString qualification_state; ///< "QUALIFIED" | "FAIL" | "UNAVAILABLE" | "STALE"
    QString qualification_role;
    QString qualification_detail;

    // SCALP SHADOW PROOF — same report authority as qualification (jsonl)
    CryptoCockpitProofRow proof_all;
    CryptoCockpitProofRow proof_symbol;
    QString proof_status;
};

inline QString crypto_cockpit_venue_title(const QString& id) {
    if (id.compare(QLatin1String("coinbase"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("COINBASE ADVANCED");
    if (id.compare(QLatin1String("kraken"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("KRAKEN PRO");
    return id.toUpper();
}

inline QString crypto_cockpit_bps_text(double bps) {
    return QString::number(bps, 'f', 1) + QStringLiteral(" bps");
}

inline QString crypto_cockpit_heartbeat_text(const QString& iso_time, qint64 now_ms) {
    const QDateTime at = QDateTime::fromString(iso_time, Qt::ISODateWithMs);
    if (!at.isValid()) return QStringLiteral("NO DAEMON HEARTBEAT");
    const qint64 seconds = (now_ms - at.toMSecsSinceEpoch()) / 1000;
    if (seconds < 0) return QStringLiteral("NO DAEMON HEARTBEAT");
    if (seconds < 60) return QStringLiteral("DAEMON LIVE %1s").arg(seconds);
    return QStringLiteral("DAEMON STALE %1m").arg(seconds / 60);
}

inline QStringList crypto_cockpit_security_blockers(const CryptoCockpitSecurityInputs& security,
                                                    const QString& exchange_id) {
    QStringList blocked;
    if (security.kill_switch) blocked << QStringLiteral("kill switch is on");
    if (!security.cli_trading_allowed) blocked << QStringLiteral("CLI trading is off");
    if (!security.cli_live_armed) blocked << QStringLiteral("CLI LIVE trading is not armed");
    if (!security.cli_fast_live_armed) blocked << QStringLiteral("FAST live mode is not armed");
    if (!security.venue_allowed) {
        blocked << QStringLiteral("%1 is not in allowed AI venues")
                       .arg(exchange_id.isEmpty() ? QStringLiteral("venue") : exchange_id.toLower());
    }
    return blocked;
}

inline void crypto_cockpit_qualification_of(const QJsonObject& qualification,
                                            qint64 qualification_age_ms,
                                            QString* state, QString* role, QString* detail) {
    if (qualification.isEmpty()) {
        *state = QStringLiteral("UNAVAILABLE");
        *role = QStringLiteral("grey");
        *detail = QStringLiteral("no scalp_qualification_v1.json");
        return;
    }
    const QString version = qualification.value(QStringLiteral("report_version")).toString();
    if (version != QLatin1String("crypto-scalp-qualification-v1")) {
        *state = QStringLiteral("UNAVAILABLE");
        *role = QStringLiteral("grey");
        *detail = QStringLiteral("qualification report version missing or unknown");
        return;
    }
    const QString raw = qualification.value(QStringLiteral("state")).toString();
    const bool eligible = qualification.value(QStringLiteral("execution_eligible")).toBool();
    const bool fresh =
        qualification_age_ms >= 0 && qualification_age_ms <= kCryptoQualificationFreshMs;
    if (raw == QLatin1String("QUALIFIED") && eligible && !fresh) {
        *state = QStringLiteral("STALE");
        *role = QStringLiteral("amber");
        *detail = QStringLiteral("QUALIFIED but report older than %1s")
                      .arg(kCryptoQualificationFreshMs / 1000);
        return;
    }
    if (raw == QLatin1String("QUALIFIED") && eligible) {
        *state = QStringLiteral("QUALIFIED");
        *role = QStringLiteral("green");
        *detail = QStringLiteral("execution_eligible");
        return;
    }
    *state = QStringLiteral("FAIL");
    *role = QStringLiteral("red");
    *detail = raw.isEmpty() ? QStringLiteral("not QUALIFIED")
                            : (eligible ? raw : raw + QStringLiteral(" · not execution_eligible"));
}

inline QString crypto_cockpit_feeds_line(const QJsonObject& decision) {
    const QJsonArray sources = decision.value(QStringLiteral("microstructure"))
                                   .toObject()
                                   .value(QStringLiteral("sources"))
                                   .toArray();
    QStringList parts;
    for (const auto& value : sources) {
        const QJsonObject source = value.toObject();
        const QJsonValue age = source.value(QStringLiteral("age_ms"));
        const qint64 age_ms =
            age.isString() ? age.toString().toLongLong() : static_cast<qint64>(age.toDouble(-1));
        const QString name = source.value(QStringLiteral("source")).toString().toUpper();
        const QString status = source.value(QStringLiteral("status")).toString().toUpper();
        if (!name.isEmpty())
            parts << QStringLiteral("%1 %2 %3ms")
                         .arg(name, status, age_ms >= 0 ? QString::number(age_ms) : QStringLiteral("--"));
    }
    return parts.isEmpty() ? QStringLiteral("No fresh cross-venue snapshot is available yet.")
                           : parts.join(QStringLiteral("  |  "));
}

inline QString crypto_cockpit_selected_venue_of(const QJsonObject& decision) {
    const int selected = decision.value(QStringLiteral("selected_proposal_index")).toInt(-1);
    const QJsonArray proposals = decision.value(QStringLiteral("venue_proposals")).toArray();
    if (selected >= 0 && selected < proposals.size()) {
        const QString venue = proposals.at(selected).toObject().value(QStringLiteral("venue")).toString();
        if (!venue.isEmpty()) return venue.toLower();
    }
    return decision.value(QStringLiteral("selected_venue"))
        .toString(decision.value(QStringLiteral("venue")).toString())
        .toLower();
}

inline QString crypto_cockpit_pct_text(double rate) {
    return QString::number(rate * 100.0, 'f', 1) + QStringLiteral("%");
}

inline void crypto_cockpit_proof_row_empty(CryptoCockpitProofRow* row, const QString& scope,
                                           const QString& note, const QString& role) {
    row->scope = scope;
    row->verdict = note;
    row->verdict_role = role;
    row->sample = QStringLiteral("--");
    row->mean_net = QStringLiteral("--");
    row->win_rate = QStringLiteral("--");
    row->coverage = QStringLiteral("--");
    row->has_metrics = false;
}

/// Fill aggregate + optional per-symbol proof from scalp_qualification_v1.json.
inline void crypto_cockpit_fill_proof(const QJsonObject& qualification,
                                      const QString& active_symbol,
                                      const QString& qualification_state,
                                      CryptoCockpitProofRow* proof_all,
                                      CryptoCockpitProofRow* proof_symbol,
                                      QString* proof_status) {
    const QString all_scope = QStringLiteral("ALL SYMBOLS");
    const QString symbol_scope =
        active_symbol.isEmpty() ? QStringLiteral("ACTIVE SYMBOL") : active_symbol;
    const QString version = qualification.value(QStringLiteral("report_version")).toString();
    if (qualification.isEmpty() || version != QLatin1String("crypto-scalp-qualification-v1")) {
        crypto_cockpit_proof_row_empty(proof_all, all_scope, QStringLiteral("NO DATA"),
                                       QStringLiteral("grey"));
        crypto_cockpit_proof_row_empty(proof_symbol, symbol_scope, QStringLiteral("NO DATA"),
                                       QStringLiteral("grey"));
        *proof_status = QStringLiteral(
            "SCALP SHADOW PROOF reads scalp_qualification_v1.json "
            "(crypto-scalp-qualification-v1 over scalp_decisions.jsonl). "
            "No report yet — not the edge crypto-recommend SQLite scoreboard.");
        return;
    }

    const int resolved = qualification.value(QStringLiteral("resolved_count")).toInt();
    const int candidates = qualification.value(QStringLiteral("candidate_count")).toInt();
    const int required = qualification.value(QStringLiteral("required_resolved")).toInt(200);
    const double coverage = qualification.value(QStringLiteral("coverage")).toDouble();
    const double mean_net = qualification.value(QStringLiteral("mean_net_bps")).toDouble();
    const double win_rate = qualification.value(QStringLiteral("win_rate")).toDouble();
    const QJsonArray ci = qualification.value(QStringLiteral("mean_net_ci95")).toArray();
    const double ci_low = ci.size() > 0 ? ci.at(0).toDouble() : 0.0;
    const double ci_high = ci.size() > 1 ? ci.at(1).toDouble() : 0.0;
    const QString raw_state = qualification.value(QStringLiteral("state")).toString();

    proof_all->scope = all_scope;
    proof_all->has_metrics = true;
    if (qualification_state == QLatin1String("QUALIFIED")) {
        proof_all->verdict = QStringLiteral("QUALIFIED");
        proof_all->verdict_role = QStringLiteral("green");
    } else if (resolved <= 0) {
        proof_all->verdict = QStringLiteral("AWAITING");
        proof_all->verdict_role = QStringLiteral("amber");
    } else {
        proof_all->verdict = raw_state.isEmpty() ? QStringLiteral("SHADOW") : raw_state;
        proof_all->verdict_role = QStringLiteral("red");
    }
    proof_all->sample = QStringLiteral("%1/%2 · need %3")
                            .arg(resolved)
                            .arg(candidates)
                            .arg(required);
    proof_all->mean_net = resolved > 0 ? crypto_cockpit_bps_text(mean_net) : QStringLiteral("--");
    proof_all->win_rate = resolved > 0 ? crypto_cockpit_pct_text(win_rate) : QStringLiteral("--");
    proof_all->coverage =
        resolved > 0
            ? QStringLiteral("cov %1 · CI [%2, %3]")
                  .arg(crypto_cockpit_pct_text(coverage), crypto_cockpit_bps_text(ci_low),
                       crypto_cockpit_bps_text(ci_high))
            : QStringLiteral("cov %1 · awaiting horizon").arg(crypto_cockpit_pct_text(coverage));

    // Per-symbol rollup from report.resolved[] when present.
    int sym_resolved = 0;
    int sym_wins = 0;
    double sym_net_sum = 0.0;
    const QString want = active_symbol.trimmed().toUpper();
    for (const auto& value : qualification.value(QStringLiteral("resolved")).toArray()) {
        const QJsonObject row = value.toObject();
        const QString sym = row.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
        if (want.isEmpty() || sym != want) continue;
        ++sym_resolved;
        sym_net_sum += row.value(QStringLiteral("net_bps")).toDouble();
        if (row.value(QStringLiteral("won")).toBool()) ++sym_wins;
    }

    if (want.isEmpty()) {
        crypto_cockpit_proof_row_empty(proof_symbol, symbol_scope,
                                       QStringLiteral("NO ACTIVE ENGINE SYMBOL"),
                                       QStringLiteral("grey"));
    } else if (sym_resolved <= 0) {
        crypto_cockpit_proof_row_empty(
            proof_symbol, symbol_scope,
            resolved <= 0 ? QStringLiteral("AWAITING") : QStringLiteral("NO SYMBOL RESOLVES"),
            QStringLiteral("amber"));
        proof_symbol->sample = QStringLiteral("0 resolved on %1").arg(want);
    } else {
        const double sym_mean = sym_net_sum / static_cast<double>(sym_resolved);
        const double sym_wr = static_cast<double>(sym_wins) / static_cast<double>(sym_resolved);
        proof_symbol->scope = want;
        proof_symbol->has_metrics = true;
        proof_symbol->verdict = sym_mean > 0.0 ? QStringLiteral("NET+") : QStringLiteral("NET-");
        proof_symbol->verdict_role = sym_mean > 0.0 ? QStringLiteral("green") : QStringLiteral("red");
        proof_symbol->sample =
            QStringLiteral("%1 resolved · of report %2").arg(sym_resolved).arg(resolved);
        proof_symbol->mean_net = crypto_cockpit_bps_text(sym_mean);
        proof_symbol->win_rate = crypto_cockpit_pct_text(sym_wr);
        proof_symbol->coverage = QStringLiteral("symbol slice of shadow report");
    }

    *proof_status = QStringLiteral(
        "SCALP SHADOW PROOF from crypto-scalp-qualification-v1 "
        "(scalp_decisions.jsonl + ticks). Same authority as QUALIFICATION. "
        "Not edge crypto-recommend SQLite. Read-only.");
}

inline CryptoCockpitTapeRow crypto_cockpit_tape_row_of(const QJsonObject& decision) {
    CryptoCockpitTapeRow row;
    row.symbol = decision.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
    row.verdict = decision.value(QStringLiteral("verdict")).toString(QStringLiteral("WAITING"));
    row.direction = decision.value(QStringLiteral("direction")).toString(QStringLiteral("--")).toUpper();
    row.net_known = decision.contains(QStringLiteral("net_after_cost_bps"));
    row.net_bps = decision.value(QStringLiteral("net_after_cost_bps")).toDouble();
    row.required_known = decision.contains(QStringLiteral("required_edge_bps"));
    row.required_bps = decision.value(QStringLiteral("required_edge_bps")).toDouble();
    row.liquidity = decision.value(QStringLiteral("liquidity")).toString().toLower();
    row.selected_venue = crypto_cockpit_selected_venue_of(decision);
    for (const auto& value : decision.value(QStringLiteral("blockers")).toArray())
        row.blockers << value.toString();
    return row;
}

inline CryptoCockpitScene present_crypto_cockpit(const CryptoCockpitInputs& inputs) {
    CryptoCockpitScene scene;
    const qint64 now_ms =
        inputs.now_ms > 0 ? inputs.now_ms : QDateTime::currentMSecsSinceEpoch();

    const QJsonObject config =
        inputs.scalp_state.value(QStringLiteral("config"))
            .toObject(inputs.scalp_engine);
    const QString style =
        config.value(QStringLiteral("style"))
            .toString(inputs.scalp_engine.value(QStringLiteral("style")).toString(QStringLiteral("scalp")))
            .toUpper();
    scene.style = (style == QLatin1String("SPOT")) ? QStringLiteral("SPOT") : QStringLiteral("SCALP");

    scene.engine_running =
        inputs.scalp_state.value(QStringLiteral("status")).toString() == QLatin1String("running") &&
        config.value(QStringLiteral("enabled")).toBool();
    scene.engine_line =
        scene.engine_running ? scene.style + QStringLiteral(" · RUNNING")
                             : scene.style + QStringLiteral(" · OFFLINE");
    scene.engine_role = scene.engine_running ? QStringLiteral("green") : QStringLiteral("red");

    scene.venue_line = crypto_cockpit_venue_title(inputs.exchange_id) +
                       (inputs.is_paper ? QStringLiteral(" / PAPER") : QStringLiteral(" / LIVE"));
    scene.venue_role = inputs.is_paper ? QStringLiteral("cyan") : QStringLiteral("green");

    scene.heartbeat = crypto_cockpit_heartbeat_text(
        inputs.scalp_state.value(QStringLiteral("heartbeat_at")).toString(), now_ms);
    scene.heartbeat_role = scene.engine_running ? QStringLiteral("green") : QStringLiteral("red");

    const QDateTime expiry = QDateTime::fromString(
        inputs.live_guard.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs);
    scene.canary_expires_at = inputs.live_guard.value(QStringLiteral("expires_at")).toString();
    scene.canary_on = inputs.live_guard.value(QStringLiteral("enabled")).toBool() &&
                      (!expiry.isValid() || expiry.toMSecsSinceEpoch() >= now_ms);

    scene.blockers = crypto_cockpit_security_blockers(inputs.security, inputs.exchange_id);
    if (scene.canary_on) {
        scene.mood = QStringLiteral("CANARY ON");
        if (!scene.blockers.isEmpty()) {
            scene.mood_role = QStringLiteral("amber");
            scene.mood_detail =
                QStringLiteral("armed but blocked · %1").arg(scene.blockers.join(QStringLiteral("; ")));
        } else {
            scene.mood_role = QStringLiteral("amber");
            scene.mood_detail =
                scene.canary_expires_at.isEmpty()
                    ? QStringLiteral("expires unknown · arm from Profile · not Kalshi live")
                    : QStringLiteral("expires %1 · arm from Profile · not Kalshi live")
                          .arg(scene.canary_expires_at);
        }
    } else {
        scene.mood = QStringLiteral("PAPER SCALP");
        scene.mood_role = QStringLiteral("cyan");
        scene.mood_detail = scene.blockers.isEmpty()
                                ? QStringLiteral("canary off · paper/shadow path · arm from Profile")
                                : QStringLiteral("canary off · %1").arg(scene.blockers.join(QStringLiteral("; ")));
    }

    const QJsonArray decisions = inputs.scalp_state.value(QStringLiteral("decisions")).toArray();
    const QJsonObject latest = decisions.isEmpty() ? QJsonObject{} : decisions.first().toObject();
    const CryptoCockpitTapeRow head = crypto_cockpit_tape_row_of(latest);
    scene.decide_symbol = head.symbol;
    scene.decide_verdict = head.verdict;
    scene.decide_direction = head.direction;
    scene.decide_liquidity =
        head.liquidity.isEmpty() ? QStringLiteral("--") : head.liquidity.toUpper();
    scene.decide_is_candidate = head.verdict.contains(QLatin1String("CANDIDATE"));
    scene.decide_role = scene.decide_is_candidate ? QStringLiteral("green") : QStringLiteral("amber");
    const double reference = latest.value(QStringLiteral("reference_price")).toDouble();
    scene.decide_reference =
        reference > 0 ? QStringLiteral("$%1").arg(QString::number(reference, 'f', 2))
                      : QStringLiteral("--");
    scene.decide_required =
        head.required_known ? crypto_cockpit_bps_text(head.required_bps) : QStringLiteral("--");
    scene.decide_net =
        head.net_known ? crypto_cockpit_bps_text(head.net_bps) : QStringLiteral("--");
    scene.decide_blockers =
        head.blockers.isEmpty() ? QStringLiteral("No current engine blockers.")
                                : head.blockers.join(QStringLiteral("  |  "));
    scene.hurdle_line = scene.decide_required;

    const int tape_n =
        std::min(kCryptoCockpitMaxTapeRows, static_cast<int>(decisions.size()));
    for (int i = 0; i < tape_n; ++i)
        scene.tape << crypto_cockpit_tape_row_of(decisions.at(i).toObject());
    if (scene.tape.isEmpty()) {
        scene.tape_census = QStringLiteral("NO TAPE · waiting for daemon decisions");
    } else if (decisions.size() > scene.tape.size()) {
        scene.tape_census = QStringLiteral("%1 of %2 decisions · newest first · L→R read")
                                .arg(scene.tape.size())
                                .arg(decisions.size());
    } else {
        scene.tape_census =
            QStringLiteral("%1 decisions · newest first · L→R read").arg(scene.tape.size());
    }

    scene.feeds_line = crypto_cockpit_feeds_line(latest);

    crypto_cockpit_qualification_of(inputs.qualification, inputs.qualification_age_ms,
                                    &scene.qualification_state, &scene.qualification_role,
                                    &scene.qualification_detail);
    crypto_cockpit_fill_proof(inputs.qualification, scene.decide_symbol, scene.qualification_state,
                              &scene.proof_all, &scene.proof_symbol, &scene.proof_status);
    return scene;
}

} // namespace openmarketterminal::screens::crypto
