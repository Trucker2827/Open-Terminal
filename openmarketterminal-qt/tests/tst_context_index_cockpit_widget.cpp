#include "screens/kalshi/KalshiBotCockpitView.h"

#include <QDateTime>
#include <QPixmap>
#include <QtTest>

using namespace openmarketterminal::screens::kalshi;

class ContextIndexCockpitWidgetTest final : public QObject {
    Q_OBJECT

  private slots:
    void reload_wires_index_state_into_real_widget_and_renders();
};

void ContextIndexCockpitWidgetTest::reload_wires_index_state_into_real_widget_and_renders() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ContextIndexCockpitInput input;
    input.read_state = ContextIndexReadState::Current;
    input.current_batch_id = QStringLiteral("widget-batch");
    input.current_digest = QStringLiteral("sha256:abcdef0123456789");
    input.capture_finished_at_ms = now;
    input.node_count = 14;
    input.edge_count = 11;

    KalshiBotCockpitView view;
    view.set_live_status_provider([] { return QJsonObject(); });
    view.set_context_index_provider([input] { return input; });
    view.resize(1000, 760);
    view.reload();

    QCOMPARE(view.scene().context_index.state, QStringLiteral("indexed"));
    QCOMPARE(view.scene().context_index.role, QStringLiteral("cyan"));
    QVERIFY(view.scene().context_index.headline.contains(QStringLiteral("14 nodes · 11 edges")));
    QVERIFY(view.scene().context_index.detail.contains(QStringLiteral("OBSERVATION ONLY")));

    view.show();
    QCoreApplication::processEvents();
    const QPixmap rendered = view.grab();
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.size(), view.size());
}

QTEST_MAIN(ContextIndexCockpitWidgetTest)
#include "tst_context_index_cockpit_widget.moc"
