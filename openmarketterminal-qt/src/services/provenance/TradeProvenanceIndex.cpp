#include "services/provenance/TradeProvenanceIndex.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <algorithm>

namespace openmarketterminal::provenance {
namespace {

constexpr int kSchemaVersion = 1;

const QSet<QString>& node_kinds() {
    static const QSet<QString> values = {QStringLiteral("source_observation"),
                                         QStringLiteral("contract_family"),
                                         QStringLiteral("market_contract"),
                                         QStringLiteral("model_report"),
                                         QStringLiteral("model_trust_verdict"),
                                         QStringLiteral("sealed_gate"),
                                         QStringLiteral("gate_verdict"),
                                         QStringLiteral("decision"),
                                         QStringLiteral("order"),
                                         QStringLiteral("fill"),
                                         QStringLiteral("settlement"),
                                         QStringLiteral("quarantine")};
    return values;
}

const QSet<QString>& edge_relations() {
    static const QSet<QString> values = {QStringLiteral("belongs_to_family"),  QStringLiteral("derived_from"),
                                         QStringLiteral("paper_admitted_by"),  QStringLiteral("evaluated_by"),
                                         QStringLiteral("live_authorized_by"), QStringLiteral("submitted_as"),
                                         QStringLiteral("filled_as"),          QStringLiteral("settled_as"),
                                         QStringLiteral("excluded_by"),        QStringLiteral("supersedes")};
    return values;
}

bool valid_scope(const QString& scope) {
    return scope == QStringLiteral("legacy-shared") || scope == QStringLiteral("unknown") ||
           (scope.startsWith(QStringLiteral("profile:")) && scope.size() > 8);
}

Result<void> validate_payload(const QJsonObject& object, const QSet<QString>& allowed_keys, const char* label) {
    constexpr qsizetype kMaxPayloadBytes = 16 * 1024;
    if (QJsonDocument(object).toJson(QJsonDocument::Compact).size() > kMaxPayloadBytes)
        return Result<void>::err(std::string(label) + " exceeds 16 KiB");
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed_keys.contains(it.key()))
            return Result<void>::err(std::string(label) + " contains non-allowlisted key: " + it.key().toStdString());
        if (it.value().isArray() || it.value().isObject())
            return Result<void>::err(std::string(label) + " values must be bounded scalars");
    }
    return Result<void>::ok();
}

const QSet<QString>& source_snapshot_keys() {
    static const QSet<QString> keys = {
        QStringLiteral("app_db_watermark"),         QStringLiteral("app_db_data_version"),
        QStringLiteral("kalshi_ledger_generation"), QStringLiteral("kalshi_ledger_offset"),
        QStringLiteral("report_content_hash"),      QStringLiteral("gate_content_hash")};
    return keys;
}

const QSet<QString>& node_metadata_keys() {
    static const QSet<QString> keys = {
        QStringLiteral("family"),     QStringLiteral("mode"),        QStringLiteral("account_id"),
        QStringLiteral("venue"),      QStringLiteral("ticker"),      QStringLiteral("side"),
        QStringLiteral("price"),      QStringLiteral("quantity"),    QStringLiteral("status"),
        QStringLiteral("profile_id"), QStringLiteral("report_kind"), QStringLiteral("verdict"),
        QStringLiteral("reason"),     QStringLiteral("outcome"),     QStringLiteral("settlement_event_id"),
        QStringLiteral("source_type")};
    return keys;
}

const QSet<QString>& edge_metadata_keys() {
    static const QSet<QString> keys = {QStringLiteral("family"), QStringLiteral("mode"),   QStringLiteral("account_id"),
                                       QStringLiteral("venue"),  QStringLiteral("status"), QStringLiteral("reason")};
    return keys;
}

const QSet<QString>& cursor_keys() {
    static const QSet<QString> keys = {QStringLiteral("max_decision_id"), QStringLiteral("max_row_id"),
                                       QStringLiteral("byte_offset"), QStringLiteral("generation"),
                                       QStringLiteral("data_version")};
    return keys;
}

const QSet<QString>& unresolved_metadata_keys() {
    static const QSet<QString> keys = {QStringLiteral("source"), QStringLiteral("family"), QStringLiteral("mode"),
                                       QStringLiteral("expected"), QStringLiteral("actual")};
    return keys;
}

const QSet<QString>& invariant_evidence_keys() {
    static const QSet<QString> keys = {QStringLiteral("authority_ref"), QStringLiteral("family"),
                                       QStringLiteral("mode"), QStringLiteral("expected"), QStringLiteral("actual")};
    return keys;
}

QByteArray frame(const QByteArray& value) {
    return QByteArray::number(value.size()) + ':' + value;
}

QByteArray canonical_json(const QJsonValue& value) {
    if (value.isNull() || value.isUndefined())
        return QByteArrayLiteral("null");
    if (value.isBool())
        return value.toBool() ? QByteArrayLiteral("true") : QByteArrayLiteral("false");
    if (value.isDouble()) {
        const QJsonDocument wrapper(QJsonArray{value});
        const QByteArray encoded = wrapper.toJson(QJsonDocument::Compact);
        return encoded.mid(1, encoded.size() - 2);
    }
    if (value.isString()) {
        const QJsonDocument wrapper(QJsonArray{value});
        const QByteArray encoded = wrapper.toJson(QJsonDocument::Compact);
        return encoded.mid(1, encoded.size() - 2);
    }
    if (value.isArray()) {
        QByteArray out("[");
        const QJsonArray array = value.toArray();
        for (const QJsonValue& item : array)
            out += frame(canonical_json(item));
        out += ']';
        return out;
    }

    QByteArray out("{");
    const QJsonObject object = value.toObject();
    QStringList keys = object.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString& key : keys) {
        out += frame(key.toUtf8());
        out += frame(canonical_json(object.value(key)));
    }
    out += '}';
    return out;
}

void append_field(QByteArray& out, const QByteArray& value) {
    out += frame(value);
}

void append_field(QByteArray& out, const QString& value) {
    append_field(out, value.toUtf8());
}

void append_field(QByteArray& out, qint64 value) {
    append_field(out, QByteArray::number(value));
}

Result<void> validate_batch(const ContextBatch& batch) {
    if (batch.id.trimmed().isEmpty())
        return Result<void>::err("context batch id is empty");
    if (batch.capture_started_at_ms <= 0 || batch.capture_finished_at_ms <= 0 ||
        batch.capture_finished_at_ms < batch.capture_started_at_ms)
        return Result<void>::err("context batch capture interval is invalid");
    if (batch.producer_version.trimmed().isEmpty())
        return Result<void>::err("context batch producer version is empty");
    auto payload = validate_payload(batch.source_snapshot, source_snapshot_keys(), "source snapshot");
    if (payload.is_err())
        return payload;

    QSet<QString> node_ids;
    for (const ContextNode& node : batch.nodes) {
        if (node.id.isEmpty() || node.canonical_key.isEmpty() || node.authority_ref.isEmpty())
            return Result<void>::err("context node identity is incomplete");
        if (!node_kinds().contains(node.kind))
            return Result<void>::err(("unknown context node kind: " + node.kind).toStdString());
        if (!valid_scope(node.scope))
            return Result<void>::err(("invalid context node scope: " + node.scope).toStdString());
        if (node.schema_version <= 0)
            return Result<void>::err("context node schema version must be positive");
        payload = validate_payload(node.metadata, node_metadata_keys(), "node metadata");
        if (payload.is_err())
            return payload;
        node_ids.insert(node.id);
    }

    for (const ContextEdge& edge : batch.edges) {
        if (edge.id.isEmpty() || edge.from_node_id.isEmpty() || edge.to_node_id.isEmpty() ||
            edge.evidence_ref.isEmpty() || edge.producer_version.isEmpty())
            return Result<void>::err("context edge identity is incomplete");
        if (!edge_relations().contains(edge.relation))
            return Result<void>::err(("unknown context edge relation: " + edge.relation).toStdString());
        if (!valid_scope(edge.scope))
            return Result<void>::err(("invalid context edge scope: " + edge.scope).toStdString());
        if (!node_ids.contains(edge.from_node_id) || !node_ids.contains(edge.to_node_id))
            return Result<void>::err("context edge endpoint is absent from its batch");
        payload = validate_payload(edge.metadata, edge_metadata_keys(), "edge metadata");
        if (payload.is_err())
            return payload;
    }
    for (const SourceCursor& cursor : batch.source_cursors) {
        if (cursor.source_name.isEmpty() || cursor.source_identity.isEmpty())
            return Result<void>::err("context source cursor identity is incomplete");
        payload = validate_payload(cursor.cursor, cursor_keys(), "source cursor");
        if (payload.is_err())
            return payload;
    }
    for (const UnresolvedRelationship& item : batch.unresolved) {
        if (item.id.isEmpty() || item.code.isEmpty() || item.authority_ref.isEmpty())
            return Result<void>::err("unresolved relationship identity is incomplete");
        payload = validate_payload(item.metadata, unresolved_metadata_keys(), "unresolved metadata");
        if (payload.is_err())
            return payload;
    }
    for (const InvariantFailure& failure : batch.invariant_failures) {
        if (failure.id.isEmpty() || failure.code.isEmpty() || failure.status.isEmpty())
            return Result<void>::err("invariant failure identity is incomplete");
        payload = validate_payload(failure.evidence, invariant_evidence_keys(), "invariant evidence");
        if (payload.is_err())
            return payload;
    }
    return Result<void>::ok();
}

Result<void> exec_sql(QSqlDatabase& db, const QString& sql) {
    QSqlQuery query(db);
    if (!query.exec(sql))
        return Result<void>::err(query.lastError().text().toStdString());
    return Result<void>::ok();
}

Result<void> exec_prepared(QSqlDatabase& db, const QString& sql, const QVariantList& values) {
    QSqlQuery query(db);
    if (!query.prepare(sql))
        return Result<void>::err(query.lastError().text().toStdString());
    for (const QVariant& value : values)
        query.addBindValue(value);
    if (!query.exec())
        return Result<void>::err(query.lastError().text().toStdString());
    return Result<void>::ok();
}

QString json_text(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString nonnull(const QString& value) {
    return value.isNull() ? QStringLiteral("") : value;
}

} // namespace

TradeProvenanceIndex::TradeProvenanceIndex()
    : connection_name_(
          QStringLiteral("trade_provenance_index_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {}

TradeProvenanceIndex::~TradeProvenanceIndex() {
    close();
}

Result<void> TradeProvenanceIndex::open(const QString& path) {
    return open_impl(path, false);
}

Result<void> TradeProvenanceIndex::open_read_only(const QString& path) {
    return open_impl(path, true);
}

Result<void> TradeProvenanceIndex::open_impl(const QString& path, bool read_only) {
    close();
    if (path.trimmed().isEmpty())
        return Result<void>::err("context index path is empty");
    const QFileInfo target(path);
    if (read_only && !target.isFile())
        return Result<void>::err("context index does not exist");
    if (!read_only && !QDir().mkpath(target.absolutePath()))
        return Result<void>::err(("cannot create context index directory: " + target.absolutePath()).toStdString());

    connection_name_ =
        QStringLiteral("trade_provenance_index_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name_);
    db_.setDatabaseName(path);
    if (read_only)
        db_.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!db_.open()) {
        const std::string error = db_.lastError().text().toStdString();
        close();
        return Result<void>::err(error);
    }
    path_ = path;
    read_only_ = read_only;

    QStringList pragmas{QStringLiteral("PRAGMA foreign_keys=ON"), QStringLiteral("PRAGMA busy_timeout=5000")};
    if (!read_only) {
        pragmas.append(QStringLiteral("PRAGMA journal_mode=WAL"));
        pragmas.append(QStringLiteral("PRAGMA synchronous=NORMAL"));
    }
    for (const QString& pragma : pragmas) {
        const auto result = exec_sql(db_, pragma);
        if (result.is_err()) {
            const std::string error = result.error();
            close();
            return Result<void>::err(error);
        }
    }
    const auto schema = read_only ? validate_schema() : initialize_schema();
    if (schema.is_err()) {
        const std::string error = schema.error();
        close();
        return Result<void>::err(error);
    }
    return Result<void>::ok();
}

void TradeProvenanceIndex::close() {
    const QString old_name = connection_name_;
    if (db_.isValid())
        db_.close();
    db_ = QSqlDatabase();
    path_.clear();
    read_only_ = false;
    if (!old_name.isEmpty() && QSqlDatabase::contains(old_name))
        QSqlDatabase::removeDatabase(old_name);
}

bool TradeProvenanceIndex::is_open() const {
    return db_.isValid() && db_.isOpen();
}

QString TradeProvenanceIndex::path() const {
    return path_;
}

QString TradeProvenanceIndex::default_path(const QString& application_root) {
    return QDir(application_root).filePath(QStringLiteral("data/context-index.db"));
}

Result<void> TradeProvenanceIndex::initialize_schema() {
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS context_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS context_batches ("
                       "id TEXT PRIMARY KEY, capture_started_at_ms INTEGER NOT NULL, "
                       "capture_finished_at_ms INTEGER NOT NULL, producer_version TEXT NOT NULL, "
                       "source_snapshot_json TEXT NOT NULL, committed_at_ms INTEGER NOT NULL, "
                       "digest TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS context_nodes ("
            "batch_id TEXT NOT NULL, node_id TEXT NOT NULL, kind TEXT NOT NULL, "
            "canonical_key TEXT NOT NULL, authority_ref TEXT NOT NULL, content_hash TEXT NOT NULL, "
            "observed_at_ms INTEGER NOT NULL, effective_at_ms INTEGER NOT NULL, scope TEXT NOT NULL, "
            "schema_version INTEGER NOT NULL, metadata_json TEXT NOT NULL, "
            "PRIMARY KEY(batch_id,node_id), FOREIGN KEY(batch_id) REFERENCES context_batches(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS context_edges ("
            "batch_id TEXT NOT NULL, edge_id TEXT NOT NULL, from_node_id TEXT NOT NULL, "
            "to_node_id TEXT NOT NULL, relation TEXT NOT NULL, evidence_ref TEXT NOT NULL, "
            "observed_at_ms INTEGER NOT NULL, effective_at_ms INTEGER NOT NULL, scope TEXT NOT NULL, "
            "producer_version TEXT NOT NULL, metadata_json TEXT NOT NULL, PRIMARY KEY(batch_id,edge_id), "
            "FOREIGN KEY(batch_id,from_node_id) REFERENCES context_nodes(batch_id,node_id) ON DELETE CASCADE, "
            "FOREIGN KEY(batch_id,to_node_id) REFERENCES context_nodes(batch_id,node_id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS context_source_cursors ("
                       "batch_id TEXT NOT NULL, source_name TEXT NOT NULL, source_identity TEXT NOT NULL, "
                       "cursor_json TEXT NOT NULL, PRIMARY KEY(batch_id,source_name), "
                       "FOREIGN KEY(batch_id) REFERENCES context_batches(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS context_unresolved ("
            "batch_id TEXT NOT NULL, id TEXT NOT NULL, code TEXT NOT NULL, authority_ref TEXT NOT NULL, "
            "detail TEXT NOT NULL, observed_at_ms INTEGER NOT NULL, metadata_json TEXT NOT NULL, "
            "PRIMARY KEY(batch_id,id), FOREIGN KEY(batch_id) REFERENCES context_batches(id) ON DELETE CASCADE)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS context_invariant_failures ("
            "batch_id TEXT NOT NULL, id TEXT NOT NULL, code TEXT NOT NULL, evidence_json TEXT NOT NULL, "
            "first_observed_at_ms INTEGER NOT NULL, last_observed_at_ms INTEGER NOT NULL, status TEXT NOT NULL, "
            "PRIMARY KEY(batch_id,id), FOREIGN KEY(batch_id) REFERENCES context_batches(id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_context_nodes_kind ON context_nodes(batch_id,kind)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_context_edges_from ON context_edges(batch_id,from_node_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_context_edges_to ON context_edges(batch_id,to_node_id)"),
        QStringLiteral("CREATE VIEW IF NOT EXISTS context_current_nodes AS "
                       "SELECT n.* FROM context_nodes n JOIN context_meta m "
                       "ON m.key='current_batch_id' AND n.batch_id=m.value"),
        QStringLiteral("CREATE VIEW IF NOT EXISTS context_current_edges AS "
                       "SELECT e.* FROM context_edges e JOIN context_meta m "
                       "ON m.key='current_batch_id' AND e.batch_id=m.value"),
        QStringLiteral("CREATE VIEW IF NOT EXISTS context_current_source_cursors AS "
                       "SELECT c.* FROM context_source_cursors c JOIN context_meta m "
                       "ON m.key='current_batch_id' AND c.batch_id=m.value"),
        QStringLiteral("CREATE VIEW IF NOT EXISTS context_current_unresolved AS "
                       "SELECT u.* FROM context_unresolved u JOIN context_meta m "
                       "ON m.key='current_batch_id' AND u.batch_id=m.value"),
        QStringLiteral("CREATE VIEW IF NOT EXISTS context_current_invariant_failures AS "
                       "SELECT f.* FROM context_invariant_failures f JOIN context_meta m "
                       "ON m.key='current_batch_id' AND f.batch_id=m.value"),
    };

    const auto begin = exec_sql(db_, QStringLiteral("BEGIN IMMEDIATE"));
    if (begin.is_err())
        return begin;
    for (const QString& statement : statements) {
        const auto result = exec_sql(db_, statement);
        if (result.is_err()) {
            exec_sql(db_, QStringLiteral("ROLLBACK"));
            return result;
        }
    }
    auto version =
        exec_prepared(db_, QStringLiteral("INSERT OR IGNORE INTO context_meta(key,value) VALUES('schema_version',?)"),
                      {QString::number(kSchemaVersion)});
    if (version.is_err()) {
        exec_sql(db_, QStringLiteral("ROLLBACK"));
        return version;
    }
    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("SELECT value FROM context_meta WHERE key='schema_version'")) || !query.next()) {
        const std::string error = query.lastError().text().toStdString();
        exec_sql(db_, QStringLiteral("ROLLBACK"));
        return Result<void>::err(error.empty() ? "context schema version is missing" : error);
    }
    if (query.value(0).toInt() != kSchemaVersion) {
        exec_sql(db_, QStringLiteral("ROLLBACK"));
        return Result<void>::err("unsupported context index schema version");
    }
    const auto commit = exec_sql(db_, QStringLiteral("COMMIT"));
    if (commit.is_err()) {
        exec_sql(db_, QStringLiteral("ROLLBACK"));
        return commit;
    }
    return Result<void>::ok();
}

Result<void> TradeProvenanceIndex::validate_schema() const {
    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("SELECT value FROM context_meta WHERE key='schema_version'")) || !query.next()) {
        const QString error = query.lastError().text();
        return Result<void>::err(error.isEmpty() ? "context index schema version is missing" : error.toStdString());
    }
    if (query.value(0).toInt() != kSchemaVersion)
        return Result<void>::err("unsupported context index schema version");
    return Result<void>::ok();
}

Result<QString> TradeProvenanceIndex::canonical_digest(const ContextBatch& batch) {
    const auto valid = validate_batch(batch);
    if (valid.is_err())
        return Result<QString>::err(valid.error());

    QByteArray material(QByteArrayLiteral("trade-provenance-current-v1"));
    append_field(material, batch.producer_version);
    append_field(material, canonical_json(batch.source_snapshot));

    QVector<ContextNode> nodes = batch.nodes;
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const ContextNode& node : nodes) {
        append_field(material, QByteArrayLiteral("node"));
        append_field(material, node.id);
        append_field(material, node.kind);
        append_field(material, node.canonical_key);
        append_field(material, node.authority_ref);
        append_field(material, node.content_hash);
        append_field(material, node.observed_at_ms);
        append_field(material, node.effective_at_ms);
        append_field(material, node.scope);
        append_field(material, node.schema_version);
        append_field(material, canonical_json(node.metadata));
    }

    QVector<ContextEdge> edges = batch.edges;
    std::sort(edges.begin(), edges.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const ContextEdge& edge : edges) {
        append_field(material, QByteArrayLiteral("edge"));
        append_field(material, edge.id);
        append_field(material, edge.from_node_id);
        append_field(material, edge.to_node_id);
        append_field(material, edge.relation);
        append_field(material, edge.evidence_ref);
        append_field(material, edge.observed_at_ms);
        append_field(material, edge.effective_at_ms);
        append_field(material, edge.scope);
        append_field(material, edge.producer_version);
        append_field(material, canonical_json(edge.metadata));
    }

    QVector<SourceCursor> cursors = batch.source_cursors;
    std::sort(cursors.begin(), cursors.end(),
              [](const auto& a, const auto& b) { return a.source_name < b.source_name; });
    for (const SourceCursor& cursor : cursors) {
        append_field(material, QByteArrayLiteral("cursor"));
        append_field(material, cursor.source_name);
        append_field(material, cursor.source_identity);
        append_field(material, canonical_json(cursor.cursor));
    }

    QVector<UnresolvedRelationship> unresolved = batch.unresolved;
    std::sort(unresolved.begin(), unresolved.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const UnresolvedRelationship& item : unresolved) {
        append_field(material, QByteArrayLiteral("unresolved"));
        append_field(material, item.id);
        append_field(material, item.code);
        append_field(material, item.authority_ref);
        append_field(material, item.detail);
        append_field(material, item.observed_at_ms);
        append_field(material, canonical_json(item.metadata));
    }

    QVector<InvariantFailure> failures = batch.invariant_failures;
    std::sort(failures.begin(), failures.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const InvariantFailure& failure : failures) {
        append_field(material, QByteArrayLiteral("invariant"));
        append_field(material, failure.id);
        append_field(material, failure.code);
        append_field(material, canonical_json(failure.evidence));
        append_field(material, failure.first_observed_at_ms);
        append_field(material, failure.last_observed_at_ms);
        append_field(material, failure.status);
    }

    const QByteArray digest = QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
    return Result<QString>::ok(QStringLiteral("sha256:") + QString::fromLatin1(digest));
}

Result<QString> TradeProvenanceIndex::publish(const ContextBatch& batch) {
    if (!is_open())
        return Result<QString>::err("context index is not open");
    if (read_only_)
        return Result<QString>::err("context index was opened read-only");
    const auto digest_result = canonical_digest(batch);
    if (digest_result.is_err())
        return digest_result;
    const QString digest = digest_result.value();

    const auto begin = exec_sql(db_, QStringLiteral("BEGIN IMMEDIATE"));
    if (begin.is_err())
        return Result<QString>::err(begin.error());
    const auto fail = [this](const std::string& error) {
        exec_sql(db_, QStringLiteral("ROLLBACK"));
        return Result<QString>::err(error);
    };

    QSqlQuery existing(db_);
    existing.prepare(QStringLiteral("SELECT digest FROM context_batches WHERE id=?"));
    existing.addBindValue(batch.id);
    if (!existing.exec())
        return fail(existing.lastError().text().toStdString());
    if (existing.next()) {
        if (existing.value(0).toString() != digest)
            return fail("context batch id already exists with different content");

        QSqlQuery counts(db_);
        counts.prepare(QStringLiteral("SELECT "
                                      "(SELECT COUNT(*) FROM context_nodes WHERE batch_id=?),"
                                      "(SELECT COUNT(*) FROM context_edges WHERE batch_id=?),"
                                      "(SELECT COUNT(*) FROM context_source_cursors WHERE batch_id=?),"
                                      "(SELECT COUNT(*) FROM context_unresolved WHERE batch_id=?),"
                                      "(SELECT COUNT(*) FROM context_invariant_failures WHERE batch_id=?)"));
        for (int i = 0; i < 5; ++i)
            counts.addBindValue(batch.id);
        if (!counts.exec() || !counts.next())
            return fail(counts.lastError().text().toStdString());
        if (counts.value(0).toInt() != batch.nodes.size() || counts.value(1).toInt() != batch.edges.size() ||
            counts.value(2).toInt() != batch.source_cursors.size() ||
            counts.value(3).toInt() != batch.unresolved.size() ||
            counts.value(4).toInt() != batch.invariant_failures.size())
            return fail("stored context batch is incomplete or inconsistent");

        auto republish =
            exec_prepared(db_,
                          QStringLiteral("INSERT INTO context_meta(key,value) VALUES('current_batch_id',?) "
                                         "ON CONFLICT(key) DO UPDATE SET value=excluded.value"),
                          {batch.id});
        if (republish.is_err())
            return fail(republish.error());
        const auto commit_existing = exec_sql(db_, QStringLiteral("COMMIT"));
        if (commit_existing.is_err())
            return fail(commit_existing.error());
        return Result<QString>::ok(digest);
    }

    auto result = exec_prepared(
        db_,
        QStringLiteral("INSERT INTO context_batches(id,capture_started_at_ms,capture_finished_at_ms,"
                       "producer_version,source_snapshot_json,committed_at_ms,digest) VALUES(?,?,?,?,?,?,?)"),
        {batch.id, batch.capture_started_at_ms, batch.capture_finished_at_ms, batch.producer_version,
         json_text(batch.source_snapshot), QDateTime::currentMSecsSinceEpoch(), digest});
    if (result.is_err())
        return fail(result.error());

    for (const ContextNode& node : batch.nodes) {
        result = exec_prepared(
            db_,
            QStringLiteral(
                "INSERT INTO context_nodes(batch_id,node_id,kind,canonical_key,authority_ref,content_hash,"
                "observed_at_ms,effective_at_ms,scope,schema_version,metadata_json) VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
            {batch.id, node.id, node.kind, node.canonical_key, node.authority_ref, nonnull(node.content_hash),
             node.observed_at_ms, node.effective_at_ms, node.scope, node.schema_version, json_text(node.metadata)});
        if (result.is_err())
            return fail(result.error());
    }
    for (const ContextEdge& edge : batch.edges) {
        result = exec_prepared(
            db_,
            QStringLiteral(
                "INSERT INTO context_edges(batch_id,edge_id,from_node_id,to_node_id,relation,evidence_ref,"
                "observed_at_ms,effective_at_ms,scope,producer_version,metadata_json) VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
            {batch.id, edge.id, edge.from_node_id, edge.to_node_id, edge.relation, edge.evidence_ref,
             edge.observed_at_ms, edge.effective_at_ms, edge.scope, edge.producer_version, json_text(edge.metadata)});
        if (result.is_err())
            return fail(result.error());
    }
    for (const SourceCursor& cursor : batch.source_cursors) {
        result = exec_prepared(
            db_,
            QStringLiteral(
                "INSERT INTO context_source_cursors(batch_id,source_name,source_identity,cursor_json) VALUES(?,?,?,?)"),
            {batch.id, cursor.source_name, cursor.source_identity, json_text(cursor.cursor)});
        if (result.is_err())
            return fail(result.error());
    }
    for (const UnresolvedRelationship& item : batch.unresolved) {
        result = exec_prepared(
            db_,
            QStringLiteral(
                "INSERT INTO context_unresolved(batch_id,id,code,authority_ref,detail,observed_at_ms,metadata_json) "
                "VALUES(?,?,?,?,?,?,?)"),
            {batch.id, item.id, item.code, item.authority_ref, item.detail, item.observed_at_ms,
             json_text(item.metadata)});
        if (result.is_err())
            return fail(result.error());
    }
    for (const InvariantFailure& failure : batch.invariant_failures) {
        result = exec_prepared(db_,
                               QStringLiteral("INSERT INTO "
                                              "context_invariant_failures(batch_id,id,code,evidence_json,first_"
                                              "observed_at_ms,last_observed_at_ms,status) VALUES(?,?,?,?,?,?,?)"),
                               {batch.id, failure.id, failure.code, json_text(failure.evidence),
                                failure.first_observed_at_ms, failure.last_observed_at_ms, failure.status});
        if (result.is_err())
            return fail(result.error());
    }
    result = exec_prepared(db_,
                           QStringLiteral("INSERT INTO context_meta(key,value) VALUES('current_batch_id',?) "
                                          "ON CONFLICT(key) DO UPDATE SET value=excluded.value"),
                           {batch.id});
    if (result.is_err())
        return fail(result.error());

    const auto commit = exec_sql(db_, QStringLiteral("COMMIT"));
    if (commit.is_err())
        return fail(commit.error());
    return Result<QString>::ok(digest);
}

Result<ContextIndexStatus> TradeProvenanceIndex::status() const {
    if (!is_open())
        return Result<ContextIndexStatus>::err("context index is not open");

    ContextIndexStatus status;
    QSqlQuery version(db_);
    if (!version.exec(QStringLiteral("SELECT value FROM context_meta WHERE key='schema_version'")) || !version.next())
        return Result<ContextIndexStatus>::err("context index schema version is missing");
    status.schema_version = version.value(0).toInt();

    QSqlQuery current(db_);
    if (!current.exec(QStringLiteral("SELECT value FROM context_meta WHERE key='current_batch_id'")))
        return Result<ContextIndexStatus>::err(current.lastError().text().toStdString());
    if (!current.next())
        return Result<ContextIndexStatus>::ok(status);
    status.has_current_batch = true;
    status.current_batch_id = current.value(0).toString();

    QSqlQuery details(db_);
    details.prepare(QStringLiteral("SELECT b.digest,b.capture_finished_at_ms,"
                                   "(SELECT COUNT(*) FROM context_nodes n WHERE n.batch_id=b.id),"
                                   "(SELECT COUNT(*) FROM context_edges e WHERE e.batch_id=b.id),"
                                   "(SELECT COUNT(*) FROM context_unresolved u WHERE u.batch_id=b.id),"
                                   "(SELECT COUNT(*) FROM context_invariant_failures f WHERE f.batch_id=b.id) "
                                   "FROM context_batches b WHERE b.id=?"));
    details.addBindValue(status.current_batch_id);
    if (!details.exec() || !details.next())
        return Result<ContextIndexStatus>::err("current context batch metadata is missing");
    status.current_digest = details.value(0).toString();
    status.capture_finished_at_ms = details.value(1).toLongLong();
    status.node_count = details.value(2).toInt();
    status.edge_count = details.value(3).toInt();
    status.unresolved_count = details.value(4).toInt();
    status.invariant_failure_count = details.value(5).toInt();
    return Result<ContextIndexStatus>::ok(status);
}

} // namespace openmarketterminal::provenance
