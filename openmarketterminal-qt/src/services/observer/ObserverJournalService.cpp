#include "services/observer/ObserverJournalService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace openmarketterminal::services {

ObserverJournalService& ObserverJournalService::instance() {
    static ObserverJournalService s;
    return s;
}

void ObserverJournalService::setDirOverride(const QString& dir) {
    m_dirOverride = dir;
}

QString ObserverJournalService::journalDir() const {
    if (!m_dirOverride.isEmpty()) return m_dirOverride;
    const QString env = qEnvironmentVariable("OPENTERMINAL_OBSERVER_DIR");
    if (!env.isEmpty()) return env;
    return QDir::homePath() + "/src/Open-Terminal/trading-mcp-server";
}

QString ObserverJournalService::journalPath() const {
    return QDir(journalDir()).filePath("trade-journal.md");
}

QString ObserverJournalService::historyPath() const {
    return QDir(journalDir()).filePath("observe_history.jsonl");
}

bool ObserverJournalService::available() const {
    return QFileInfo::exists(journalPath());
}

static QString classify_kind(const QString& title) {
    if (title.startsWith("Week review")) return QStringLiteral("weekly");
    if (title.contains("(auto)"))        return QStringLiteral("daily");
    return QStringLiteral("other");
}

QVector<ObserverJournalService::Block> ObserverJournalService::readBlocks() const {
    QVector<Block> blocks;
    QFile f(journalPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return blocks;
    const QString text = QString::fromUtf8(f.readAll());

    Block cur;
    bool in = false;
    auto flush = [&]() {
        if (!in) return;
        cur.markdown = cur.markdown.trimmed();
        cur.kind = classify_kind(cur.title);
        blocks.push_back(cur);
    };
    const QStringList lines = text.split('\n');
    for (const QString& ln : lines) {
        if (ln.startsWith("## ")) {
            flush();
            cur = Block{};
            cur.title = ln.mid(3).trimmed();
            cur.markdown = ln + "\n";
            in = true;
        } else if (in) {
            cur.markdown += ln + "\n";
            if (cur.alert.isEmpty() && ln.contains("ALERT")) {
                const int p = ln.indexOf("ALERT:**");
                cur.alert = (p >= 0) ? ln.mid(p + 8).trimmed() : ln.trimmed();
            }
        }
    }
    flush();
    return blocks;
}

std::optional<ObserverJournalService::Block> ObserverJournalService::latestDaily() const {
    const auto blocks = readBlocks();
    for (int i = blocks.size() - 1; i >= 0; --i)
        if (blocks[i].kind == "daily") return blocks[i];
    return std::nullopt;
}

std::optional<ObserverJournalService::Block> ObserverJournalService::latestWeekly() const {
    const auto blocks = readBlocks();
    for (int i = blocks.size() - 1; i >= 0; --i)
        if (blocks[i].kind == "weekly") return blocks[i];
    return std::nullopt;
}

QVector<ObserverJournalService::Block> ObserverJournalService::recentAlerts(int limit) const {
    const auto blocks = readBlocks();
    QVector<Block> out;
    for (int i = blocks.size() - 1; i >= 0 && out.size() < limit; --i)
        if (blocks[i].kind == "daily" && !blocks[i].alert.isEmpty())
            out.push_back(blocks[i]);   // newest first
    return out;
}

QJsonArray ObserverJournalService::history(int lastN) const {
    QJsonArray arr;
    QFile f(historyPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return arr;
    const QString text = QString::fromUtf8(f.readAll());
    for (const QString& raw : text.split('\n')) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        const QJsonDocument d = QJsonDocument::fromJson(line.toUtf8());
        if (d.isObject()) arr.append(d.object());
    }
    if (lastN > 0 && arr.size() > lastN) {
        QJsonArray tail;
        for (int i = arr.size() - lastN; i < arr.size(); ++i) tail.append(arr[i]);
        return tail;
    }
    return arr;
}

} // namespace openmarketterminal::services
