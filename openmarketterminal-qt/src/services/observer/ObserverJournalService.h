#pragma once

// Read-only bridge to the headless observer's output (trade-journal.md +
// observe_history.jsonl, written out-of-process by the daily/weekly LaunchAgents in
// trading-mcp-server). This service ONLY reads and parses those files — it has no
// write methods, never recomputes regime/thesis/scoring (it renders exactly what the
// Python observer produced), and has no path to any broker/order call. The app reads
// and displays; the LaunchAgent observes. Shared by the CLI `observe` command and,
// later, the GUI panel and an MCP read tool.

#include <QString>
#include <QVector>
#include <QJsonArray>
#include <optional>

namespace openmarketterminal::services {

class ObserverJournalService {
public:
    struct Block {
        QString kind;       // "daily" | "weekly" | "other"
        QString title;      // header text after "## " (e.g. "2026-06-16 18:38 (auto)")
        QString markdown;   // the full block text, including its header line
        QString alert;      // the ⚠️ ALERT line content, or empty if none
    };

    static ObserverJournalService& instance();

    // Path resolution: a test/override dir wins, else $OPENTERMINAL_OBSERVER_DIR, else
    // the default sibling project dir. Files are re-read on every call (they change
    // out-of-process), so there is no caching/staleness.
    QString journalDir() const;
    QString journalPath() const;    // <dir>/trade-journal.md
    QString historyPath() const;    // <dir>/observe_history.jsonl
    bool available() const;         // trade-journal.md exists?

    std::optional<Block> latestDaily() const;    // newest "(auto)" block
    std::optional<Block> latestWeekly() const;   // newest "Week review" block
    QVector<Block> recentAlerts(int limit = 10) const;  // daily blocks that fired an alert, newest first

    // Parsed observe_history.jsonl records. lastN <= 0 returns all.
    QJsonArray history(int lastN = 0) const;

    // Test seam: force the directory (takes precedence over env + default).
    void setDirOverride(const QString& dir);

private:
    ObserverJournalService() = default;
    QVector<Block> readBlocks() const;   // split the journal into classified blocks (file order)

    QString m_dirOverride;
};

} // namespace openmarketterminal::services
