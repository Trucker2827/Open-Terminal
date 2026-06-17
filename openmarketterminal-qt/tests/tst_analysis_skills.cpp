#include <QtTest>
#include <QTemporaryDir>
#include <QFile>

#include "services/llm/AnalysisSkillLoader.h"

using openmarketterminal::services::AnalysisSkillLoader;

static void write(const QString& path, const QByteArray& body) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(body);
    f.close();
}

class TstAnalysisSkills : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        write(dir_.filePath("comps.md"), "# Skill: Comparable Company Analysis\nUse edgar_get_financials and edgar_calc_multiples.");
        write(dir_.filePath("dcf.md"), "# Skill: DCF Valuation\nProject FCF and discount at WACC.");
        write(dir_.filePath("NOTICE.md"), "Attribution: anthropics/financial-services Apache-2.0");
        AnalysisSkillLoader::instance().setDirOverride(dir_.path());
    }

    void names_exclude_notice_and_are_sorted() {
        const QStringList n = AnalysisSkillLoader::instance().skill_names();
        QCOMPARE(n, (QStringList{"comps", "dcf"}));   // NOTICE.md excluded, .md stripped, sorted
    }

    void fragment_has_sentinel_and_skill_bodies() {
        const QString f = AnalysisSkillLoader::instance().system_prompt_fragment();
        QVERIFY(f.contains("[Analysis skills]"));               // idempotency sentinel
        QVERIFY(f.contains("playbook: comps"));
        QVERIFY(f.contains("playbook: dcf"));
        QVERIFY(f.contains("edgar_calc_multiples"));            // real methodology body
        QVERIFY(f.contains("discount at WACC"));
        QVERIFY(!f.contains("Attribution:"));                   // NOTICE not injected
    }

    void empty_dir_yields_empty_fragment() {
        QTemporaryDir empty;
        QVERIFY(empty.isValid());
        auto& svc = AnalysisSkillLoader::instance();
        svc.setDirOverride(empty.path());
        QVERIFY(svc.system_prompt_fragment().isEmpty());
        QVERIFY(svc.skill_names().isEmpty());
        svc.setDirOverride(dir_.path());   // restore for any later slot
    }
};

QTEST_MAIN(TstAnalysisSkills)
#include "tst_analysis_skills.moc"
