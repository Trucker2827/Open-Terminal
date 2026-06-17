// src/screens/ai_chat/SessionTitle.cpp
//
// Pure session-title helpers (see SessionTitle.h). No GUI dependencies.

#include "screens/ai_chat/SessionTitle.h"

#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringList>

namespace openmarketterminal::ai_chat {

// The word pools below are the single source of truth for the placeholder
// format. is_auto_generated_title()'s regex is built from the SAME lists, so a
// title produced here always tests positive — tests/tst_session_title.cpp pins
// that invariant.
static const QStringList& title_prefixes() {
    static const QStringList kPrefixes = {"Amber", "Apex",  "Atlas", "Echo",
                                          "Flux",  "Nova",  "Slate", "Vector"};
    return kPrefixes;
}

static const QStringList& title_nouns() {
    static const QStringList kNouns = {"Brief", "Drift",  "Focus",  "Ledger",
                                       "Macro", "Pulse",  "Signal", "Tape"};
    return kNouns;
}

QString generate_session_title() {
    const QStringList& prefixes = title_prefixes();
    const QStringList& nouns = title_nouns();
    const int pi = QRandomGenerator::global()->bounded(static_cast<int>(prefixes.size()));
    const int ni = QRandomGenerator::global()->bounded(static_cast<int>(nouns.size()));
    const QString sfx = QString::number(QRandomGenerator::global()->bounded(0x10000), 16)
                            .rightJustified(4, '0')
                            .toUpper();
    return QString("%1 %2 %3").arg(prefixes[pi], nouns[ni], sfx);
}

bool is_auto_generated_title(const QString& title) {
    static const QRegularExpression re(
        QStringLiteral("^(%1) (%2) [0-9A-F]{4}$")
            .arg(title_prefixes().join('|'), title_nouns().join('|')));
    return re.match(title.trimmed()).hasMatch();
}

QString derive_session_title_from_message(const QString& msg, int max_len) {
    // simplified(): trims ends and collapses every internal whitespace run
    // (incl. newlines/tabs) to a single space.
    QString s = msg.simplified();
    if (s.isEmpty())
        return {};
    if (max_len < 1 || s.size() <= max_len)
        return s;

    // Back off to the last space at/before max_len so we don't cut mid-word —
    // but only if that leaves a reasonable amount of text (else hard-cut).
    int cut = max_len;
    const int sp = static_cast<int>(s.lastIndexOf(QLatin1Char(' '), cut));
    if (sp >= max_len / 2)
        cut = sp;
    return s.left(cut).trimmed() + QStringLiteral("…");
}

} // namespace openmarketterminal::ai_chat
