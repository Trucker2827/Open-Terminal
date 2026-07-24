// src/screens/equity_research/EquityResearchNote.h
//
// Issue #103 — composition of the "NOTE" action's draft on the Equity
// Research screen. Pure functions only: the screen feeds in what the quote
// bar currently shows and stores the result via NotesRepository. Kept free
// of repository/storage headers so tst_equity_research_note links against
// Qt6::Core alone.
#pragma once
#include <QDate>
#include <QRegularExpression>
#include <QString>

namespace openmarketterminal::screens::equity_note {

// Snapshot of the quote bar at the moment the NOTE action fires. `loaded` is
// true only when a real quote for the current symbol has arrived — the caller
// must never guess values. Honesty rule: no quote → no snapshot line in the
// note body, never a zero-filled one.
struct ResearchNoteQuote {
    bool loaded = false;
    double price = 0.0;
    double change = 0.0;
    double change_pct = 0.0;
    QString currency_symbol; // as rendered in the quote bar, e.g. "$", "€"
};

// The composed draft handed to NotesRepository::create().
struct ResearchNoteDraft {
    QString title;
    QString content;
    QString tickers;
    int word_count = 0;
};

inline ResearchNoteDraft compose_research_note(const QString& symbol,
                                               const ResearchNoteQuote& quote,
                                               const QDate& date) {
    ResearchNoteDraft d;
    const QString sym = symbol.trimmed().toUpper();
    d.title = QStringLiteral("%1 — Research Note %2").arg(sym, date.toString(Qt::ISODate));
    d.tickers = sym;

    if (quote.loaded) {
        // Mirror the quote bar: price to 2dp with currency symbol, signed
        // change and percent — the snapshot records what the user saw.
        const QString sign = quote.change >= 0.0 ? QStringLiteral("+") : QString();
        const QString pct_sign = quote.change_pct >= 0.0 ? QStringLiteral("+") : QString();
        d.content = QStringLiteral("%1 — %2%3  %4%5 (%6%7%)\n")
                        .arg(sym, quote.currency_symbol, QString::number(quote.price, 'f', 2),
                             sign, QString::number(quote.change, 'f', 2), pct_sign,
                             QString::number(quote.change_pct, 'f', 2));
    }

    d.word_count = static_cast<int>(
        d.content.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size());
    return d;
}

} // namespace openmarketterminal::screens::equity_note
