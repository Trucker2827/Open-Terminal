#include "services/provenance/ContextFixtureMaterializer.h"
#include "services/provenance/TradeProvenanceIndex.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace openmarketterminal::provenance;

namespace {

ContextBatch fixture_batch(const QString& id) {
    ContextBatch batch;
    batch.id = id;
    batch.capture_started_at_ms = 1000;
    batch.capture_finished_at_ms = 1100;
    batch.producer_version = QStringLiteral("test-materializer-v1");
    batch.source_snapshot = {{QStringLiteral("app_db_watermark"), 42}};

    batch.nodes = {
        {QStringLiteral("family:KXBTC15M"),
         QStringLiteral("contract_family"),
         QStringLiteral("KXBTC15M"),
         QStringLiteral("gate-family:KXBTC15M"),
         {},
         1001,
         900,
         QStringLiteral("legacy-shared"),
         1,
         {{QStringLiteral("family"), QStringLiteral("KXBTC15M")}}},
        {QStringLiteral("decision:d-1"),
         QStringLiteral("decision"),
         QStringLiteral("d-1"),
         QStringLiteral("decision_envelopes:d-1"),
         QStringLiteral("sha256:decision"),
         1002,
         950,
         QStringLiteral("profile:paper"),
         1,
         {{QStringLiteral("mode"), QStringLiteral("paper")}}},
    };
    batch.edges = {
        {QStringLiteral("edge:d-1-family"),
         QStringLiteral("decision:d-1"),
         QStringLiteral("family:KXBTC15M"),
         QStringLiteral("belongs_to_family"),
         QStringLiteral("decision_envelopes:d-1#family"),
         1003,
         950,
         QStringLiteral("profile:paper"),
         QStringLiteral("test-materializer-v1"),
         {}},
    };
    batch.source_cursors = {
        {QStringLiteral("application-db"),
         QStringLiteral("sqlite:data_version:42"),
         {{QStringLiteral("max_decision_id"), QStringLiteral("d-1")}}},
    };
    batch.unresolved = {
        {QStringLiteral("unresolved:legacy-report"),
         QStringLiteral("missing_report_hash"),
         QStringLiteral("legacy-report.json"),
         QStringLiteral("historical report predates hash stamps"),
         1004,
         {}},
    };
    return batch;
}

QByteArray fixture_json(const ContextBatch& batch) {
    QJsonArray nodes;
    for (const ContextNode& node : batch.nodes) {
        nodes.append(QJsonObject{{"id", node.id},
                                 {"kind", node.kind},
                                 {"canonical_key", node.canonical_key},
                                 {"authority_ref", node.authority_ref},
                                 {"content_hash", node.content_hash},
                                 {"observed_at_ms", QString::number(node.observed_at_ms)},
                                 {"effective_at_ms", QString::number(node.effective_at_ms)},
                                 {"scope", node.scope},
                                 {"schema_version", node.schema_version},
                                 {"metadata", node.metadata}});
    }
    QJsonArray edges;
    for (const ContextEdge& edge : batch.edges) {
        edges.append(QJsonObject{{"id", edge.id},
                                 {"from_node_id", edge.from_node_id},
                                 {"to_node_id", edge.to_node_id},
                                 {"relation", edge.relation},
                                 {"evidence_ref", edge.evidence_ref},
                                 {"observed_at_ms", QString::number(edge.observed_at_ms)},
                                 {"effective_at_ms", QString::number(edge.effective_at_ms)},
                                 {"scope", edge.scope},
                                 {"producer_version", edge.producer_version},
                                 {"metadata", edge.metadata}});
    }
    QJsonArray cursors;
    for (const SourceCursor& cursor : batch.source_cursors)
        cursors.append(QJsonObject{{"source_name", cursor.source_name},
                                   {"source_identity", cursor.source_identity},
                                   {"cursor", cursor.cursor}});
    QJsonArray unresolved;
    for (const UnresolvedRelationship& item : batch.unresolved) {
        unresolved.append(QJsonObject{{"id", item.id},
                                      {"code", item.code},
                                      {"authority_ref", item.authority_ref},
                                      {"detail", item.detail},
                                      {"observed_at_ms", QString::number(item.observed_at_ms)},
                                      {"metadata", item.metadata}});
    }
    QJsonArray failures;
    for (const InvariantFailure& failure : batch.invariant_failures) {
        failures.append(QJsonObject{{"id", failure.id},
                                    {"code", failure.code},
                                    {"evidence", failure.evidence},
                                    {"first_observed_at_ms", QString::number(failure.first_observed_at_ms)},
                                    {"last_observed_at_ms", QString::number(failure.last_observed_at_ms)},
                                    {"status", failure.status}});
    }
    return QJsonDocument(QJsonObject{{"id", batch.id},
                                     {"capture_started_at_ms", QString::number(batch.capture_started_at_ms)},
                                     {"capture_finished_at_ms", QString::number(batch.capture_finished_at_ms)},
                                     {"producer_version", batch.producer_version},
                                     {"source_snapshot", batch.source_snapshot},
                                     {"nodes", nodes},
                                     {"edges", edges},
                                     {"source_cursors", cursors},
                                     {"unresolved", unresolved},
                                     {"invariant_failures", failures}})
        .toJson(QJsonDocument::Compact);
}

#define REQUIRE_OK(result_expression)                                                                                  \
    do {                                                                                                               \
        const auto& require_ok_result = (result_expression);                                                           \
        if (require_ok_result.is_err())                                                                                \
            QFAIL(require_ok_result.error().c_str());                                                                  \
    } while (false)

} // namespace

class TradeProvenanceIndexTest final : public QObject {
    Q_OBJECT

  private slots:
    void creates_a_separate_empty_index();
    void publishes_a_complete_batch_and_reopens_it();
    void digest_is_independent_of_input_order_and_batch_identity();
    void failed_second_pass_preserves_the_previous_projection();
    void rejects_unknown_ontology_and_scope();
    void fixture_rebuild_is_deterministic();
    void read_only_open_cannot_publish();
    void repeated_ingestion_is_idempotent_but_id_reuse_is_not();
    void republish_refuses_an_incomplete_stored_batch();
    void metadata_is_allowlisted_and_bounded();
    void fixed_fixture_materializes();
    void current_views_switch_only_after_publish();
};

void TradeProvenanceIndexTest::creates_a_separate_empty_index() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString app_db = temp.filePath(QStringLiteral("openmarketterminal.db"));
    const QString context_db = temp.filePath(QStringLiteral("context-index.db"));

    {
        QSqlDatabase source =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("context_index_source_control"));
        source.setDatabaseName(app_db);
        QVERIFY(source.open());
        QSqlQuery q(source);
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE authority_sentinel(value TEXT NOT NULL)")));
        QVERIFY(q.exec(QStringLiteral("INSERT INTO authority_sentinel VALUES('untouched')")));
        source.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("context_index_source_control"));

    TradeProvenanceIndex index;
    REQUIRE_OK(index.open(context_db));
    const auto status = index.status();
    REQUIRE_OK(status);
    QCOMPARE(status.value().schema_version, 1);
    QVERIFY(!status.value().has_current_batch);
    QCOMPARE(QFileInfo(index.path()).canonicalFilePath(), QFileInfo(context_db).canonicalFilePath());
    QCOMPARE(TradeProvenanceIndex::default_path(QStringLiteral("/application-root")),
             QStringLiteral("/application-root/data/context-index.db"));

    QSqlDatabase source =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("context_index_source_verify"));
    source.setDatabaseName(app_db);
    QVERIFY(source.open());
    QSqlQuery q(source);
    QVERIFY(q.exec(QStringLiteral("SELECT value FROM authority_sentinel")));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QStringLiteral("untouched"));
    source.close();
    source = {};
    QSqlDatabase::removeDatabase(QStringLiteral("context_index_source_verify"));
}

void TradeProvenanceIndexTest::publishes_a_complete_batch_and_reopens_it() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("context-index.db"));

    QString digest;
    {
        TradeProvenanceIndex index;
        REQUIRE_OK(index.open(path));
        const auto published = index.publish(fixture_batch(QStringLiteral("batch-a")));
        REQUIRE_OK(published);
        digest = published.value();

        const auto status = index.status();
        REQUIRE_OK(status);
        QVERIFY(status.value().has_current_batch);
        QCOMPARE(status.value().current_batch_id, QStringLiteral("batch-a"));
        QCOMPARE(status.value().current_digest, digest);
        QCOMPARE(status.value().node_count, 2);
        QCOMPARE(status.value().edge_count, 1);
        QCOMPARE(status.value().unresolved_count, 1);
        QCOMPARE(status.value().invariant_failure_count, 0);
    }

    TradeProvenanceIndex reopened;
    REQUIRE_OK(reopened.open(path));
    const auto status = reopened.status();
    REQUIRE_OK(status);
    QCOMPARE(status.value().current_batch_id, QStringLiteral("batch-a"));
    QCOMPARE(status.value().current_digest, digest);
}

void TradeProvenanceIndexTest::digest_is_independent_of_input_order_and_batch_identity() {
    ContextBatch first = fixture_batch(QStringLiteral("batch-a"));
    ContextBatch reordered = fixture_batch(QStringLiteral("batch-b"));
    std::reverse(reordered.nodes.begin(), reordered.nodes.end());

    const auto a = TradeProvenanceIndex::canonical_digest(first);
    const auto b = TradeProvenanceIndex::canonical_digest(reordered);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    QCOMPARE(a.value(), b.value());
    QVERIFY(a.value().startsWith(QStringLiteral("sha256:")));
}

void TradeProvenanceIndexTest::failed_second_pass_preserves_the_previous_projection() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    TradeProvenanceIndex index;
    REQUIRE_OK(index.open(temp.filePath(QStringLiteral("context-index.db"))));

    const auto first = index.publish(fixture_batch(QStringLiteral("batch-good")));
    REQUIRE_OK(first);

    ContextBatch broken = fixture_batch(QStringLiteral("batch-broken"));
    broken.edges.push_back(broken.edges.front()); // duplicate key fails after node insertion
    const auto rejected = index.publish(broken);
    QVERIFY(rejected.is_err());

    const auto status = index.status();
    REQUIRE_OK(status);
    QCOMPARE(status.value().current_batch_id, QStringLiteral("batch-good"));
    QCOMPARE(status.value().current_digest, first.value());
    QCOMPARE(status.value().node_count, 2);
    QCOMPARE(status.value().edge_count, 1);

    // A status-only assertion is insufficient: a broken implementation could
    // leave the old pointer current while committing the failed batch's rows.
    // Reusing the failed id with corrected content must succeed, proving the
    // entire failed transaction was rolled back rather than merely unpublished.
    const auto repaired = index.publish(fixture_batch(QStringLiteral("batch-broken")));
    REQUIRE_OK(repaired);
    const auto repaired_status = index.status();
    REQUIRE_OK(repaired_status);
    QCOMPARE(repaired_status.value().current_batch_id, QStringLiteral("batch-broken"));
}

void TradeProvenanceIndexTest::rejects_unknown_ontology_and_scope() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    TradeProvenanceIndex index;
    REQUIRE_OK(index.open(temp.filePath(QStringLiteral("context-index.db"))));

    ContextBatch bad_kind = fixture_batch(QStringLiteral("bad-kind"));
    bad_kind.nodes[0].kind = QStringLiteral("persuasive_story");
    QVERIFY(index.publish(bad_kind).is_err());

    ContextBatch bad_scope = fixture_batch(QStringLiteral("bad-scope"));
    bad_scope.nodes[0].scope = QStringLiteral("whatever-profile-is-active");
    QVERIFY(index.publish(bad_scope).is_err());

    const auto status = index.status();
    REQUIRE_OK(status);
    QVERIFY(!status.value().has_current_batch);
}

void TradeProvenanceIndexTest::fixture_rebuild_is_deterministic() {
    const auto parsed = parse_context_fixture(fixture_json(fixture_batch(QStringLiteral("fixture-batch"))));
    REQUIRE_OK(parsed);

    QTemporaryDir first_dir;
    QTemporaryDir second_dir;
    QVERIFY(first_dir.isValid());
    QVERIFY(second_dir.isValid());
    TradeProvenanceIndex first;
    TradeProvenanceIndex second;
    REQUIRE_OK(first.open(first_dir.filePath(QStringLiteral("context-index.db"))));
    REQUIRE_OK(second.open(second_dir.filePath(QStringLiteral("context-index.db"))));
    const auto first_digest = first.publish(parsed.value());
    const auto second_digest = second.publish(parsed.value());
    REQUIRE_OK(first_digest);
    REQUIRE_OK(second_digest);
    QCOMPARE(first_digest.value(), second_digest.value());
    const auto first_status = first.status();
    const auto second_status = second.status();
    REQUIRE_OK(first_status);
    REQUIRE_OK(second_status);
    QCOMPARE(first_status.value().current_digest, second_status.value().current_digest);
}

void TradeProvenanceIndexTest::read_only_open_cannot_publish() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("context-index.db"));
    {
        TradeProvenanceIndex writer;
        REQUIRE_OK(writer.open(path));
        REQUIRE_OK(writer.publish(fixture_batch(QStringLiteral("published"))));
    }

    TradeProvenanceIndex reader;
    REQUIRE_OK(reader.open_read_only(path));
    const auto status = reader.status();
    REQUIRE_OK(status);
    QCOMPARE(status.value().current_batch_id, QStringLiteral("published"));
    const auto rejected = reader.publish(fixture_batch(QStringLiteral("must-not-write")));
    QVERIFY(rejected.is_err());
    QVERIFY(QString::fromStdString(rejected.error()).contains(QStringLiteral("read-only")));
}

void TradeProvenanceIndexTest::repeated_ingestion_is_idempotent_but_id_reuse_is_not() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    TradeProvenanceIndex index;
    REQUIRE_OK(index.open(temp.filePath(QStringLiteral("context-index.db"))));
    ContextBatch batch = fixture_batch(QStringLiteral("stable-source-snapshot"));
    const auto first = index.publish(batch);
    const auto repeated = index.publish(batch);
    REQUIRE_OK(first);
    REQUIRE_OK(repeated);
    QCOMPARE(first.value(), repeated.value());

    batch.nodes[1].content_hash = QStringLiteral("sha256:different-decision");
    const auto conflicting = index.publish(batch);
    QVERIFY(conflicting.is_err());
    QVERIFY(QString::fromStdString(conflicting.error()).contains(QStringLiteral("different content")));
    const auto status = index.status();
    REQUIRE_OK(status);
    QCOMPARE(status.value().current_digest, first.value());
}

void TradeProvenanceIndexTest::republish_refuses_an_incomplete_stored_batch() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("context-index.db"));
    const ContextBatch batch = fixture_batch(QStringLiteral("batch-corrupted"));

    {
        TradeProvenanceIndex index;
        REQUIRE_OK(index.open(path));
        REQUIRE_OK(index.publish(batch));
    }
    {
        QSqlDatabase corrupt = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("corrupt-index"));
        corrupt.setDatabaseName(path);
        QVERIFY(corrupt.open());
        QSqlQuery query(corrupt);
        QVERIFY(query.exec(QStringLiteral("DELETE FROM context_source_cursors WHERE batch_id='batch-corrupted'")));
        corrupt.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("corrupt-index"));

    TradeProvenanceIndex reopened;
    REQUIRE_OK(reopened.open(path));
    const auto republished = reopened.publish(batch);
    QVERIFY(republished.is_err());
    QCOMPARE(QString::fromStdString(republished.error()),
             QStringLiteral("stored context batch is incomplete or inconsistent"));
}

void TradeProvenanceIndexTest::metadata_is_allowlisted_and_bounded() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    TradeProvenanceIndex index;
    REQUIRE_OK(index.open(temp.filePath(QStringLiteral("context-index.db"))));

    ContextBatch secret = fixture_batch(QStringLiteral("secret"));
    secret.nodes[0].metadata.insert(QStringLiteral("api_token"), QStringLiteral("must-not-copy"));
    const auto secret_result = index.publish(secret);
    QVERIFY(secret_result.is_err());
    QVERIFY(QString::fromStdString(secret_result.error()).contains(QStringLiteral("non-allowlisted")));

    ContextBatch oversized = fixture_batch(QStringLiteral("oversized"));
    oversized.nodes[0].metadata.insert(QStringLiteral("reason"), QString(17 * 1024, QLatin1Char('x')));
    const auto oversized_result = index.publish(oversized);
    QVERIFY(oversized_result.is_err());
    QVERIFY(QString::fromStdString(oversized_result.error()).contains(QStringLiteral("16 KiB")));

    const auto status = index.status();
    REQUIRE_OK(status);
    QVERIFY(!status.value().has_current_batch);
}

void TradeProvenanceIndexTest::fixed_fixture_materializes() {
    const QString fixture_path = QFINDTESTDATA("fixtures/trade_provenance_foundation_v1.json");
    QVERIFY2(!fixture_path.isEmpty(), "fixed provenance fixture was not found");
    QFile fixture(fixture_path);
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const auto parsed = parse_context_fixture(fixture.readAll());
    REQUIRE_OK(parsed);

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    TradeProvenanceIndex index;
    REQUIRE_OK(index.open(temp.filePath(QStringLiteral("context-index.db"))));
    const auto published = index.publish(parsed.value());
    REQUIRE_OK(published);
    QCOMPARE(published.value(),
             QStringLiteral("sha256:1727af5fb1ec907ced0f318f16bf5431ba5067dddb55db962afacb79e06abef6"));
}

void TradeProvenanceIndexTest::current_views_switch_only_after_publish() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("context-index.db"));
    {
        TradeProvenanceIndex index;
        REQUIRE_OK(index.open(path));
        REQUIRE_OK(index.publish(fixture_batch(QStringLiteral("first"))));
        ContextBatch second = fixture_batch(QStringLiteral("second"));
        second.nodes.removeLast();
        second.edges.clear();
        REQUIRE_OK(index.publish(second));
    }

    QSqlDatabase reader =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("context_index_view_reader"));
    reader.setDatabaseName(path);
    reader.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    QVERIFY(reader.open());
    QSqlQuery query(reader);
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM context_current_nodes")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM context_current_edges")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
    reader.close();
    query = QSqlQuery();
    reader = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("context_index_view_reader"));
}

QTEST_MAIN(TradeProvenanceIndexTest)
#include "tst_trade_provenance_index.moc"
