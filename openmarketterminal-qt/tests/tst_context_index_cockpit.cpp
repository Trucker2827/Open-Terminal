#include "screens/kalshi/ContextIndexCockpitPresentation.h"
#include "screens/kalshi/ContextIndexCockpitReader.h"
#include "services/provenance/TradeProvenanceIndex.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace openmarketterminal::provenance;
using namespace openmarketterminal::screens::kalshi;

namespace {

#define REQUIRE_OK(result_expression)                                                                                  \
    do {                                                                                                               \
        const auto& require_ok_result = (result_expression);                                                           \
        if (require_ok_result.is_err())                                                                                \
            QFAIL(require_ok_result.error().c_str());                                                                  \
    } while (false)

ContextBatch batch_at(qint64 finished_at_ms) {
    ContextBatch batch;
    batch.id = QStringLiteral("cockpit-batch");
    batch.capture_started_at_ms = finished_at_ms - 100;
    batch.capture_finished_at_ms = finished_at_ms;
    batch.producer_version = QStringLiteral("cockpit-test-v1");
    batch.source_snapshot = {{QStringLiteral("app_db_watermark"), 7}};
    batch.nodes = {
        {QStringLiteral("family:KXBTC15M"),
         QStringLiteral("contract_family"),
         QStringLiteral("KXBTC15M"),
         QStringLiteral("gate-family:KXBTC15M"),
         {},
         finished_at_ms,
         finished_at_ms,
         QStringLiteral("legacy-shared"),
         1,
         {{QStringLiteral("family"), QStringLiteral("KXBTC15M")}}},
        {QStringLiteral("decision:d-1"),
         QStringLiteral("decision"),
         QStringLiteral("d-1"),
         QStringLiteral("decision_envelopes:d-1"),
         QStringLiteral("sha256:decision"),
         finished_at_ms,
         finished_at_ms,
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
         finished_at_ms,
         finished_at_ms,
         QStringLiteral("profile:paper"),
         QStringLiteral("cockpit-test-v1"),
         {}},
    };
    return batch;
}

ContextIndexCockpitInput current_input(qint64 finished_at_ms) {
    ContextIndexCockpitInput input;
    input.read_state = ContextIndexReadState::Current;
    input.current_batch_id = QStringLiteral("batch-7");
    input.current_digest = QStringLiteral("sha256:1234567890abcdef");
    input.capture_finished_at_ms = finished_at_ms;
    input.node_count = 12;
    input.edge_count = 9;
    return input;
}

} // namespace

class ContextIndexCockpitTest final : public QObject {
    Q_OBJECT

  private slots:
    void absent_read_is_non_creating_and_truthful();
    void empty_index_is_not_presented_as_current();
    void fresh_index_is_cyan_not_authority_green();
    void stale_unresolved_and_invariant_states_have_strict_precedence();
    void reader_surfaces_real_counts_and_corruption();
};

void ContextIndexCockpitTest::absent_read_is_non_creating_and_truthful() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("context-index.db"));

    const ContextIndexCockpitInput input = read_context_index_cockpit(path);
    QCOMPARE(input.read_state, ContextIndexReadState::Absent);
    QVERIFY(!QFile::exists(path));
    const ContextIndexCockpitCard card = present_context_index_cockpit(input, 1'000'000);
    QCOMPARE(card.state, QStringLiteral("absent"));
    QVERIFY(card.headline.contains(QStringLiteral("NOT MATERIALIZED")));
    QVERIFY(card.detail.contains(QStringLiteral("OBSERVATION ONLY")));
}

void ContextIndexCockpitTest::empty_index_is_not_presented_as_current() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("context-index.db"));
    TradeProvenanceIndex index;
    REQUIRE_OK(index.open(path));
    index.close();

    const ContextIndexCockpitCard card = present_context_index_cockpit(read_context_index_cockpit(path), 1'000'000);
    QCOMPARE(card.state, QStringLiteral("empty"));
    QCOMPARE(card.role, QStringLiteral("grey"));
    QVERIFY(card.headline.contains(QStringLiteral("EMPTY")));
}

void ContextIndexCockpitTest::fresh_index_is_cyan_not_authority_green() {
    const qint64 now = 2'000'000;
    const ContextIndexCockpitCard card = present_context_index_cockpit(current_input(now - 1'000), now);
    QCOMPARE(card.state, QStringLiteral("indexed"));
    QCOMPARE(card.role, QStringLiteral("cyan"));
    QVERIFY(card.role != QStringLiteral("green"));
    QVERIFY(card.headline.contains(QStringLiteral("12 nodes · 9 edges")));
    QVERIFY(card.detail.contains(QStringLiteral("OBSERVATION ONLY")));
}

void ContextIndexCockpitTest::stale_unresolved_and_invariant_states_have_strict_precedence() {
    const qint64 now = 10'000'000;
    ContextIndexCockpitInput input = current_input(now - kContextIndexCockpitStaleAfterMs - 1);
    QCOMPARE(present_context_index_cockpit(input, now).state, QStringLiteral("stale"));

    input.unresolved_count = 3;
    ContextIndexCockpitCard card = present_context_index_cockpit(input, now);
    QCOMPARE(card.state, QStringLiteral("unresolved"));
    QCOMPARE(card.role, QStringLiteral("amber"));
    QVERIFY(card.headline.contains(QStringLiteral("3 UNRESOLVED")));

    input.invariant_failure_count = 2;
    card = present_context_index_cockpit(input, now);
    QCOMPARE(card.state, QStringLiteral("invariant_failure"));
    QCOMPARE(card.role, QStringLiteral("red"));
    QVERIFY(card.headline.contains(QStringLiteral("2 INVARIANT FAILURES")));
}

void ContextIndexCockpitTest::reader_surfaces_real_counts_and_corruption() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("context-index.db"));
    const qint64 now = 20'000'000;
    {
        TradeProvenanceIndex index;
        REQUIRE_OK(index.open(path));
        REQUIRE_OK(index.publish(batch_at(now - 500)));
    }

    const ContextIndexCockpitInput input = read_context_index_cockpit(path);
    QCOMPARE(input.read_state, ContextIndexReadState::Current);
    QCOMPARE(input.node_count, 2);
    QCOMPARE(input.edge_count, 1);
    QCOMPARE(present_context_index_cockpit(input, now).state, QStringLiteral("indexed"));

    const QString corrupt_path = temp.filePath(QStringLiteral("corrupt.db"));
    QFile corrupt(corrupt_path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    QCOMPARE(corrupt.write("not sqlite"), qint64(10));
    corrupt.close();
    const ContextIndexCockpitInput corrupt_input = read_context_index_cockpit(corrupt_path);
    QCOMPARE(corrupt_input.read_state, ContextIndexReadState::Unreadable);
    QCOMPARE(present_context_index_cockpit(corrupt_input, now).state, QStringLiteral("unreadable"));
}

QTEST_MAIN(ContextIndexCockpitTest)
#include "tst_context_index_cockpit.moc"
