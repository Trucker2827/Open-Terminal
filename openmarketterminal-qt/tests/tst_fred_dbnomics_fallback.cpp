#include "screens/economics/panels/FredDbnomicsFallback.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::screens;

class TstFredDbnomicsFallback : public QObject {
    Q_OBJECT
  private slots:
    void mapped_series_resolve() {
        const auto gdp = fred_dbnomics_fallback("GDP");
        QVERIFY(gdp.has_value());
        QCOMPARE(gdp->provider, QString("BEA"));
        QCOMPARE(gdp->dataset, QString("NIPA-T10105"));
        QCOMPARE(gdp->series, QString("A191RC-Q"));
        QVERIFY(fred_dbnomics_fallback("gdpc1").has_value()); // case-insensitive
        QCOMPARE(fred_dbnomics_fallback("UNRATE")->series, QString("LNS14000000"));
        QCOMPARE(fred_dbnomics_fallback("FEDFUNDS")->provider, QString("FED"));
        QCOMPARE(fred_dbnomics_fallback("DGS10")->series, QString("RIFLGFCY10_N.B"));
    }
    void unmapped_series_stay_keyed() {
        // No verified DBnomics mirror — must NOT invent one.
        for (const char* id : {"M2SL", "INDPRO", "SP500", "VIXCLS", "MORTGAGE30US", "HOUST", "NOPE"})
            QVERIFY2(!fred_dbnomics_fallback(id).has_value(), id);
    }
    void well_formed() {
        for (const char* id : {"GDP", "GDPC1", "CPIAUCSL", "CPILFESL", "UNRATE", "FEDFUNDS", "DGS10", "DGS2"}) {
            const auto f = fred_dbnomics_fallback(id);
            QVERIFY2(f.has_value(), id);
            QVERIFY(!f->provider.isEmpty());
            QVERIFY(!f->dataset.isEmpty());
            QVERIFY(!f->series.isEmpty());
            QVERIFY(!f->source_label.isEmpty());
        }
    }
};

QTEST_MAIN(TstFredDbnomicsFallback)
#include "tst_fred_dbnomics_fallback.moc"
