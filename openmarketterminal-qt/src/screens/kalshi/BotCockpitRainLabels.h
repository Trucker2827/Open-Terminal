#pragma once

// Human-readable labels for the BOT COCKPIT rain columns. `column_head()` in
// KalshiBotCockpitView.cpp used to show a raw ticker segment, so a 15-minute
// contract rendered as `26JUL311730-…` — unreadable. Split out as a pure
// string helper so it is unit-testable without pulling in QWidget/QPainter.
//
// This changes only the LABEL text: the rain still draws the same live
// calibrator contracts it always did.

#include <QString>
#include <QStringList>

namespace openmarketterminal::screens::kalshi {

namespace bot_cockpit_detail {

/// The previous behavior: enough of the ticker to identify the contract in a
/// column that is only a few characters wide. `KXBTCD-26JUL2521-T64299.99`
/// identifies itself by its strike, but `KXBTC15M-26JUL230800-00` ends in a
/// bare `00`, so the last segment alone would label two different columns the
/// same. A second segment is taken whenever the last one is that short. Kept
/// as the fallback for any ticker family `bot_cockpit_column_head` does not
/// recognize.
inline QString bot_cockpit_column_head_fallback(const QString& ticker) {
    const QString last = ticker.section(QLatin1Char('-'), -1);
    if (last.isEmpty()) return ticker;
    if (last.size() >= 4) return last;
    const QString previous = ticker.section(QLatin1Char('-'), -2, -2);
    return previous.isEmpty() ? last : previous + QLatin1Char('-') + last;
}

} // namespace bot_cockpit_detail

/// The head of a rain column, human-readable rather than a raw ticker
/// segment:
///  - a KXBTC15M contract shows its own close time HH:MM, parsed out of the
///    ticker's YYMMMDDHHMM segment (`26JUL311730` -> `17:30`);
///  - a KXBTCD threshold contract shows its strike compactly
///    (`T62899.99` -> `$62.9k`);
///  - anything unrecognized falls back to the previous segment-based label,
///    so an unfamiliar ticker still identifies itself.
inline QString bot_cockpit_column_head(const QString& ticker) {
    const QStringList parts = ticker.split(QLatin1Char('-'));
    if (parts.size() >= 2) {
        const QString family = parts.first();
        if (family.startsWith(QStringLiteral("KXBTC15M"))) {
            const QString time_segment = parts.at(1);
            if (time_segment.size() == 11) {
                static const QStringList kMonths = {
                    QStringLiteral("JAN"), QStringLiteral("FEB"), QStringLiteral("MAR"),
                    QStringLiteral("APR"), QStringLiteral("MAY"), QStringLiteral("JUN"),
                    QStringLiteral("JUL"), QStringLiteral("AUG"), QStringLiteral("SEP"),
                    QStringLiteral("OCT"), QStringLiteral("NOV"), QStringLiteral("DEC")};
                const QString month = time_segment.mid(2, 3).toUpper();
                if (kMonths.contains(month)) {
                    const QString hour = time_segment.mid(7, 2);
                    const QString minute = time_segment.mid(9, 2);
                    bool hour_ok = false;
                    bool minute_ok = false;
                    hour.toInt(&hour_ok);
                    minute.toInt(&minute_ok);
                    if (hour_ok && minute_ok)
                        return QStringLiteral("%1:%2").arg(hour, minute);
                }
            }
        } else if (family == QStringLiteral("KXBTCD")) {
            const QString strike_segment = parts.last();
            if (strike_segment.startsWith(QLatin1Char('T'))) {
                bool ok = false;
                const double strike = strike_segment.mid(1).toDouble(&ok);
                if (ok)
                    return QStringLiteral("$%1k").arg(QString::number(strike / 1000.0, 'f', 1));
            }
        }
    }
    return bot_cockpit_detail::bot_cockpit_column_head_fallback(ticker);
}

} // namespace openmarketterminal::screens::kalshi
