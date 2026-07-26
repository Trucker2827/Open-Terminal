#pragma once

// Observable-loop runtime for the PAPER Kalshi bot (ladder rung 4): the kill
// switch and the one loop-status classifier that the GUI BOT chip and
// `kalshi bot status` both render.
//
// Why this header exists at all: the chip and the CLI must never be able to
// disagree. Rung 3 gave the panel its own freshness constant; the moment a
// second reader (the CLI, a launchd job) grew its own, "the GUI shows exactly
// what the bot is doing" would be a claim rather than a property. So the
// threshold, the state names, the colour role, the headline sentence, the
// ledger tail reader and the stop-file format all live here once, and both
// sides are thin renderers over `kalshi_bot_loop_status()`.
//
// Three honesty rules are structural here too:
//   1. **The kill switch fails CLOSED.** The stop file's EXISTENCE is the
//      switch. Its contents are audit metadata (who, when, why) and are never
//      consulted to decide whether the switch is engaged, so a truncated,
//      empty, or unparseable file still stops the bot.
//   2. **STOPPED outranks fresh.** A killed bot writes one last refusal row,
//      which would otherwise read as a fresh (green) ledger for two more
//      minutes and then as merely stale. Stopped is classified first and
//      rendered as its own state, because "running" would be a lie.
//   3. **An absent ledger is absent.** No ledger means "the bot has never run
//      here", never a zero-age heartbeat.
//
// Header-inline (no .cpp) for the same reason as `kalshi_evidence_path` and
// the daemon chip's classifier in cli/ServeCommand.h: the GUI does not link
// the CLI's translation units, and both need this.

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>
#include <QTimeZone>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// The bot's own evidence files, named once. Consumers resolve the directory
/// through `cli::kalshi_evidence_path()`; only the names live here so a file
/// rename cannot leave one reader looking at the old path.
inline constexpr auto kKalshiBotDecisionLedgerFile = "kalshi-bot-decisions.jsonl";
inline constexpr auto kKalshiBotStopFileName = "kalshi-bot-stop.json";

/// `kalshi bot run` and the launchd job (org.openterminal.kalshi-bot) tick on
/// this interval. Two missed ticks is the staleness line: one skipped tick can
/// be a slow calibrator read, two is a loop that is not looping.
inline constexpr int kKalshiBotIntervalSeconds = 60;
inline constexpr qint64 kKalshiBotStaleMs =
    2LL * static_cast<qint64>(kKalshiBotIntervalSeconds) * 1000;

/// The kill switch, as read from disk. `engaged` is exactly "the file is
/// there" — see honesty rule 1. Everything else is what the writer said about
/// itself, and is absent (empty / -1) when it did not say.
struct KalshiBotStopFile {
    bool engaged = false;
    qint64 ts_ms = -1;
    QString source;   ///< "cli" or "gui" — who threw the switch
    QString reason;   ///< free text the operator supplied, if any
};

/// The loop as both surfaces report it.
///
/// `state` is one of "stopped", "running", "stale", "off"; `age_ms` is the age
/// of the newest ledger row, or -1 when there is no row to age.
struct KalshiBotLoopStatus {
    QString state = QStringLiteral("off");
    qint64 age_ms = -1;
    QString headline;
    KalshiBotStopFile stop;
};

/// Colour intent for `state`, so the mapping itself is testable: the widget
/// only turns these roles into theme colours, and the CLI prints the role name.
inline QString kalshi_bot_state_color_role(const QString& state) {
    if (state == QStringLiteral("running")) return QStringLiteral("green");
    if (state == QStringLiteral("stale")) return QStringLiteral("amber");
    if (state == QStringLiteral("stopped")) return QStringLiteral("red");
    return QStringLiteral("grey");
}

inline QString kalshi_bot_age_text(qint64 age_ms) {
    if (age_ms < 120'000) return QStringLiteral("%1s ago").arg(age_ms / 1'000);
    if (age_ms < 7'200'000) return QStringLiteral("%1m ago").arg(age_ms / 60'000);
    return QStringLiteral("%1h ago").arg(age_ms / 3'600'000);
}

/// The one classifier. `newest_ledger_ms` is the newest `ts_ms` in
/// kalshi-bot-decisions.jsonl (0 when the ledger is absent or has no rows).
inline KalshiBotLoopStatus kalshi_bot_loop_status(qint64 newest_ledger_ms,
                                                  const KalshiBotStopFile& stop,
                                                  qint64 now_ms,
                                                  qint64 stale_after_ms = kKalshiBotStaleMs) {
    KalshiBotLoopStatus status;
    status.stop = stop;
    if (newest_ledger_ms > 0 && now_ms >= newest_ledger_ms) status.age_ms = now_ms - newest_ledger_ms;

    // Honesty rule 2: a stopped bot is never reported as running, whatever the
    // ledger's age says — the last thing it wrote was its own refusal.
    if (stop.engaged) {
        status.state = QStringLiteral("stopped");
        QString detail;
        if (stop.ts_ms > 0 && now_ms >= stop.ts_ms)
            detail += QStringLiteral(" · thrown %1").arg(kalshi_bot_age_text(now_ms - stop.ts_ms));
        if (!stop.source.isEmpty()) detail += QStringLiteral(" by %1").arg(stop.source);
        if (!stop.reason.isEmpty()) detail += QStringLiteral(" · \"%1\"").arg(stop.reason);
        status.headline =
            QStringLiteral("BOT STOPPED · kill switch engaged%1 · no bid is placed until it is "
                           "cleared (`kalshi bot resume`)").arg(detail);
        return status;
    }

    if (newest_ledger_ms <= 0) {
        status.state = QStringLiteral("off");
        status.headline = QStringLiteral("BOT OFF · no %1 — `kalshi bot once` has never run here")
                              .arg(QString::fromLatin1(kKalshiBotDecisionLedgerFile));
        return status;
    }
    // A timestamp in the future is mistrusted rather than clamped, exactly as
    // the daemon chip mistrusts a future heartbeat.
    if (newest_ledger_ms > now_ms) {
        status.state = QStringLiteral("stale");
        status.headline = QStringLiteral("BOT STALE · last decision timestamped in the future");
        return status;
    }
    if (status.age_ms > stale_after_ms) {
        status.state = QStringLiteral("stale");
        status.headline =
            QStringLiteral("BOT STALE · last decision %1").arg(kalshi_bot_age_text(status.age_ms));
        return status;
    }
    status.state = QStringLiteral("running");
    status.headline =
        QStringLiteral("BOT RUNNING · last decision %1").arg(kalshi_bot_age_text(status.age_ms));
    return status;
}

/// Parses a stop file that is known to exist. Contents never un-engage the
/// switch (honesty rule 1) — junk simply carries no metadata.
inline KalshiBotStopFile kalshi_bot_parse_stop_file(const QByteArray& bytes) {
    KalshiBotStopFile stop;
    stop.engaged = true;
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    const QJsonValue ts = root.value(QStringLiteral("ts_ms"));
    if (ts.isDouble()) stop.ts_ms = static_cast<qint64>(ts.toDouble());
    stop.source = root.value(QStringLiteral("source")).toString();
    stop.reason = root.value(QStringLiteral("reason")).toString();
    return stop;
}

inline KalshiBotStopFile kalshi_bot_read_stop_file(const QString& path) {
    QFile file(path);
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly)) return kalshi_bot_parse_stop_file({});
    return kalshi_bot_parse_stop_file(file.readAll());
}

inline QJsonObject kalshi_bot_stop_payload(const QString& source, const QString& reason,
                                           qint64 now_ms) {
    QJsonObject payload{
        {QStringLiteral("stopped"), true},
        {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
        {QStringLiteral("ts"),
         QDateTime::fromMSecsSinceEpoch(now_ms, QTimeZone::UTC).toString(Qt::ISODateWithMs)},
        {QStringLiteral("source"), source},
        {QStringLiteral("note"),
         QStringLiteral("The existence of this file is the kill switch; its contents are audit "
                        "metadata only. Remove it (or run `kalshi bot resume`) to re-enable.")}};
    if (!reason.trimmed().isEmpty()) payload.insert(QStringLiteral("reason"), reason.trimmed());
    return payload;
}

/// Writes the stop file atomically. The GUI and the CLI both call this, so the
/// switch one throws is byte-identical to the switch the other throws.
inline bool kalshi_bot_write_stop_file(const QString& path, const QJsonObject& payload) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    return file.commit();
}

/// Clears the switch. Returns true when the file is gone afterwards, whether
/// or not this call is what removed it.
inline bool kalshi_bot_clear_stop_file(const QString& path) {
    QFile file(path);
    if (!file.exists()) return true;
    return file.remove();
}

/// The tail of the decision ledger. Only the recent end is needed (freshness,
/// signal trust, the latest decisions) and the file grows without bound, so a
/// fixed window is read rather than the whole ledger.
inline QJsonArray kalshi_bot_read_ledger_tail(const QString& path,
                                              qint64 window_bytes = 512LL * 1024) {
    QJsonArray rows;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text) || file.size() <= 0) return rows;
    const bool truncated = file.size() > window_bytes;
    if (truncated) file.seek(file.size() - window_bytes);
    QList<QByteArray> lines = file.readAll().split('\n');
    // A window that starts mid-file starts mid-line; that first fragment is
    // dropped rather than parsed as a partial row.
    if (truncated && !lines.isEmpty()) lines.removeFirst();
    for (const QByteArray& line : lines) {
        if (line.trimmed().isEmpty()) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject()) rows.append(document.object());
    }
    return rows;
}

/// When a ledger row was written, whatever vintage of the bot wrote it: `ts_ms`
/// when the row carries one, otherwise the ISO `ts` every row this repo has
/// ever written carries beside it. 0 when the row is undated.
///
/// Freshness must never depend on recognising a row's shape (issue #145): a
/// timestamped row proves the loop ticked even if this build understands
/// nothing else in it, and a reader that skipped such rows would freeze the age
/// at the last row it happened to understand — reporting a running bot as
/// hours stale.
inline qint64 kalshi_bot_row_ts_ms(const QJsonObject& row) {
    const QJsonValue ms = row.value(QStringLiteral("ts_ms"));
    if (ms.isDouble()) return static_cast<qint64>(ms.toDouble());
    const QDateTime iso =
        QDateTime::fromString(row.value(QStringLiteral("ts")).toString(), Qt::ISODateWithMs);
    return iso.isValid() ? iso.toMSecsSinceEpoch() : 0;
}

/// Newest timestamp across ledger rows (decisions, paper settlements, and rows
/// of any other vintage alike): the loop's heartbeat. 0 when no row is dated.
inline qint64 kalshi_bot_newest_ts_ms(const QJsonArray& rows) {
    qint64 newest = 0;
    for (const auto& value : rows) newest = qMax(newest, kalshi_bot_row_ts_ms(value.toObject()));
    return newest;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
