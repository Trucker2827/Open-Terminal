#include <QtTest>

#include "core/layout/LayoutTemplates.h"

using namespace openmarketterminal::layout;

class TstLayoutTemplates : public QObject {
    Q_OBJECT

private slots:
    void crypto_trader_template_stays_multi_asset() {
        bool listed = false;
        for (const auto& persona : LayoutTemplates::personas()) {
            if (persona.id == QStringLiteral("crypto_trader")) {
                listed = true;
                QCOMPARE(persona.display_name, QStringLiteral("Crypto Trader"));
                QVERIFY(persona.description.contains(QStringLiteral("Multi-asset")));
            }
        }
        QVERIFY(listed);

        const Workspace w = LayoutTemplates::make(QStringLiteral("crypto_trader"));
        QCOMPARE(w.name, QStringLiteral("Crypto Trader"));
        QCOMPARE(w.frames.size(), 1);
        QCOMPARE(w.frames.first().panels.size(), 1);
        QCOMPARE(w.frames.first().panels.first().type_id, QStringLiteral("crypto_trading"));
        QCOMPARE(w.frames.first().panels.first().title, QStringLiteral("Crypto Trading"));
    }
};

QTEST_MAIN(TstLayoutTemplates)
#include "tst_layout_templates.moc"
