#include "screens/code_editor/CodeEditorScreen.h"

#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace openmarketterminal::screens {
using namespace openmarketterminal::ui;

namespace {
QString arena_root() {
    return QDir::homePath() + QStringLiteral("/Library/Application Support/org.openterminal.OpenTerminal/daemon");
}

QJsonObject read_object(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

QLabel* arena_metric(QWidget* parent, QHBoxLayout* row, const QString& caption, QLabel*& value) {
    auto* panel = new QWidget(parent);
    panel->setObjectName(QStringLiteral("nbMetricPanel"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 8, 12, 8);
    value = new QLabel(QStringLiteral("--"), panel);
    value->setObjectName(QStringLiteral("nbMetricValue"));
    auto* label = new QLabel(caption, panel);
    label->setObjectName(QStringLiteral("nbMetricLabel"));
    layout->addWidget(value);
    layout->addWidget(label);
    row->addWidget(panel, 1);
    return value;
}

QString pct(double value) { return QStringLiteral("%1%").arg(value * 100.0, 0, 'f', 1); }
QString probability(double value) { return QStringLiteral("%1%").arg(value * 100.0, 0, 'f', 1); }
}

QWidget* CodeEditorScreen::build_forecast_arena_page() {
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("nbLabPage"));
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(14, 10, 14, 12);
    root->setSpacing(8);

    auto* title = new QLabel(tr("CLAUDE vs CODEX · LIVE SHADOW DUEL"), page);
    title->setObjectName(QStringLiteral("nbLabTitle"));
    root->addWidget(title);
    auto* safety = new QLabel(tr("SHADOW ONLY · NO ORDERS · NO EXECUTION AUTHORITY · READ-ONLY JOURNAL MIRROR"), page);
    safety->setStyleSheet(QStringLiteral("color:%1;border:1px solid %1;padding:7px;font-weight:900;letter-spacing:1px;")
                              .arg(colors::WARNING()));
    root->addWidget(safety);
    arena_state_ = new QLabel(page);
    arena_state_->setWordWrap(true);
    arena_state_->setMinimumHeight(48);
    root->addWidget(arena_state_);

    auto* metrics = new QWidget(page);
    auto* metric_row = new QHBoxLayout(metrics);
    metric_row->setContentsMargins(0, 0, 0, 0);
    metric_row->setSpacing(0);
    arena_metric(metrics, metric_row, tr("JOINTLY RESOLVED"), arena_progress_);
    arena_metric(metrics, metric_row, tr("PAIRED BRIER Δ · CLAUDE−CODEX"), arena_delta_);
    arena_metric(metrics, metric_row, tr("CLAUDE COVERAGE / ABSTENTION"), arena_claude_coverage_);
    arena_metric(metrics, metric_row, tr("CODEX COVERAGE / ABSTENTION"), arena_codex_coverage_);
    root->addWidget(metrics);

    arena_integrity_ = new QLabel(page);
    arena_integrity_->setWordWrap(true);
    arena_integrity_->setStyleSheet(QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:8px;")
                                        .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    root->addWidget(arena_integrity_);

    arena_table_ = new QTableWidget(page);
    arena_table_->setObjectName(QStringLiteral("nbLabTable"));
    arena_table_->setColumnCount(9);
    arena_table_->setHorizontalHeaderLabels({tr("OPPORTUNITY"), tr("CONTRACT"), tr("OUTCOME"),
        tr("CLAUDE P(YES)"), tr("CODEX P(YES)"), tr("CLAUDE BRIER"), tr("CODEX BRIER"),
        tr("FORECAST WINNER"), tr("CONTEXT HASH")});
    arena_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    arena_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    arena_table_->verticalHeader()->hide();
    arena_table_->horizontalHeader()->setStretchLastSection(true);
    connect(arena_table_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { open_forecast_comparison(row); });
    root->addWidget(arena_table_, 1);

    auto* note = new QLabel(tr("HYPOTHETICAL / COUNTERFACTUAL VALUE ONLY · no execution occurred. Double-click any forecast for its audit card."), page);
    note->setObjectName(QStringLiteral("nbLabSummary"));
    root->addWidget(note);

    arena_timer_ = new QTimer(this);
    arena_timer_->setInterval(5000);
    connect(arena_timer_, &QTimer::timeout, this, &CodeEditorScreen::refresh_forecast_arena);
    arena_timer_->start();
    refresh_forecast_arena();
    return page;
}

void CodeEditorScreen::refresh_forecast_arena() {
    if (!arena_state_ || !arena_table_) return;
    const QJsonObject report = read_object(arena_root() + QStringLiteral("/advisor_competition_report.json"));
    const QString state = report.value(QStringLiteral("result_state")).toString(
        QStringLiteral("INSUFFICIENT_PAIRED_DATA"));
    const int resolved = report.value(QStringLiteral("jointly_resolved")).toInt();
    const int opportunities = report.value(QStringLiteral("opportunities")).toInt();
    const bool invalid = state == QStringLiteral("INVALID_EPOCH");
    QString banner;
    if (invalid) {
        QStringList reasons;
        for (const auto& value : report.value(QStringLiteral("invalid_reasons")).toArray())
            reasons.append(value.toString());
        banner = tr("INVALID EPOCH · LEADER SUPPRESSED · %1").arg(reasons.join(QStringLiteral(", ")));
    }
    else if (state == QStringLiteral("CLAUDE_WINS") || state == QStringLiteral("CODEX_WINS"))
        banner = tr("MECHANICAL RESULT · %1").arg(state);
    else if (resolved == 0)
        banner = tr("ARENA INITIALIZING · 0/200 JOINTLY RESOLVED · NO WINNER YET");
    else
        banner = tr("%1 · PROVISIONAL · NOT YET DECISIVE").arg(state);
    arena_state_->setText(banner);
    arena_state_->setStyleSheet(QStringLiteral("color:%1;background:%2;border:2px solid %1;padding:10px;font-weight:900;font-size:15px;")
                                    .arg(invalid ? colors::NEGATIVE() : colors::AMBER(), colors::BG_RAISED()));
    arena_progress_->setText(QStringLiteral("%1 / 200").arg(resolved));
    const double delta = report.value(QStringLiteral("paired_brier_delta_claude_minus_codex")).toDouble();
    arena_delta_->setText(QStringLiteral("%1 · CI [%2, %3]").arg(delta, 0, 'f', 5)
        .arg(report.value(QStringLiteral("ci_low")).toDouble(), 0, 'f', 5)
        .arg(report.value(QStringLiteral("ci_high")).toDouble(), 0, 'f', 5));
    const auto coverage = report.value(QStringLiteral("coverage")).toObject();
    const auto rates = report.value(QStringLiteral("rates")).toObject();
    const auto lane_text = [&](const QString& lane) {
        const auto counts = rates.value(lane).toObject();
        return QStringLiteral("%1 · %2 abstained").arg(pct(coverage.value(lane).toDouble()))
            .arg(counts.value(QStringLiteral("abstained")).toInt());
    };
    arena_claude_coverage_->setText(lane_text(QStringLiteral("claude")));
    arena_codex_coverage_->setText(lane_text(QStringLiteral("codex")));
    const auto epochs = report.value(QStringLiteral("epoch_pair")).toObject();
    arena_integrity_->setText(tr("EPOCH PAIR  %1  ↔  %2\nFIREWALL %3 · %4 opportunities · prompt/context hashes enforced")
        .arg(epochs.value(QStringLiteral("claude")).toString(QStringLiteral("kalshi-blind-claude-cli-v2")),
             epochs.value(QStringLiteral("codex")).toString(QStringLiteral("kalshi-blind-codex-v3-zero-capability")),
             report.value(QStringLiteral("firewall_safe")).toBool(false) ? QStringLiteral("LOCKED") : QStringLiteral("NOT VERIFIED"))
        .arg(opportunities));

    const QJsonArray rows = report.value(QStringLiteral("forecasts")).toArray();
    arena_table_->setRowCount(rows.size());
    for (int index = 0; index < rows.size(); ++index) {
        const auto row = rows.at(rows.size() - 1 - index).toObject();
        const bool is_resolved = row.contains(QStringLiteral("outcome"));
        const double cb = row.value(QStringLiteral("claude_brier")).toDouble();
        const double ob = row.value(QStringLiteral("codex_brier")).toDouble();
        const auto forecast_value = [&](const QString& key, const QString& status_key) {
            return row.value(key).isDouble() ? probability(row.value(key).toDouble())
                                             : row.value(status_key).toString(QStringLiteral("ABSTAINED"));
        };
        const QStringList values{row.value(QStringLiteral("competition_pair_id")).toString().left(8),
            row.value(QStringLiteral("ticker")).toString(), is_resolved ? QString::number(row.value(QStringLiteral("outcome")).toInt()) : QStringLiteral("UNRESOLVED"),
            forecast_value(QStringLiteral("claude_probability"), QStringLiteral("claude_status")),
            forecast_value(QStringLiteral("codex_probability"), QStringLiteral("codex_status")),
            is_resolved ? QString::number(cb, 'f', 5) : QStringLiteral("--"),
            is_resolved ? QString::number(ob, 'f', 5) : QStringLiteral("--"),
            is_resolved ? row.value(QStringLiteral("forecast_winner")).toString() : QStringLiteral("PENDING"),
            row.value(QStringLiteral("context_hash")).toString().left(16)};
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, row);
            arena_table_->setItem(index, column, item);
        }
    }
}

void CodeEditorScreen::open_forecast_comparison(int row) {
    if (!arena_table_ || row < 0 || !arena_table_->item(row, 0)) return;
    const QJsonObject forecast = arena_table_->item(row, 0)->data(Qt::UserRole).toJsonObject();
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Blind Forecast Comparison · Read Only"));
    dialog->resize(820, 620);
    auto* layout = new QVBoxLayout(dialog);
    auto* banner = new QLabel(tr("SHADOW FORECAST AUDIT · NO ORDERS · NO EXECUTION AUTHORITY"), dialog);
    banner->setStyleSheet(QStringLiteral("color:%1;font-weight:900;padding:8px;border:1px solid %1;").arg(colors::WARNING()));
    layout->addWidget(banner);
    auto* details = new QPlainTextEdit(dialog);
    details->setReadOnly(true);
    details->setPlainText(QString::fromUtf8(QJsonDocument(forecast).toJson(QJsonDocument::Indented)));
    layout->addWidget(details, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}

} // namespace openmarketterminal::screens
