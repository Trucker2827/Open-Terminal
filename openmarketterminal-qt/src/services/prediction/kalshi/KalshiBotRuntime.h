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
// Issue #155 moved the MODE word in here for the same reason. The chip derived
// it from the newest readable tick and failed closed to UNKNOWN; the CLI's copy
// was the literal `"paper"`, so a live tick read LIVE in the window and paper in
// the shell — the worst direction for a second truth to point. There is now one
// `kalshi_bot_mode()`, and both surfaces render its answer.
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
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include <functional>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// The bot's own evidence files, named once. Consumers resolve the directory
/// through `cli::kalshi_evidence_path()`; only the names live here so a file
/// rename cannot leave one reader looking at the old path.
inline constexpr auto kKalshiBotDecisionLedgerFile = "kalshi-bot-decisions.jsonl";
inline constexpr auto kKalshiBotStopFileName = "kalshi-bot-stop.json";

// --- the record and its generations -----------------------------------------
//
// The decision ledger is not a rolling window over recent activity: it is the
// sealed promotion gate's entire evidence, and it is the book's only memory of
// what is resting, what is filled, and which contracts have already resolved.
// So it rotates by NAMING a new generation and never by discarding one (issue
// #152): `kalshi-bot-decisions.jsonl.1` is the OLDEST chunk, then `.2`, …, and
// the base file is the newest, still being appended to.
//
// The writer (KalshiEvidenceEngine::append_jsonl) and every reader share the
// four functions below, so a generation the writer creates cannot be one a
// reader does not look at — the failure mode this issue is about was exactly
// that split.

/// The base path for generation 0 (the live file), `base.N` otherwise.
inline QString kalshi_bot_generation_path(const QString& base_path, int generation) {
    return generation <= 0 ? base_path : base_path + QStringLiteral(".%1").arg(generation);
}

/// The generation index `file_name` encodes relative to `base_name`, or -1 when
/// it is not a generation of it. Only a bare positive integer suffix counts:
/// `.jsonl.1` is generation 1, while `.jsonl.1.gz`, `.jsonl.old` and
/// `.jsonl.01` are somebody else's files and are never read as the record.
inline int kalshi_bot_generation_index(const QString& base_name, const QString& file_name) {
    if (!file_name.startsWith(base_name + QLatin1Char('.'))) return -1;
    const QString suffix = file_name.mid(base_name.size() + 1);
    bool ok = false;
    const int index = suffix.toInt(&ok);
    if (!ok || index < 1 || suffix != QString::number(index)) return -1;
    return index;
}

/// The record as it is on disk right now.
struct KalshiBotLedgerRecord {
    /// The files that exist, in CHRONOLOGICAL order: `.1`, `.2`, …, then the
    /// base file last. `KalshiBotOrders::replay()` is order-dependent (a fill
    /// only lands on an order it has already seen opened), so this order is a
    /// contract, not a convenience.
    QStringList paths;
    /// Generations missing from the 1..newest run. Generations are only ever
    /// created in order, so a gap is not a naming quirk — it is a chunk of the
    /// record that something deleted, and the gate refuses to score it.
    QStringList missing;
    /// The highest generation index on disk; 0 when the ledger has never
    /// rotated.
    int newest_generation = 0;
};

inline KalshiBotLedgerRecord kalshi_bot_ledger_record(const QString& base_path) {
    KalshiBotLedgerRecord record;
    const QFileInfo base_info(base_path);
    const QString base_name = base_info.fileName();
    QSet<int> present;
    const QDir dir(base_info.absolutePath());
    const auto entries =
        dir.entryList(QStringList{base_name + QStringLiteral(".*")}, QDir::Files);
    for (const QString& name : entries) {
        const int index = kalshi_bot_generation_index(base_name, name);
        if (index < 0) continue;
        present.insert(index);
        record.newest_generation = qMax(record.newest_generation, index);
    }
    for (int i = 1; i <= record.newest_generation; ++i)
        (present.contains(i) ? record.paths : record.missing)
            .append(kalshi_bot_generation_path(base_path, i));
    if (QFileInfo::exists(base_path)) record.paths.append(base_path);
    return record;
}

/// Where the next rotation must move the live file: one past the NEWEST
/// generation that exists, never the first free slot. Backfilling a gap would
/// both put a newer chunk under an older name (breaking replay order) and heal
/// the hole that tells the gate the record is incomplete.
inline QString kalshi_bot_next_generation_path(const QString& base_path) {
    return kalshi_bot_generation_path(base_path,
                                      kalshi_bot_ledger_record(base_path).newest_generation + 1);
}

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

/// How old a published gate verdict may be before the surfaces render it as
/// stale. Two tick intervals, the same line the status chip and the funnel go
/// stale on: since issue #167 the paper loop re-evaluates the gate on EVERY
/// tick, so a verdict older than two ticks means the evaluation is not
/// happening, exactly as a ledger older than two ticks means the loop is not
/// looping.
///
/// Deliberately NOT the same threshold as KalshiBotLive::kMaxGateAgeMs (one
/// hour), and neither should be "fixed" to match the other: this one asks "is
/// the screen showing a current verdict?", that one asks "is this verdict
/// recent enough to admit real money?". They answer different questions and a
/// display threshold has no business gating live admission.
inline constexpr qint64 kKalshiBotGateStaleMs = kKalshiBotStaleMs;

/// The age of a published verdict, as BOTH surfaces render it next to the
/// verdict word. One formatter, so the BOT panel and the cockpit can never
/// disagree about whether what they are showing is current.
///
/// `stale` is true for every reason the age cannot be vouched for — too old, a
/// timestamp in the future, or no timestamp at all — because each of those is a
/// verdict that must not render as silently current (issue #167).
struct KalshiBotGateFreshness {
    bool dated = false;    ///< the verdict carries a usable, non-future ts_ms
    qint64 age_ms = -1;    ///< -1 when there is no age to state
    bool stale = true;     ///< an undated verdict is stale, never current
    QString text;          ///< "evaluated 12s ago" / "… · STALE" / the refusal
};

/// `gate` is kalshi-bot-gate.json as published (an empty object means no file,
/// which has no age at all and yields an empty reading).
inline KalshiBotGateFreshness kalshi_bot_gate_freshness(
    const QJsonObject& gate, qint64 now_ms, qint64 stale_after_ms = kKalshiBotGateStaleMs) {
    KalshiBotGateFreshness freshness;
    if (gate.isEmpty()) return freshness;  // no file: the caller says NOT EVALUATED
    const QJsonValue ts = gate.value(QStringLiteral("ts_ms"));
    if (!ts.isDouble()) {
        // A verdict that does not say when it was evaluated is not treated as
        // fresh: the whole point of this reading is that a displayed verdict
        // carries its age, and "no age" is not "no problem".
        freshness.text = QStringLiteral("evaluated at an UNKNOWN time · the verdict carries no "
                                        "ts_ms, so its age cannot be stated");
        return freshness;
    }
    const auto published_ms = static_cast<qint64>(ts.toDouble());
    if (published_ms > now_ms) {
        // Mistrusted rather than clamped to zero, exactly as the status chip
        // mistrusts a future heartbeat.
        freshness.text = QStringLiteral("evaluated at a timestamp in the FUTURE, so its age is not "
                                        "trusted");
        return freshness;
    }
    freshness.dated = true;
    freshness.age_ms = now_ms - published_ms;
    freshness.stale = freshness.age_ms > stale_after_ms;
    freshness.text = QStringLiteral("evaluated %1").arg(kalshi_bot_age_text(freshness.age_ms));
    if (freshness.stale)
        freshness.text += QStringLiteral(" · STALE (the loop re-evaluates every tick; this verdict "
                                         "is older than %1s)")
                              .arg(stale_after_ms / 1000);
    return freshness;
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
/// signal trust, the latest decisions) and the record grows without bound, so a
/// fixed window is read rather than every generation.
///
/// The window spans generations backwards from the newest (issue #152): the
/// tick right after a rotation leaves a base file holding one row, and a reader
/// that only ever looked at the base file would report a running bot as `off`
/// for the rest of the window it could not see.
inline QJsonArray kalshi_bot_read_ledger_tail(const QString& path,
                                              qint64 window_bytes = 512LL * 1024) {
    const KalshiBotLedgerRecord record = kalshi_bot_ledger_record(path);
    QList<QByteArray> chunks;  // oldest first, so the rows come out in order
    qint64 remaining = window_bytes;
    for (qsizetype i = record.paths.size() - 1; i >= 0 && remaining > 0; --i) {
        QFile file(record.paths.at(i));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text) || file.size() <= 0) continue;
        // Only the OLDEST file the window reaches into is seeked into mid-file;
        // every newer generation is read whole from byte 0. Dropping a first
        // line per file would silently eat a real row from each of them.
        const bool truncated = file.size() > remaining;
        if (truncated) file.seek(file.size() - remaining);
        remaining -= file.size();
        QByteArray bytes = file.readAll();
        if (truncated) {
            // A window that starts mid-file starts mid-line; that first
            // fragment is dropped rather than parsed as a partial row.
            const qsizetype newline = bytes.indexOf('\n');
            bytes = newline < 0 ? QByteArray() : bytes.mid(newline + 1);
        }
        chunks.prepend(bytes);
    }
    QJsonArray rows;
    for (const QByteArray& chunk : chunks) {
        for (const QByteArray& line : chunk.split('\n')) {
            if (line.trimmed().isEmpty()) continue;
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (document.isObject()) rows.append(document.object());
        }
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

/// The three ledger event kinds a paper tick's BOOK REPLAY must see: an open
/// decision, a real settlement, and a paper void (the orphan-retirement rung
/// that voids a position with no settlement record left in the retained
/// window). One predicate for both of `run_tick()`'s disk reads (the stopped
/// path and the main path) so they cannot drift apart the way they did before:
/// the settlement kind was in both from the start, but the void kind was added
/// later and only reached the in-memory replay call sites, never this shared
/// reader — so a void journaled on one tick was invisible to the very next
/// tick's fresh read from disk, the position it retired reappeared in
/// `book.positions`, and `settle_paper()` voided it again, forever, on an
/// append-only ledger.
///
/// The sealed promotion gate's own reads are deliberately NOT built on this:
/// the gate counts settlements exclusively (`kalshi bot gate`'s promotion
/// tally) and must keep excluding voids from it.
inline bool kalshi_bot_is_replay_event(const QJsonObject& row) {
    const QString event = row.value(QStringLiteral("event")).toString();
    return event == QLatin1String("kalshi_bot_decision") ||
           event == QLatin1String("kalshi_bot_paper_settlement") ||
           event == QLatin1String("kalshi_bot_paper_void");
}

/// EVERY row of the record, oldest generation first — the one whole-record
/// reader (issue #152). The tick's replay, the stopped tick's book and the gate
/// all go through this, so none of them can be looking at a different record
/// than the others.
///
/// `keep` filters rows as they are parsed, so a record of any size is never
/// held in memory whole; an empty `keep` accepts everything.
inline QJsonArray kalshi_bot_read_ledger(
    const QString& base_path, const std::function<bool(const QJsonObject&)>& keep = {}) {
    QJsonArray rows;
    for (const QString& path : kalshi_bot_ledger_record(base_path).paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!file.atEnd()) {
            const QJsonDocument document = QJsonDocument::fromJson(file.readLine());
            if (!document.isObject()) continue;
            const QJsonObject row = document.object();
            if (!keep || keep(row)) rows.append(row);
        }
    }
    return rows;
}

/// The oldest dated row in the whole record, or 0 when it carries none. Rows
/// are appended in time order, so this is the first dated row of the oldest
/// generation that has one — read without loading the record.
///
/// This is the anchor the gate compares against what it has already published:
/// a record whose oldest row postdates a settlement the gate has already scored
/// is a record something has truncated.
inline qint64 kalshi_bot_oldest_row_ts_ms(const QString& base_path) {
    for (const QString& path : kalshi_bot_ledger_record(base_path).paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!file.atEnd()) {
            const QJsonDocument document = QJsonDocument::fromJson(file.readLine());
            if (!document.isObject()) continue;
            const qint64 ts = kalshi_bot_row_ts_ms(document.object());
            if (ts > 0) return ts;
        }
    }
    return 0;
}

// --- the mode word both surfaces render (issue #155) -------------------------

/// The ledger events this build understands. A row carrying any other `event`
/// was written by a bot binary neither surface knows — see `kalshi_bot_mode`.
inline constexpr auto kKalshiBotDecisionEvent = "kalshi_bot_decision";
inline constexpr auto kKalshiBotSettlementEvent = "kalshi_bot_paper_settlement";

/// Where a row sits in the record: by timestamp first, by position only to break
/// a tie. Two appenders (the launchd loop and a hand-run `kalshi bot once`)
/// interleave rows, so position alone is not "newest" (issue #145).
///
/// `valid()` is "this stamp came off a row", NOT "that row was dated": an
/// undated row still states a mode, and dropping it would let a mode nothing
/// claimed win by default.
struct KalshiBotRowStamp {
    qint64 ts_ms = 0;
    int index = -1;
    bool valid() const { return index >= 0; }
};

inline bool kalshi_bot_row_newer(const KalshiBotRowStamp& a, const KalshiBotRowStamp& b) {
    if (!a.valid()) return false;
    if (!b.valid()) return true;
    return a.ts_ms != b.ts_ms ? a.ts_ms > b.ts_ms : a.index > b.index;
}

/// The mode word a row states about itself, or an empty string when it states
/// none this build can read. `paper` and `live` are the only readable answers:
/// a row whose `mode` is missing or unrecognised is a tick of unknown vintage,
/// not a paper one.
inline QString kalshi_bot_stated_mode(const QJsonObject& row) {
    const QString mode = row.value(QStringLiteral("mode")).toString();
    return mode == QStringLiteral("live") || mode == QStringLiteral("paper") ? mode : QString();
}

/// The hint the operator acts on when the record's newest row is of a vintage
/// this build cannot read: the loop is running a binary older (or newer) than
/// the reader, which is fixed by restarting the launchd job — the exact command
/// the job's own plist documents.
inline QString kalshi_bot_vintage_hint() {
    return QStringLiteral(
        "bot binary older than GUI — launchctl kickstart -k gui/$UID/org.openterminal.kalshi-bot");
}

/// What the bot's most recent TICK was doing, as the ledger states it.
///
/// `mode` is `live`, `paper`, `unknown`, or EMPTY when no row claims anything —
/// the same absent-is-absent rule `last_decision_age_ms` follows. Renderers
/// print `badge()` (`LIVE` / `PAPER` / `UNKNOWN`).
struct KalshiBotMode {
    QString mode;
    bool live = false;     ///< the newest tick explicitly said `mode=live`
    bool unknown = false;  ///< the newest row is one this build cannot read
    bool stated() const { return !mode.isEmpty(); }
    QString badge() const { return mode.toUpper(); }
};

/// The mode classifier, failing CLOSED (issue #145, moved here by #155).
///
/// Read off the NEWEST tick: the answer is what the bot is doing NOW, not what
/// it has ever done. A bot that ran live an hour ago and papers now is papering,
/// and a reader that latched on any live row in the window would keep claiming
/// otherwise.
///
/// LIVE is claimed only when that newest tick explicitly says `mode=live`. When
/// the newest row is one this build cannot read (an unknown `event`, or a
/// decision row whose `mode` is absent or unrecognised) the honest answer is
/// UNKNOWN: reading the mode off the last row that happened to parse is how a
/// surface ends up announcing LIVE over a record that never went live, and
/// failing open to LIVE is the worst direction to fail in. A paper settlement is
/// the same vintage and is not a tick, so it makes no claim either way. With
/// nothing to read there is nothing to claim, and `mode` stays empty.
inline KalshiBotMode kalshi_bot_mode(const QJsonArray& rows) {
    KalshiBotRowStamp newest_readable_tick;
    KalshiBotRowStamp newest_unreadable_row;
    QString newest_readable_mode;
    for (int index = 0; index < rows.size(); ++index) {
        const QJsonObject row = rows.at(index).toObject();
        const QString event = row.value(QStringLiteral("event")).toString();
        KalshiBotRowStamp stamp;
        stamp.ts_ms = kalshi_bot_row_ts_ms(row);
        stamp.index = index;
        if (event == QLatin1String(kKalshiBotDecisionEvent)) {
            const QString stated = kalshi_bot_stated_mode(row);
            if (stated.isEmpty()) {
                if (kalshi_bot_row_newer(stamp, newest_unreadable_row))
                    newest_unreadable_row = stamp;
            } else if (kalshi_bot_row_newer(stamp, newest_readable_tick)) {
                newest_readable_tick = stamp;
                newest_readable_mode = stated;
            }
        } else if (event != QLatin1String(kKalshiBotSettlementEvent)) {
            if (kalshi_bot_row_newer(stamp, newest_unreadable_row)) newest_unreadable_row = stamp;
        }
    }

    KalshiBotMode mode;
    if (newest_readable_tick.valid() &&
        !kalshi_bot_row_newer(newest_unreadable_row, newest_readable_tick)) {
        mode.live = newest_readable_mode == QStringLiteral("live");
        mode.mode = newest_readable_mode;
    } else if (newest_unreadable_row.valid()) {
        mode.unknown = true;
        mode.mode = QStringLiteral("unknown");
    }
    return mode;
}

/// The status line both surfaces print: `[MODE] <status>`, and — when the mode
/// is UNKNOWN — why nothing is claimed plus the launchctl hint. One sentence,
/// so the window and `kalshi bot status` cannot word it differently.
inline QString kalshi_bot_mode_headline(const QString& status_text, const KalshiBotMode& mode) {
    if (!mode.stated()) return status_text;
    QString line = QStringLiteral("[%1] %2").arg(mode.badge(), status_text);
    if (mode.unknown)
        line += QStringLiteral(" · the newest ledger row carries no mode this build can read, so "
                               "no mode is claimed · %1").arg(kalshi_bot_vintage_hint());
    return line;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
