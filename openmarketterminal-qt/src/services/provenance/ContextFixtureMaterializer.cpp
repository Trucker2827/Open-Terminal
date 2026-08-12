#include "services/provenance/ContextFixtureMaterializer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace openmarketterminal::provenance {
namespace {

QString string_value(const QJsonObject& object, const char* key) {
    return object.value(QString::fromLatin1(key)).toString();
}

qint64 integer_value(const QJsonObject& object, const char* key) {
    return object.value(QString::fromLatin1(key)).toVariant().toLongLong();
}

Result<QJsonArray> array_value(const QJsonObject& object, const char* key) {
    const QJsonValue value = object.value(QString::fromLatin1(key));
    if (value.isUndefined())
        return Result<QJsonArray>::ok({});
    if (!value.isArray())
        return Result<QJsonArray>::err((std::string("fixture field is not an array: ") + key));
    return Result<QJsonArray>::ok(value.toArray());
}

} // namespace

Result<ContextBatch> parse_context_fixture(const QByteArray& json) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return Result<ContextBatch>::err(("invalid context fixture JSON: " + parse_error.errorString()).toStdString());

    const QJsonObject root = document.object();
    ContextBatch batch;
    batch.id = string_value(root, "id");
    batch.capture_started_at_ms = integer_value(root, "capture_started_at_ms");
    batch.capture_finished_at_ms = integer_value(root, "capture_finished_at_ms");
    batch.producer_version = string_value(root, "producer_version");
    batch.source_snapshot = root.value(QStringLiteral("source_snapshot")).toObject();

    const auto nodes = array_value(root, "nodes");
    const auto edges = array_value(root, "edges");
    const auto cursors = array_value(root, "source_cursors");
    const auto unresolved = array_value(root, "unresolved");
    const auto failures = array_value(root, "invariant_failures");
    for (const auto* value : {&nodes, &edges, &cursors, &unresolved, &failures}) {
        if (value->is_err())
            return Result<ContextBatch>::err(value->error());
    }

    for (const QJsonValue& value : nodes.value()) {
        if (!value.isObject())
            return Result<ContextBatch>::err("fixture node is not an object");
        const QJsonObject object = value.toObject();
        batch.nodes.push_back({string_value(object, "id"), string_value(object, "kind"),
                               string_value(object, "canonical_key"), string_value(object, "authority_ref"),
                               string_value(object, "content_hash"), integer_value(object, "observed_at_ms"),
                               integer_value(object, "effective_at_ms"), string_value(object, "scope"),
                               object.value(QStringLiteral("schema_version")).toInt(1),
                               object.value(QStringLiteral("metadata")).toObject()});
    }
    for (const QJsonValue& value : edges.value()) {
        if (!value.isObject())
            return Result<ContextBatch>::err("fixture edge is not an object");
        const QJsonObject object = value.toObject();
        batch.edges.push_back({string_value(object, "id"), string_value(object, "from_node_id"),
                               string_value(object, "to_node_id"), string_value(object, "relation"),
                               string_value(object, "evidence_ref"), integer_value(object, "observed_at_ms"),
                               integer_value(object, "effective_at_ms"), string_value(object, "scope"),
                               string_value(object, "producer_version"),
                               object.value(QStringLiteral("metadata")).toObject()});
    }
    for (const QJsonValue& value : cursors.value()) {
        if (!value.isObject())
            return Result<ContextBatch>::err("fixture source cursor is not an object");
        const QJsonObject object = value.toObject();
        batch.source_cursors.push_back({string_value(object, "source_name"), string_value(object, "source_identity"),
                                        object.value(QStringLiteral("cursor")).toObject()});
    }
    for (const QJsonValue& value : unresolved.value()) {
        if (!value.isObject())
            return Result<ContextBatch>::err("fixture unresolved item is not an object");
        const QJsonObject object = value.toObject();
        batch.unresolved.push_back({string_value(object, "id"), string_value(object, "code"),
                                    string_value(object, "authority_ref"), string_value(object, "detail"),
                                    integer_value(object, "observed_at_ms"),
                                    object.value(QStringLiteral("metadata")).toObject()});
    }
    for (const QJsonValue& value : failures.value()) {
        if (!value.isObject())
            return Result<ContextBatch>::err("fixture invariant failure is not an object");
        const QJsonObject object = value.toObject();
        batch.invariant_failures.push_back(
            {string_value(object, "id"), string_value(object, "code"),
             object.value(QStringLiteral("evidence")).toObject(), integer_value(object, "first_observed_at_ms"),
             integer_value(object, "last_observed_at_ms"), string_value(object, "status")});
    }
    return Result<ContextBatch>::ok(std::move(batch));
}

} // namespace openmarketterminal::provenance
