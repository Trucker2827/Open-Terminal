#pragma once
// Loads bundled financial-analysis skill playbooks (resources/ai_skills/*.md) and
// builds a system-prompt fragment that teaches the in-app LLM to follow them using
// the app's own read-only tools (edgar_*, get_quote, report_*). Read-only; no data
// or trading path. Methodology adapted from anthropics/financial-services (Apache-2.0)
// — see resources/ai_skills/NOTICE.md.
#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>

namespace openmarketterminal::services {

class AnalysisSkillLoader {
  public:
    static AnalysisSkillLoader& instance();

    // System-prompt fragment for all loaded skills, prefixed with the
    // "[Analysis skills]" sentinel (used for idempotent injection). Empty if none found.
    QString system_prompt_fragment() const;

    QStringList skill_names() const;     // e.g. ["comps","dcf","earnings_note"]
    QString skills_dir() const;          // resolved directory (diagnostics)
    void setDirOverride(const QString& dir);  // test seam

  private:
    AnalysisSkillLoader() = default;
    QVector<QPair<QString, QString>> load() const;  // [(name, markdown)], sorted, excludes NOTICE

    QString m_dirOverride;
};

} // namespace openmarketterminal::services
