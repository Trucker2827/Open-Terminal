#include "screens/weather/WeatherScreen.h"

#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace openmarketterminal::screens {

namespace pred = openmarketterminal::services::prediction;

namespace {

static const char* MF = "font-family:'Consolas',monospace;";

QString table_style() {
    return QString("QTableWidget{background:%1;color:%2;border:none;gridline-color:%3;font-size:11px;%4}"
                   "QHeaderView::section{background:%5;color:%6;border:1px solid %3;padding:4px;"
                   "font-size:10px;font-weight:700;%4}"
                   "QTableWidget::item{padding:3px 6px;border-bottom:1px solid %3;}")
        .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
             ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY());
}

QTableWidgetItem* cell(const QString& text, const QString& color = {}) {
    auto* it = new QTableWidgetItem(text);
    if (!color.isEmpty())
        it->setForeground(QColor(color));
    return it;
}

double outcome_price(const pred::PredictionMarket& market, int index) {
    return index >= 0 && index < market.outcomes.size() ? market.outcomes[index].price : 0.0;
}

/// City abbreviation from the series ticker prefix (e.g. "KXHIGHNY" for the
/// NYC daily-high-temperature series). Matches the city keys weather_producer
/// .py's WEATHER_CITIES table uses. Unknown prefixes fall back to the raw
/// series ticker so a new/unmapped city still shows something identifiable
/// rather than a blank cell.
QString city_for_series(const QString& series_ticker) {
    static const QHash<QString, QString> kKnownSeries = {
        {QStringLiteral("KXHIGHNY"), QStringLiteral("NYC")},
        {QStringLiteral("KXHIGHCHI"), QStringLiteral("CHI")},
        {QStringLiteral("KXHIGHDEN"), QStringLiteral("DEN")},
        {QStringLiteral("KXHIGHTSFO"), QStringLiteral("SFO")},
        {QStringLiteral("KXHIGHPHIL"), QStringLiteral("PHIL")},
        {QStringLiteral("KXHIGHTSEA"), QStringLiteral("SEA")},
    };
    return kKnownSeries.value(series_ticker, series_ticker);
}

} // namespace

WeatherScreen::WeatherScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    wire_adapter();
}

void WeatherScreen::build_ui() {
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    auto* title = new QLabel(tr("KALSHI WEATHER — daily city high-temp brackets"));
    title->setStyleSheet(QString("color:%1;font-size:15px;font-weight:800;background:transparent;%2")
                             .arg(ui::colors::AMBER(), MF));
    root->addWidget(title);

    auto* subtitle = new QLabel(tr("Every evaluated bracket across the weather cities. "
                                   "Separate from Bitcoin predictions; both run in parallel."));
    subtitle->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;%2")
                                .arg(ui::colors::TEXT_TERTIARY(), MF));
    root->addWidget(subtitle);

    auto* header_row = new QHBoxLayout;
    status_label_ = new QLabel(QStringLiteral("-"));
    status_label_->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;%2")
                                     .arg(ui::colors::TEXT_SECONDARY(), MF));
    header_row->addWidget(status_label_);
    header_row->addStretch();
    count_label_ = new QLabel(QStringLiteral("0 BRACKETS"));
    count_label_->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                    .arg(ui::colors::CYAN(), MF));
    header_row->addWidget(count_label_);
    root->addLayout(header_row);

    auto* browser_hdr = new QLabel(tr("BRACKET BROWSER"));
    browser_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                                   .arg(ui::colors::CYAN(), MF));
    root->addWidget(browser_hdr);

    bracket_table_ = new QTableWidget(0, 4, this);
    bracket_table_->setHorizontalHeaderLabels({tr("City"), tr("Bracket"), tr("Mkt (mid)"), tr("Vol")});
    bracket_table_->setStyleSheet(table_style());
    bracket_table_->verticalHeader()->setVisible(false);
    bracket_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bracket_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    bracket_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    bracket_table_->horizontalHeader()->setStretchLastSection(true);
    bracket_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    root->addWidget(bracket_table_, 1);
}

pred::PredictionExchangeAdapter* WeatherScreen::adapter() const {
    return pred::PredictionExchangeRegistry::instance().adapter(QStringLiteral("kalshi"));
}

void WeatherScreen::wire_adapter() {
    auto* a = adapter();
    if (!a) {
        if (status_label_)
            status_label_->setText(tr("Adapter offline — Kalshi weather markets unavailable."));
        return;
    }
    connect(a, &pred::PredictionExchangeAdapter::events_ready, this, &WeatherScreen::populate_events);
    connect(a, &pred::PredictionExchangeAdapter::markets_ready, this, &WeatherScreen::populate_markets);
}

void WeatherScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (first_show_) {
        first_show_ = false;
        refresh();
    }
}

void WeatherScreen::refresh() {
    if (status_label_)
        status_label_->setText(tr("Loading weather brackets…"));
    if (auto* a = adapter())
        a->list_events(QStringLiteral("Weather"), QStringLiteral("volume"), 200, 0);
}

void WeatherScreen::populate_events(const QVector<pred::PredictionEvent>& events) {
    QVector<pred::PredictionMarket> flattened;
    for (const auto& event : events) {
        for (auto market : event.markets) {
            if (market.question.trimmed().isEmpty())
                market.question = event.title;
            market.extras.insert(QStringLiteral("event_title"), event.title);
            flattened.push_back(market);
        }
    }
    populate_markets(flattened);
}

void WeatherScreen::populate_markets(const QVector<pred::PredictionMarket>& markets) {
    if (!bracket_table_)
        return;

    QVector<pred::PredictionMarket> sorted = markets;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left.volume > right.volume;
    });

    bracket_table_->setRowCount(0);
    int row = 0;
    for (const auto& market : sorted) {
        const QString series_ticker = market.extras.value(QStringLiteral("series_ticker")).toString();
        const QString city = city_for_series(series_ticker);
        const QString bracket_label = market.question.isEmpty()
                                          ? market.extras.value(QStringLiteral("event_title")).toString()
                                          : market.question;
        const double mid = outcome_price(market, 0);

        bracket_table_->insertRow(row);
        bracket_table_->setItem(row, 0, cell(city));
        bracket_table_->setItem(row, 1, cell(bracket_label));
        bracket_table_->setItem(row, 2, cell(QString::asprintf("%.2f", mid)));
        bracket_table_->setItem(row, 3, cell(QString::asprintf("%.0f", market.volume)));
        ++row;
    }

    if (count_label_)
        count_label_->setText(QStringLiteral("%1 BRACKETS").arg(sorted.size()));
    if (status_label_)
        status_label_->setText(sorted.isEmpty()
                                   ? tr("No weather brackets returned.")
                                   : tr("Weather brackets loaded — %1 evaluated.").arg(sorted.size()));
}

} // namespace openmarketterminal::screens
