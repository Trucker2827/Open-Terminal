#pragma once

#include "core/result/Result.h"

#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

namespace openmarketterminal::provenance {

struct ContextNode {
    QString id;
    QString kind;
    QString canonical_key;
    QString authority_ref;
    QString content_hash;
    qint64 observed_at_ms = 0;
    qint64 effective_at_ms = 0;
    QString scope;
    int schema_version = 1;
    QJsonObject metadata;
};

struct ContextEdge {
    QString id;
    QString from_node_id;
    QString to_node_id;
    QString relation;
    QString evidence_ref;
    qint64 observed_at_ms = 0;
    qint64 effective_at_ms = 0;
    QString scope;
    QString producer_version;
    QJsonObject metadata;
};

struct SourceCursor {
    QString source_name;
    QString source_identity;
    QJsonObject cursor;
};

struct UnresolvedRelationship {
    QString id;
    QString code;
    QString authority_ref;
    QString detail;
    qint64 observed_at_ms = 0;
    QJsonObject metadata;
};

struct InvariantFailure {
    QString id;
    QString code;
    QJsonObject evidence;
    qint64 first_observed_at_ms = 0;
    qint64 last_observed_at_ms = 0;
    QString status;
};

struct ContextBatch {
    QString id;
    qint64 capture_started_at_ms = 0;
    qint64 capture_finished_at_ms = 0;
    QString producer_version;
    QJsonObject source_snapshot;
    QVector<ContextNode> nodes;
    QVector<ContextEdge> edges;
    QVector<SourceCursor> source_cursors;
    QVector<UnresolvedRelationship> unresolved;
    QVector<InvariantFailure> invariant_failures;
};

struct ContextIndexStatus {
    int schema_version = 0;
    bool has_current_batch = false;
    QString current_batch_id;
    QString current_digest;
    qint64 capture_finished_at_ms = 0;
    int node_count = 0;
    int edge_count = 0;
    int unresolved_count = 0;
    int invariant_failure_count = 0;
};

/// Single-threaded owner of one context-index SQLite connection. Writers and
/// readers use separate instances; readers must use open_read_only().
class TradeProvenanceIndex final {
  public:
    TradeProvenanceIndex();
    ~TradeProvenanceIndex();

    TradeProvenanceIndex(const TradeProvenanceIndex&) = delete;
    TradeProvenanceIndex& operator=(const TradeProvenanceIndex&) = delete;

    Result<void> open(const QString& path);
    Result<void> open_read_only(const QString& path);
    void close();
    bool is_open() const;
    QString path() const;

    Result<QString> publish(const ContextBatch& batch);
    Result<ContextIndexStatus> status() const;

    static Result<QString> canonical_digest(const ContextBatch& batch);
    /// The index is application-level, not active-profile-level. Callers pass
    /// AppPaths::root(), never AppPaths::data().
    static QString default_path(const QString& application_root);

  private:
    Result<void> open_impl(const QString& path, bool read_only);
    Result<void> initialize_schema();
    Result<void> validate_schema() const;

    QString connection_name_;
    QString path_;
    QSqlDatabase db_;
    bool read_only_ = false;
};

} // namespace openmarketterminal::provenance
