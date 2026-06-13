#pragma once
#include "screens/dashboard/widgets/QuoteTableWidget.h"

#include <QCoreApplication>

namespace openmarketterminal::screens::widgets {

/// Global indices widget — fetches the major US/global indices via yfinance.
inline QuoteTableWidget* create_indices_widget(QWidget* parent = nullptr) {
    QMap<QString, QString> labels = {
        {"^GSPC", "S&P 500"},  {"^DJI", "DOW JONES"}, {"^IXIC", "NASDAQ"}, {"^RUT", "RUSSELL 2000"},
        {"^FTSE", "FTSE 100"}, {"^GDAXI", "DAX"},     {"^FCHI", "CAC 40"}, {"^N225", "NIKKEI 225"},
        {"^HSI", "HANG SENG"}, {"000001.SS", "SHANGHAI"},
    };
    const QString title = QCoreApplication::translate("openmarketterminal::screens::widgets::QuoteTableWidget", "GLOBAL INDICES");
    return new QuoteTableWidget(title, services::MarketDataService::indices_symbols(), labels, 2, {},
                                parent);
}

} // namespace openmarketterminal::screens::widgets
