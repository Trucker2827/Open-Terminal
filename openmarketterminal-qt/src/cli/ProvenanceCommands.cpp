#include "cli/ProvenanceCommands.h"

#include "cli/CommandDispatch.h"
#include "core/config/AppPaths.h"
#include "services/provenance/ContextFixtureMaterializer.h"
#include "services/provenance/TradeProvenanceIndex.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>

namespace openmarketterminal::cli {
namespace {

bool take_path(QStringList& args, const QString& flag, QString& value) {
    const int index = args.indexOf(flag);
    if (index < 0)
        return true;
    if (index + 1 >= args.size())
        return false;
    value = args.at(index + 1);
    args.removeAt(index + 1);
    args.removeAt(index);
    return true;
}

QJsonObject status_json(const provenance::ContextIndexStatus& status, const QString& path) {
    const qint64 age_ms = status.capture_finished_at_ms > 0
                              ? std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - status.capture_finished_at_ms)
                              : 0;
    return {{QStringLiteral("path"), path},
            {QStringLiteral("present"), true},
            {QStringLiteral("schema_version"), status.schema_version},
            {QStringLiteral("has_current_batch"), status.has_current_batch},
            {QStringLiteral("current_batch_id"), status.current_batch_id},
            {QStringLiteral("current_digest"), status.current_digest},
            {QStringLiteral("capture_finished_at_ms"), QString::number(status.capture_finished_at_ms)},
            {QStringLiteral("age_ms"), QString::number(age_ms)},
            {QStringLiteral("nodes"), status.node_count},
            {QStringLiteral("edges"), status.edge_count},
            {QStringLiteral("unresolved"), status.unresolved_count},
            {QStringLiteral("invariant_failures"), status.invariant_failure_count},
            {QStringLiteral("authoritative"), false},
            {QStringLiteral("grants_trading_authority"), false}};
}

int emit_status(const GlobalOpts& opts, const QString& path) {
    if (!QFileInfo::exists(path)) {
        const QJsonObject output{{QStringLiteral("path"), path},
                                 {QStringLiteral("present"), false},
                                 {QStringLiteral("authoritative"), false},
                                 {QStringLiteral("grants_trading_authority"), false}};
        if (opts.json)
            std::printf("%s\n", QJsonDocument(output).toJson(QJsonDocument::Compact).constData());
        else
            std::printf("context index absent: %s\n", qUtf8Printable(path));
        return 0;
    }

    provenance::TradeProvenanceIndex index;
    const auto opened = index.open_read_only(path);
    if (opened.is_err()) {
        std::fprintf(stderr, "context index open failed: %s\n", opened.error().c_str());
        return 5;
    }
    const auto status = index.status();
    if (status.is_err()) {
        std::fprintf(stderr, "context index status failed: %s\n", status.error().c_str());
        return 5;
    }
    const QJsonObject output = status_json(status.value(), path);
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(output).toJson(QJsonDocument::Compact).constData());
    } else if (!status.value().has_current_batch) {
        std::printf("context index present, no complete batch published\npath: %s\n", qUtf8Printable(path));
    } else {
        std::printf(
            "context index batch %s\ndigest: %s\nnodes: %d  edges: %d  unresolved: %d  invariant failures: %d\n",
            qUtf8Printable(status.value().current_batch_id), qUtf8Printable(status.value().current_digest),
            status.value().node_count, status.value().edge_count, status.value().unresolved_count,
            status.value().invariant_failure_count);
    }
    return 0;
}

int verify_fixture(const GlobalOpts& opts, const QString& fixture_path, const QString& index_path) {
    QFile fixture(fixture_path);
    if (!fixture.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read provenance fixture: %s\n", qUtf8Printable(fixture.errorString()));
        return 5;
    }
    const auto batch = provenance::parse_context_fixture(fixture.readAll());
    if (batch.is_err()) {
        std::fprintf(stderr, "%s\n", batch.error().c_str());
        return 5;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        std::fprintf(stderr, "cannot create temporary provenance rebuild directory\n");
        return 5;
    }
    provenance::TradeProvenanceIndex rebuilt;
    const auto opened = rebuilt.open(temporary.filePath(QStringLiteral("context-index.db")));
    if (opened.is_err()) {
        std::fprintf(stderr, "%s\n", opened.error().c_str());
        return 5;
    }
    const auto published = rebuilt.publish(batch.value());
    if (published.is_err()) {
        std::fprintf(stderr, "%s\n", published.error().c_str());
        return 5;
    }

    QString current_digest;
    bool current_present = QFileInfo::exists(index_path);
    if (current_present) {
        provenance::TradeProvenanceIndex current;
        const auto current_opened = current.open_read_only(index_path);
        if (current_opened.is_err()) {
            std::fprintf(stderr, "%s\n", current_opened.error().c_str());
            return 5;
        }
        const auto current_status = current.status();
        if (current_status.is_err()) {
            std::fprintf(stderr, "%s\n", current_status.error().c_str());
            return 5;
        }
        current_present = current_status.value().has_current_batch;
        current_digest = current_status.value().current_digest;
    }
    const bool matches = current_present && current_digest == published.value();
    const QJsonObject output{{QStringLiteral("fixture"), fixture_path},
                             {QStringLiteral("index"), index_path},
                             {QStringLiteral("rebuilt_digest"), published.value()},
                             {QStringLiteral("current_digest"), current_digest},
                             {QStringLiteral("matches"), matches},
                             {QStringLiteral("fixture_only_foundation"), true},
                             {QStringLiteral("authoritative"), false},
                             {QStringLiteral("grants_trading_authority"), false}};
    if (opts.json)
        std::printf("%s\n", QJsonDocument(output).toJson(QJsonDocument::Compact).constData());
    else
        std::printf("fixture rebuild digest: %s\ncurrent digest: %s\nmatch: %s\n", qUtf8Printable(published.value()),
                    current_digest.isEmpty() ? "(none)" : qUtf8Printable(current_digest), matches ? "yes" : "no");
    return matches ? 0 : 4;
}

} // namespace

int provenance_command(const GlobalOpts& opts, QStringList args) {
    const QString subcommand = args.isEmpty() ? QStringLiteral("status") : args.takeFirst();
    if (opts.help || subcommand == QStringLiteral("help") || subcommand == QStringLiteral("--help") ||
        subcommand == QStringLiteral("-h")) {
        std::printf("usage: provenance status [--path PATH]\n"
                    "       provenance verify-rebuild --fixture FILE [--path PATH]\n");
        return 0;
    }
    QString path;
    if (!take_path(args, QStringLiteral("--path"), path)) {
        std::fprintf(stderr, "--path requires a value\n");
        return 2;
    }
    if (path.isEmpty())
        path = provenance::TradeProvenanceIndex::default_path(AppPaths::root());

    if (subcommand == QStringLiteral("status")) {
        if (!args.isEmpty()) {
            std::fprintf(stderr, "usage: provenance status [--path PATH]\n");
            return 2;
        }
        return emit_status(opts, path);
    }
    if (subcommand == QStringLiteral("verify-rebuild")) {
        QString fixture;
        if (!take_path(args, QStringLiteral("--fixture"), fixture) || fixture.isEmpty() || !args.isEmpty()) {
            std::fprintf(stderr, "usage: provenance verify-rebuild --fixture FILE [--path PATH]\n");
            return 2;
        }
        return verify_fixture(opts, fixture, path);
    }
    std::fprintf(stderr, "usage: provenance status|verify-rebuild\n");
    return 2;
}

} // namespace openmarketterminal::cli
