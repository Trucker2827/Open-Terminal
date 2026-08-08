#pragma once

// Human-readable labels for the BOT COCKPIT rain columns. `column_head()` in
// KalshiBotCockpitView.cpp used to show a raw ticker segment, so a 15-minute
// contract rendered as `26JUL311730-…` — unreadable. Split out as a pure
// string helper so it is unit-testable without pulling in QWidget/QPainter.
//
// This changes only the LABEL text: the rain still draws the same live
// calibrator contracts it always did.

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QTimeZone>

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

/// Close instant (ms) for a *15M ticker from its YYMONDDHHMM segment, US/Eastern.
/// Matches `kxbtc15m_calibrator.parse_close_ms`. -1 when unparseable / not 15m.
inline qint64 bot_cockpit_15m_close_ms(const QString& ticker) {
    const QStringList parts = ticker.split(QLatin1Char('-'));
    if (parts.size() < 2) return -1;
    if (!parts.first().endsWith(QStringLiteral("15M"))) return -1;
    const QString token = parts.at(1);
    if (token.size() != 11) return -1;
    static const QStringList kMonths = {
        QStringLiteral("JAN"), QStringLiteral("FEB"), QStringLiteral("MAR"),
        QStringLiteral("APR"), QStringLiteral("MAY"), QStringLiteral("JUN"),
        QStringLiteral("JUL"), QStringLiteral("AUG"), QStringLiteral("SEP"),
        QStringLiteral("OCT"), QStringLiteral("NOV"), QStringLiteral("DEC")};
    const int month = kMonths.indexOf(token.mid(2, 3).toUpper()) + 1;
    if (month <= 0) return -1;
    bool ok = false;
    const int yy = token.mid(0, 2).toInt(&ok);
    if (!ok) return -1;
    const int dd = token.mid(5, 2).toInt(&ok);
    if (!ok) return -1;
    const int hh = token.mid(7, 2).toInt(&ok);
    if (!ok) return -1;
    const int mm = token.mid(9, 2).toInt(&ok);
    if (!ok) return -1;
    const QDate date(2000 + yy, month, dd);
    const QTime time(hh, mm);
    if (!date.isValid() || !time.isValid()) return -1;
    const QDateTime dt(date, time, QTimeZone(QByteArrayLiteral("America/New_York")));
    return dt.isValid() ? dt.toMSecsSinceEpoch() : -1;
}

/// True when a 15m race is still open (or the ticker is not a parseable 15m).
/// Closed 15m windows must not paint as current FLOW.
inline bool bot_cockpit_15m_still_open(const QString& ticker, qint64 now_ms) {
    const qint64 close_ms = bot_cockpit_15m_close_ms(ticker);
    if (close_ms <= 0) return true;
    return now_ms < close_ms;
}

} // namespace bot_cockpit_detail

/// The head of a rain column, human-readable rather than a raw ticker
/// segment:
///  - a *15M race (KXBTC15M / KXGOLD15M / KXSILVER15M / KXWTI15M) shows its
///    close time HH:MM from the ticker's YYMMMDDHHMM segment
///    (`26JUL311730` -> `17:30`);
///  - a KXBTCD threshold contract shows its strike compactly
///    (`T62899.99` -> `$62.9k`);
///  - anything unrecognized falls back to the previous segment-based label,
///    so an unfamiliar ticker still identifies itself.
inline QString bot_cockpit_column_head(const QString& ticker) {
    const QStringList parts = ticker.split(QLatin1Char('-'));
    if (parts.size() >= 2) {
        const QString family = parts.first();
        if (family.endsWith(QStringLiteral("15M"))) {
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

using bot_cockpit_detail::bot_cockpit_15m_close_ms;
using bot_cockpit_detail::bot_cockpit_15m_still_open;

} // namespace openmarketterminal::screens::kalshi
