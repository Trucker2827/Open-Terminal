#include "screens/code_editor/EdgeIcEvidencePresentation.h"

#include <QTemporaryDir>
#include <QtTest>

using openmarketterminal::screens::edge_ic_evidence_row;

class EdgeIcEvidencePresentationTest final : public QObject {
    Q_OBJECT
  private slots:
    void missing_file_is_absent_not_fabricated();
    void malformed_json_is_absent();
    void wrong_event_is_absent();
    void published_report_lists_with_generated_time_and_venues();
    void zero_generated_at_reads_missing();

  private:
    QString write_file(const QTemporaryDir& dir, const QByteArray& bytes) {
        const QString path = dir.filePath(QStringLiteral("edge-ic.json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        file.write(bytes);
        return path;
    }
};

void EdgeIcEvidencePresentationTest::missing_file_is_absent_not_fabricated() {
    QTemporaryDir dir;
    const auto row = edge_ic_evidence_row(dir.filePath(QStringLiteral("edge-ic.json")));
    QVERIFY(!row.present);
}

void EdgeIcEvidencePresentationTest::malformed_json_is_absent() {
    QTemporaryDir dir;
    const auto row = edge_ic_evidence_row(write_file(dir, "not json"));
    QVERIFY(!row.present);
}

void EdgeIcEvidencePresentationTest::wrong_event_is_absent() {
    QTemporaryDir dir;
    const auto row = edge_ic_evidence_row(write_file(dir, R"({"event":"something_else"})"));
    QVERIFY(!row.present);
}

void EdgeIcEvidencePresentationTest::published_report_lists_with_generated_time_and_venues() {
    QTemporaryDir dir;
    const QByteArray payload = R"({
        "schema": 1, "event": "edge_ic_report", "generated_at_ms": 1753228800000,
        "window_days": 30,
        "overall": {"resolved_decisions": 3},
        "venues": {"coinbase": {"resolved_decisions": 2}, "kraken": {"resolved_decisions": 1}}
    })";
    const auto row = edge_ic_evidence_row(write_file(dir, payload));
    QVERIFY(row.present);
    QCOMPARE(row.title, QStringLiteral("edge-ic"));
    QCOMPARE(row.type, QStringLiteral("EDGE IC REPORT"));
    QCOMPARE(row.generated, QStringLiteral("2025-07-23 00:00 UTC"));
    QVERIFY(row.detail.contains(QStringLiteral("3 resolved decisions")));
    QVERIFY(row.detail.contains(QStringLiteral("coinbase, kraken")));
}

void EdgeIcEvidencePresentationTest::zero_generated_at_reads_missing() {
    QTemporaryDir dir;
    const QByteArray payload =
        R"({"event":"edge_ic_report","overall":{"resolved_decisions":0},"venues":{}})";
    const auto row = edge_ic_evidence_row(write_file(dir, payload));
    QVERIFY(row.present);
    QCOMPARE(row.generated, QStringLiteral("--"));
    QVERIFY(row.detail.contains(QStringLiteral("venues: none")));
}

QTEST_APPLESS_MAIN(EdgeIcEvidencePresentationTest)
#include "tst_edge_ic_evidence_presentation.moc"
