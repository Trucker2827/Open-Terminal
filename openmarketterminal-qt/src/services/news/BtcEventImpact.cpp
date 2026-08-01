#include "services/news/BtcEventImpact.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>

namespace openmarketterminal::services::news {

namespace {

struct Capture {
    bool ok = false;
    QString stdout_text;
    QString stderr_text;
    QString error;
};

// Start `program args`, write `stdin_bytes`, close the write channel, and wait
// up to `timeout_ms` for the process to finish.  Mirrors the CLI's run_capture
// but feeds stdin -- the scorer reads its payload from stdin.
Capture run_capture_stdin(const QString& program, const QStringList& args,
                          const QByteArray& stdin_bytes, int timeout_ms) {
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.start();
    Capture out;
    if (!p.waitForStarted(2000)) {
        out.error = p.errorString();
        return out;
    }
    p.write(stdin_bytes);
    p.closeWriteChannel();
    if (!p.waitForFinished(timeout_ms)) {
        p.kill();
        p.waitForFinished(1000);
        out.error = QStringLiteral("timeout");
        return out;
    }
    out.stdout_text = QString::fromUtf8(p.readAllStandardOutput());
    out.stderr_text = QString::fromUtf8(p.readAllStandardError());
    out.ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (!out.ok && out.error.isEmpty())
        out.error = out.stderr_text.trimmed().isEmpty()
                        ? QStringLiteral("exit %1").arg(p.exitCode())
                        : out.stderr_text.trimmed();
    return out;
}

QJsonObject read_object(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

// Byte-for-byte identical to the CLI's bitcoin_evidence_write_snapshot so the
// Task-2 calibrator consumer reads exactly what it expects: `-latest.json`
// written atomically (Indented), and one Compact line appended to `.jsonl`.
bool write_snapshot(const QString& data_dir, const QString& stem, const QJsonObject& row) {
    QDir().mkpath(data_dir);
    QSaveFile latest(data_dir + QLatin1Char('/') + stem + QStringLiteral("-latest.json"));
    if (!latest.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    latest.write(QJsonDocument(row).toJson(QJsonDocument::Indented));
    if (!latest.commit()) return false;
    QFile history(data_dir + QLatin1Char('/') + stem + QStringLiteral(".jsonl"));
    if (!history.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return false;
    history.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
    history.write("\n");
    return true;
}

} // namespace

BtcEventImpactResult run_btc_event_impact(const QString& data_dir,
                                          const QString& python,
                                          const QString& default_scorer_script,
                                          int limit, qint64 as_of_ms) {
    BtcEventImpactResult result;

    const QJsonObject pulse =
        read_object(data_dir + QStringLiteral("/btc-news-pulse-latest.json"));
    if (pulse.isEmpty()) {
        result.error = QStringLiteral("no Bitcoin pulse available; run `news bitcoin-pulse --force` first");
        return result;
    }

    // Build the blind stdin payload: only stories with a publish time in
    // (0, as_of_ms].  The pulse serializes `published_ts` as a STRING of epoch
    // milliseconds (BtcNewsPulse::to_json), so parse via QVariant and keep the
    // seconds->ms guard as a safety net for any alternate source unit.
    QJsonArray stories;
    int taken = 0;
    for (const auto& value : pulse.value(QStringLiteral("stories")).toArray()) {
        if (taken >= limit) break;
        const QJsonObject s = value.toObject();
        qint64 ts_ms = s.value(QStringLiteral("published_ts")).toVariant().toLongLong();
        if (ts_ms <= 0) continue;
        if (ts_ms < 100000000000LL) ts_ms *= 1000;  // seconds -> ms if needed
        if (ts_ms > as_of_ms) continue;             // no look-ahead at the source
        stories.append(QJsonObject{
            {QStringLiteral("headline"), s.value(QStringLiteral("headline")).toString()},
            {QStringLiteral("published_ts_ms"), ts_ms}});
        ++taken;
    }
    const QByteArray stdin_bytes =
        QJsonDocument(QJsonObject{{QStringLiteral("as_of_ms"), as_of_ms},
                                  {QStringLiteral("stories"), stories}})
            .toJson(QJsonDocument::Compact);

    // Scorer resolution: env override wins so the selftest can inject a stub.
    QString script = qEnvironmentVariable("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
    if (script.isEmpty()) script = default_scorer_script;
    if (python.isEmpty() || script.isEmpty()) {
        result.error = QStringLiteral("event-impact scorer unavailable (python/script missing)");
        return result;
    }

    const Capture r = run_capture_stdin(python, {script, QStringLiteral("score")}, stdin_bytes, 60000);
    if (!r.ok) {
        // NO snapshot written on failure -- consumer keeps last good / neutral.
        result.error = QStringLiteral("event-impact scorer failed: %1").arg(r.error);
        return result;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(r.stdout_text.toUtf8());
    if (!doc.isObject() || !doc.object().contains(QStringLiteral("events")) ||
        !doc.object().contains(QStringLiteral("as_of_ms"))) {
        // Never write a garbage snapshot.
        result.error = QStringLiteral("event-impact scorer produced malformed output");
        return result;
    }

    if (!write_snapshot(data_dir, QStringLiteral("btc-event-impact"), doc.object())) {
        result.error = QStringLiteral("failed to write btc-event-impact snapshot");
        return result;
    }

    result.ok = true;
    result.wrote_snapshot = true;
    result.events = doc.object().value(QStringLiteral("events")).toArray().size();
    result.stdout_json = r.stdout_text.trimmed();
    return result;
}

} // namespace openmarketterminal::services::news
