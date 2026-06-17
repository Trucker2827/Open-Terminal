#include "services/llm/AnalysisSkillLoader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace openmarketterminal::services {

AnalysisSkillLoader& AnalysisSkillLoader::instance() {
    static AnalysisSkillLoader s;
    return s;
}

void AnalysisSkillLoader::setDirOverride(const QString& dir) {
    m_dirOverride = dir;
}

QString AnalysisSkillLoader::skills_dir() const {
    if (!m_dirOverride.isEmpty())
        return m_dirOverride;
    const QString base = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        base + "/../Resources/resources/ai_skills",  // .app bundle
        base + "/resources/ai_skills",               // portable / dev build output
        QStringLiteral("resources/ai_skills"),        // cwd (dev)
    };
    for (const QString& c : candidates) {
        if (QFileInfo::exists(c))
            return c;
    }
    return candidates.first();
}

QVector<QPair<QString, QString>> AnalysisSkillLoader::load() const {
    QVector<QPair<QString, QString>> out;
    QDir dir(skills_dir());
    if (!dir.exists())
        return out;
    QStringList files = dir.entryList({"*.md"}, QDir::Files, QDir::Name);
    for (const QString& fn : files) {
        if (fn.compare("NOTICE.md", Qt::CaseInsensitive) == 0)
            continue;  // attribution, not methodology
        QFile f(dir.filePath(fn));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString md = QString::fromUtf8(f.readAll()).trimmed();
        if (md.isEmpty())
            continue;
        QString name = fn;
        name.chop(3);  // drop ".md"
        out.append({name, md});
    }
    return out;
}

QStringList AnalysisSkillLoader::skill_names() const {
    QStringList names;
    for (const auto& s : load())
        names << s.first;
    return names;
}

QString AnalysisSkillLoader::system_prompt_fragment() const {
    const auto skills = load();
    if (skills.isEmpty())
        return {};
    QString frag =
        "\n\n[Analysis skills] You have these financial-analysis playbooks. When the user "
        "asks for the matching analysis (comps, DCF/intrinsic value, earnings/filing note), "
        "FOLLOW the playbook step by step using the named tools, and build the output in the "
        "Report Builder. Use only the app's data tools — never web-guess financial figures.\n";
    for (const auto& s : skills)
        frag += "\n=== playbook: " + s.first + " ===\n" + s.second + "\n";
    return frag;
}

} // namespace openmarketterminal::services
