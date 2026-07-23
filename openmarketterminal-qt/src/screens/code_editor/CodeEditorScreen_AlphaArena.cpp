// ALPHA ARENA page — N local/remote models on live Kalshi prediction markets.
// Read-only mirror of arena-report.json + arena-rounds.jsonl (written by
// scripts/arena/). Same evidence-file pattern as the Forecast Arena page;
// no execution authority anywhere near this screen.

#include "screens/code_editor/CodeEditorScreen.h"

#include "ui/theme/Theme.h"

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace openmarketterminal::screens {
using namespace openmarketterminal::ui;

namespace {

QString alpha_arena_root() {
    return QDir::homePath() +
           QStringLiteral("/Library/Application Support/Open Terminal/Open Terminal");
}

QJsonObject read_json_object(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

// Last complete round from the append-only journal without reading the whole
// file — tail the final 16KB and take the last parseable line.
QJsonObject read_last_round(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const qint64 tail = 16 * 1024;
    if (file.size() > tail)
        file.seek(file.size() - tail);
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty()) continue;
        const auto doc = QJsonDocument::fromJson(line.toUtf8());
        if (doc.isObject()) return doc.object();
    }
    return {};
}

QString fmt_brier(const QJsonValue& v) {
    return v.isDouble() ? QString::number(v.toDouble(), 'f', 4) : QStringLiteral("--");
}

} // namespace

QWidget* CodeEditorScreen::build_alpha_arena_page() {
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("nbLabPage"));
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(14, 10, 14, 12);
    root->setSpacing(8);

    auto* title = new QLabel(tr("ALPHA ARENA · AI MODELS vs PREDICTION MARKETS"), page);
    title->setObjectName(QStringLiteral("nbLabTitle"));
    root->addWidget(title);

    auto* safety = new QLabel(
        tr("SHADOW ONLY · NO ORDERS · SEALED BLIND FORECASTS · SETTLED BY KALSHI"), page);
    safety->setStyleSheet(
        QStringLiteral("color:%1;border:1px solid %1;padding:7px;font-weight:900;letter-spacing:1px;")
            .arg(colors::WARNING()));
    root->addWidget(safety);

    aa_verdict_ = new QLabel(page);
    aa_verdict_->setWordWrap(true);
    aa_verdict_->setMinimumHeight(44);
    root->addWidget(aa_verdict_);

    aa_round_ = new QLabel(page);
    aa_round_->setWordWrap(true);
    aa_round_->setStyleSheet(
        QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:8px;font-family:'Courier New';")
            .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    root->addWidget(aa_round_);

    aa_table_ = new QTableWidget(page);
    aa_table_->setObjectName(QStringLiteral("nbLabTable"));
    aa_table_->setColumnCount(8);
    aa_table_->setHorizontalHeaderLabels({tr("RANK"), tr("MODEL"), tr("BRIER"), tr("95% CI"),
                                          tr("COVERAGE"), tr("RESOLVED"), tr("COMMIT / OFFERED"),
                                          tr("STATUS")});
    aa_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    aa_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    aa_table_->verticalHeader()->hide();
    aa_table_->horizontalHeader()->setStretchLastSection(true);
    root->addWidget(aa_table_, 1);

    aa_howto_ = new QLabel(page);
    aa_howto_->setWordWrap(true);
    aa_howto_->setObjectName(QStringLiteral("nbLabSummary"));
    root->addWidget(aa_howto_);

    aa_timer_ = new QTimer(this);
    aa_timer_->setInterval(5000);
    connect(aa_timer_, &QTimer::timeout, this, &CodeEditorScreen::refresh_alpha_arena);
    aa_timer_->start();
    refresh_alpha_arena();
    return page;
}

void CodeEditorScreen::refresh_alpha_arena() {
    if (!aa_verdict_ || !aa_table_) return;
    const QJsonObject report =
        read_json_object(alpha_arena_root() + QStringLiteral("/arena-report.json"));

    const QString verdict =
        report.value(QStringLiteral("verdict")).toString(QStringLiteral("INSUFFICIENT_DATA"));
    const int rounds = report.value(QStringLiteral("rounds_total")).toInt();
    const bool has_leader = verdict.startsWith(QStringLiteral("LEADER"));
    QString banner;
    if (report.isEmpty())
        banner = tr("ARENA OFFLINE · no arena-report.json yet — is the arena loop running?");
    else if (rounds == 0)
        banner = tr("ARENA WARMING UP · no rounds completed yet");
    else if (has_leader)
        banner = tr("%1 · %2 rounds · confidence intervals separated").arg(verdict).arg(rounds);
    else
        banner = tr("%1 · %2 rounds so far · no leader until the statistics say so")
                     .arg(verdict).arg(rounds);
    aa_verdict_->setText(banner);
    aa_verdict_->setStyleSheet(
        QStringLiteral("color:%1;background:%2;border:2px solid %1;padding:10px;font-weight:900;font-size:15px;")
            .arg(has_leader ? colors::POSITIVE() : colors::AMBER(), colors::BG_RAISED()));

    const QJsonObject round =
        read_last_round(alpha_arena_root() + QStringLiteral("/arena-rounds.jsonl"));
    if (round.isEmpty()) {
        aa_round_->setText(tr("LIVE ROUND · none yet"));
    } else {
        QStringList lane_bits;
        for (const auto& value : round.value(QStringLiteral("lanes")).toArray()) {
            const auto lane = value.toObject();
            const QString status = lane.value(QStringLiteral("status")).toString();
            const QString dot = status == QStringLiteral("COMMITTED_BLIND")
                ? QStringLiteral("●") : (status == QStringLiteral("ABSTAINED")
                ? QStringLiteral("◐") : QStringLiteral("○"));
            lane_bits << QStringLiteral("%1 %2 %3")
                             .arg(dot, lane.value(QStringLiteral("id")).toString(), status);
        }
        aa_round_->setText(tr("LATEST ROUND · %1 · %2\n%3")
            .arg(round.value(QStringLiteral("ticker")).toString(QStringLiteral("--")),
                 round.value(QStringLiteral("status")).toString(),
                 lane_bits.join(QStringLiteral("   "))));
    }

    const QJsonArray leaderboard = report.value(QStringLiteral("leaderboard")).toArray();
    aa_table_->setRowCount(leaderboard.size());
    for (int i = 0; i < leaderboard.size(); ++i) {
        const auto e = leaderboard.at(i).toObject();
        const bool comparable = e.value(QStringLiteral("comparable")).toBool();
        const QJsonArray ci = e.value(QStringLiteral("brier_ci")).toArray();
        const QJsonValue cov = e.value(QStringLiteral("coverage"));
        const QStringList values{
            comparable ? QString::number(e.value(QStringLiteral("rank")).toInt())
                       : QStringLiteral("--"),
            e.value(QStringLiteral("model")).toString(),
            fmt_brier(e.value(QStringLiteral("brier"))),
            (ci.size() == 2 && ci.at(0).isDouble())
                ? QStringLiteral("[%1, %2]").arg(ci.at(0).toDouble(), 0, 'f', 4)
                                            .arg(ci.at(1).toDouble(), 0, 'f', 4)
                : QStringLiteral("--"),
            cov.isDouble() ? QStringLiteral("%1%").arg(cov.toDouble() * 100.0, 0, 'f', 0)
                           : QStringLiteral("--"),
            QString::number(e.value(QStringLiteral("resolved")).toInt()),
            QStringLiteral("%1 / %2").arg(e.value(QStringLiteral("committed")).toInt())
                                     .arg(e.value(QStringLiteral("offered")).toInt()),
            comparable ? tr("RANKED")
                       : e.value(QStringLiteral("reason")).toString(tr("NOT COMPARABLE"))};
        for (int col = 0; col < values.size(); ++col) {
            auto* item = new QTableWidgetItem(values.at(col));
            if (!comparable)
                item->setForeground(QColor(colors::TEXT_TERTIARY()));
            aa_table_->setItem(i, col, item);
        }
    }

    aa_howto_->setText(report.value(QStringLiteral("how_it_works")).toString(
        tr("Every model sees the same facts, never the market's odds. Each seals its "
           "probability before anyone can peek. Kalshi settles the truth within the hour. "
           "Lower Brier = better forecaster. Skipping too many rounds voids your ranking.")));
}

} // namespace openmarketterminal::screens
