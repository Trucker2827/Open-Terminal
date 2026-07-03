#include "ui/command/CommandParser.h"

#include "core/actions/ActionRegistry.h"

#include <QRegularExpression>
#include <QHash>
#include <QStringList>

namespace openmarketterminal::ui {

namespace {

/// Splits on whitespace but preserves double-quoted strings as single tokens.
QStringList tokenise(const QString& s) {
    QStringList out;
    QString cur;
    bool in_quotes = false;
    for (QChar c : s) {
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (c.isSpace() && !in_quotes) {
            if (!cur.isEmpty()) {
                out.append(cur);
                cur.clear();
            }
        } else {
            cur.append(c);
        }
    }
    if (!cur.isEmpty())
        out.append(cur);
    return out;
}

QString action_for_function_code(const QString& upper) {
    static const QHash<QString, QString> kCodes = {
        {QStringLiteral("HOME"), QStringLiteral("screen.dashboard")},
        {QStringLiteral("DASH"), QStringLiteral("screen.dashboard")},
        {QStringLiteral("MKT"), QStringLiteral("screen.markets")},
        {QStringLiteral("MKTS"), QStringLiteral("screen.markets")},
        {QStringLiteral("WL"), QStringLiteral("screen.watchlist")},
        {QStringLiteral("NEWS"), QStringLiteral("screen.news")},
        {QStringLiteral("TOP"), QStringLiteral("screen.news")},
        {QStringLiteral("PORT"), QStringLiteral("screen.portfolio")},
        {QStringLiteral("PORTF"), QStringLiteral("screen.portfolio")},
        {QStringLiteral("EQ"), QStringLiteral("screen.equity_research")},
        {QStringLiteral("DES"), QStringLiteral("screen.equity_research")},
        {QStringLiteral("FA"), QStringLiteral("screen.equity_research")},
        {QStringLiteral("ECON"), QStringLiteral("screen.economics")},
        {QStringLiteral("ECST"), QStringLiteral("screen.economics")},
        {QStringLiteral("FRED"), QStringLiteral("screen.dbnomics")},
        {QStringLiteral("GOV"), QStringLiteral("screen.gov_data")},
        {QStringLiteral("API"), QStringLiteral("screen.data_sources")},
        {QStringLiteral("DATA"), QStringLiteral("screen.data_sources")},
        {QStringLiteral("TRADE"), QStringLiteral("screen.equity_trading")},
        {QStringLiteral("BTC"), QStringLiteral("screen.bitcoin")},
        {QStringLiteral("XBT"), QStringLiteral("screen.bitcoin")},
        {QStringLiteral("BITCOIN"), QStringLiteral("screen.bitcoin")},
        {QStringLiteral("OPT"), QStringLiteral("screen.derivatives")},
        {QStringLiteral("BT"), QStringLiteral("screen.backtesting")},
        {QStringLiteral("ALGO"), QStringLiteral("screen.algo_trading")},
        {QStringLiteral("QUANT"), QStringLiteral("screen.quantlib")},
        {QStringLiteral("AI"), QStringLiteral("screen.ai_chat")},
        {QStringLiteral("GEO"), QStringLiteral("screen.geopolitics")},
        {QStringLiteral("SHIP"), QStringLiteral("screen.maritime")},
        {QStringLiteral("PMKT"), QStringLiteral("screen.polymarket")},
        {QStringLiteral("MAP"), QStringLiteral("screen.relationship_map")},
        {QStringLiteral("XL"), QStringLiteral("screen.excel")},
        {QStringLiteral("CODE"), QStringLiteral("screen.code_editor")},
        {QStringLiteral("NODE"), QStringLiteral("screen.node_editor")},
        {QStringLiteral("HELP"), QStringLiteral("screen.docs")},
        {QStringLiteral("DOCS"), QStringLiteral("screen.docs")},
        {QStringLiteral("SET"), QStringLiteral("screen.settings")},
    };
    return kCodes.value(upper);
}

} // namespace

ParsedCommand CommandParser::parse(const QString& input) {
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        ParsedCommand p;
        p.kind = ParsedCommand::Kind::Empty;
        return p;
    }
    if (trimmed == QLatin1String("?")) {
        ParsedCommand p;
        p.kind = ParsedCommand::Kind::Help;
        return p;
    }

    auto fc = try_function_code_(trimmed);
    if (fc.kind != ParsedCommand::Kind::Unknown && fc.kind != ParsedCommand::Kind::Empty)
        return fc;

    return try_verb_object_(trimmed);
}

ParsedCommand CommandParser::try_function_code_(const QString& input) {
    // Single token without dots — dotted forms go through verb-object.
    static const QRegularExpression rx(
        R"(^[A-Z0-9][A-Z0-9_]*$)",
        QRegularExpression::CaseInsensitiveOption);
    if (!rx.match(input).hasMatch())
        return ParsedCommand{ParsedCommand::Kind::Unknown, {}, {}, input, {}};

    const QString upper = input.toUpper();
    const QString function_action = action_for_function_code(upper);
    if (!function_action.isEmpty() && ActionRegistry::instance().find(function_action)) {
        ParsedCommand p;
        p.kind = ParsedCommand::Kind::Action;
        p.action_id = function_action;
        return p;
    }

    if (auto* action = ActionRegistry::instance().find(upper.toLower())) {
        ParsedCommand p;
        p.kind = ParsedCommand::Kind::Action;
        p.action_id = action->id;
        return p;
    }

    ParsedCommand p;
    p.kind = ParsedCommand::Kind::Symbol;
    p.raw_remainder = upper;
    p.args.insert(QStringLiteral("symbol"), upper);
    return p;
}

ParsedCommand CommandParser::try_verb_object_(const QString& input) {
    QStringList tokens = tokenise(input);
    if (tokens.isEmpty()) {
        return ParsedCommand{ParsedCommand::Kind::Empty, {}, {}, {}, {}};
    }

    // Greedy: longest prefix wins. "layout switch foo" → tries "layout.switch.foo", then "layout.switch", then "layout".
    auto& reg = ActionRegistry::instance();
    for (int n = tokens.size(); n > 0; --n) {
        QStringList prefix = tokens.mid(0, n);
        const QString candidate = prefix.join('.').toLower();
        if (auto* action = reg.find(candidate)) {
            ParsedCommand p;
            p.kind = ParsedCommand::Kind::Action;
            p.action_id = action->id;
            const QStringList rest = tokens.mid(n);
            p.args = bind_positional_(action->parameter_slots, rest);
            p.raw_remainder = rest.join(' ');
            return p;
        }
    }

    for (const QString& id : reg.all_ids()) {
        const ActionDef* def = reg.find(id);
        if (!def) continue;
        for (const QString& alias : def->aliases) {
            if (input.startsWith(alias, Qt::CaseInsensitive)) {
                const QStringList rest = tokenise(input.mid(alias.size()).trimmed());
                ParsedCommand p;
                p.kind = ParsedCommand::Kind::Action;
                p.action_id = id;
                p.args = bind_positional_(def->parameter_slots, rest);
                p.raw_remainder = rest.join(' ');
                return p;
            }
        }
    }

    ParsedCommand p;
    p.kind = ParsedCommand::Kind::Unknown;
    p.raw_remainder = input;
    p.error = QString("Unknown command: '%1'").arg(input);
    return p;
}

QVariantMap CommandParser::bind_positional_(const QList<ParameterSlot>& slot_list,
                                            const QStringList& positionals) {
    QVariantMap out;
    for (int i = 0; i < slot_list.size() && i < positionals.size(); ++i) {
        const ParameterSlot& slot = slot_list.at(i);
        out.insert(slot.name, positionals.at(i));
    }
    // Apply slot defaults. Handler validates required slots.
    for (int i = positionals.size(); i < slot_list.size(); ++i) {
        const ParameterSlot& slot = slot_list.at(i);
        if (slot.default_value.isValid())
            out.insert(slot.name, slot.default_value);
    }
    return out;
}

} // namespace openmarketterminal::ui
