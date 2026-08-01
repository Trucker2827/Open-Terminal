#include "services/news/BtcEventImpactSelftest.h"

#include "services/news/BtcEventImpact.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>

namespace openmarketterminal::services::news {

namespace {

int fail(const char* msg) {
    std::fprintf(stderr, "SELFTEST btc-event-impact FAILED: %s\n", msg);
    return 1;
}

bool write_text(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(bytes);
    return true;
}

} // namespace

int run_btc_event_impact_selftest() {
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
        return fail("python3 not found on PATH");

    QTemporaryDir tmp;
    if (!tmp.isValid())
        return fail("could not create temp dir");
    const QString data_dir = tmp.path();

    // Fixed "now": stories below it are eligible; a future story must be dropped
    // by the no-look-ahead filter.  `published_ts` is serialized as a STRING of
    // epoch-ms, exactly as BtcNewsPulse::to_json writes it.
    const qint64 as_of_ms = 1800000000000LL;  // ~2027-01
    const QByteArray pulse = QJsonDocument(QJsonObject{
        {QStringLiteral("stories"),
         QJsonArray{
             QJsonObject{{QStringLiteral("headline"), QStringLiteral("BTC ETF sees record inflow")},
                         {QStringLiteral("published_ts"), QStringLiteral("1700000000000")}},
             QJsonObject{{QStringLiteral("headline"), QStringLiteral("Future story that must be dropped")},
                         {QStringLiteral("published_ts"), QStringLiteral("1900000000000")}}}}}).toJson(QJsonDocument::Compact);
    if (!write_text(data_dir + QStringLiteral("/btc-news-pulse-latest.json"), pulse))
        return fail("could not write fixture pulse");

    // Valid stub scorer: ASSERTS the producer's no-look-ahead source filter by
    // inspecting the stdin it actually receives -- the fixture above carries a
    // future-published story that the producer must have dropped, so the stub
    // exits non-zero (failing branch 1) if any received story post-dates as_of
    // or if the eligible count isn't exactly the one past-dated story. Only
    // then does it echo a well-formed record with a recognizable marker (so the
    // failure branch below can prove it isn't clobbered). Without this, flipping
    // the filter's comparison would still pass the selftest.
    const QString good_scorer = data_dir + QStringLiteral("/good_scorer.py");
    const QByteArray good_src =
        R"PY(import sys, json
ctx = json.loads(sys.stdin.read() or "{}")
as_of = int(ctx.get("as_of_ms", 0))
stories = ctx.get("stories") or []
if any(int(s.get("published_ts_ms", 0)) > as_of for s in stories):
    sys.stderr.write("future-published story leaked past the source filter\n")
    sys.exit(4)
if len(stories) != 1:
    sys.stderr.write("expected exactly 1 eligible story, got %d\n" % len(stories))
    sys.exit(5)
sys.stdout.write(json.dumps({
  "as_of_ms": "1800000000000",
  "events": [{"event_ts_ms": 1700000000000, "direction": "UP",
              "magnitude": 0.5, "half_life_hours": 6, "kind": "ETF/FLOWS",
              "headline": "BTC ETF sees record inflow",
              "rationale": "selftest stub record"}],
  "model": "selftest-stub-model", "prompt_version": "selftest-v0"}))
)PY";
    if (!write_text(good_scorer, good_src))
        return fail("could not write good stub scorer");

    // Failing stub scorer: drains stdin then exits non-zero.
    const QString fail_scorer = data_dir + QStringLiteral("/fail_scorer.py");
    const QByteArray fail_src =
        "import sys\n"
        "sys.stdin.read()\n"
        "sys.exit(3)\n";
    if (!write_text(fail_scorer, fail_src))
        return fail("could not write failing stub scorer");

    const QString latest_path = data_dir + QStringLiteral("/btc-event-impact-latest.json");

    // ---- Branch 1: valid scorer -> snapshot written, parseable, events array.
    qputenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER", good_scorer.toUtf8());
    {
        const BtcEventImpactResult r =
            run_btc_event_impact(data_dir, python, QString(), 60, as_of_ms);
        if (!r.ok) {
            qunsetenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
            return fail(qUtf8Printable(QStringLiteral("valid scorer run did not succeed: %1").arg(r.error)));
        }
        if (!QFile::exists(latest_path)) {
            qunsetenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
            return fail("btc-event-impact-latest.json was not created");
        }
        QFile f(latest_path);
        if (!f.open(QIODevice::ReadOnly)) {
            qunsetenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
            return fail("could not open written snapshot");
        }
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (!doc.isObject() || !doc.object().value(QStringLiteral("events")).isArray() ||
            doc.object().value(QStringLiteral("events")).toArray().isEmpty()) {
            qunsetenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
            return fail("written snapshot has no events array");
        }
        if (doc.object().value(QStringLiteral("model")).toString() != QStringLiteral("selftest-stub-model")) {
            qunsetenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
            return fail("written snapshot did not preserve the scorer record verbatim");
        }
    }

    // Capture the good snapshot bytes for the clobber check below.
    QByteArray good_bytes;
    {
        QFile f(latest_path);
        if (f.open(QIODevice::ReadOnly)) good_bytes = f.readAll();
    }

    // ---- Branch 2: failing scorer -> non-zero, and good snapshot untouched.
    qputenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER", fail_scorer.toUtf8());
    {
        const BtcEventImpactResult r =
            run_btc_event_impact(data_dir, python, QString(), 60, as_of_ms);
        if (r.ok || r.wrote_snapshot) {
            qunsetenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
            return fail("failing scorer was treated as success");
        }
        QByteArray after_bytes;
        QFile f(latest_path);
        if (f.open(QIODevice::ReadOnly)) after_bytes = f.readAll();
        if (after_bytes != good_bytes) {
            qunsetenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
            return fail("failing scorer clobbered the last good snapshot");
        }
    }

    qunsetenv("OPENTERMINAL_BTC_EVENT_IMPACT_SCORER");
    std::printf("SELFTEST btc-event-impact OK\n");
    return 0;
}

} // namespace openmarketterminal::services::news
