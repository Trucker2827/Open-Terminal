// tst_strategy_registry.cpp — pure strategy registry (name -> factory + metadata).
#include "services/ai_strategy/StrategyRegistry.h"
#include "services/ai_strategy/Strategy.h"

#include <QtTest/QtTest>

using namespace openmarketterminal::ai_strategy;

namespace {
class FakeStrategy : public Strategy {
  public:
    explicit FakeStrategy(QStringList u) : u_(std::move(u)) {}
    QString name() const override { return QStringLiteral("fake"); }
    QStringList universe() const override { return u_; }
    QVector<TradeIntent> propose(const MarketSnapshot&) override { return {}; }
  private:
    QStringList u_;
};
} // namespace

class TstStrategyRegistry : public QObject {
    Q_OBJECT
  private slots:
    void register_list_build() {
        StrategyRegistry r;
        r.register_strategy(QStringLiteral("fake"), QStringLiteral("a test strategy"), false,
                            [](const StrategyBuildConfig& c) -> std::unique_ptr<Strategy> {
                                return std::make_unique<FakeStrategy>(c.symbols);
                            });
        const auto infos = r.list();
        QCOMPARE(infos.size(), 1);
        QCOMPARE(infos[0].name, QStringLiteral("fake"));
        QCOMPARE(infos[0].description, QStringLiteral("a test strategy"));
        QVERIFY(!infos[0].needs_provider);
        QVERIFY(r.has(QStringLiteral("fake")));
        QVERIFY(!r.has(QStringLiteral("nope")));
        auto built = r.build(QStringLiteral("fake"), {{QStringLiteral("BTC-USD")}, {}});
        QVERIFY(built != nullptr);
        QCOMPARE(built->universe(), QStringList{QStringLiteral("BTC-USD")});
        QVERIFY(r.build(QStringLiteral("nope"), {}) == nullptr);
    }

    void builtins_register_meanrev_and_claude() {
        StrategyRegistry r;
        register_builtin_strategies(r);
        QStringList names;
        for (const auto& i : r.list()) names << i.name;
        QVERIFY(names.contains(QStringLiteral("meanrev")));
        QVERIFY(names.contains(QStringLiteral("claude")));
    }
};

QTEST_GUILESS_MAIN(TstStrategyRegistry)
#include "tst_strategy_registry.moc"
