#include <QtTest>

#include "screens/ai_chat/SessionTitle.h"

using openmarketterminal::ai_chat::derive_session_title_from_message;
using openmarketterminal::ai_chat::generate_session_title;
using openmarketterminal::ai_chat::is_auto_generated_title;

class TstSessionTitle : public QObject {
    Q_OBJECT
private slots:
    // ── The placeholder ↔ predicate invariant ──────────────────────────────
    // Every title generate_session_title() can produce MUST test positive, or
    // first-message auto-naming would refuse to replace some placeholders.
    void generated_titles_are_recognized_as_auto() {
        for (int i = 0; i < 200; ++i)
            QVERIFY2(is_auto_generated_title(generate_session_title()),
                     "a generated placeholder was not recognized as auto-generated");
    }

    void user_names_are_not_auto_generated() {
        QVERIFY(!is_auto_generated_title("My AAPL valuation"));
        QVERIFY(!is_auto_generated_title("whats your read on AAPL"));
        QVERIFY(!is_auto_generated_title(""));
        QVERIFY(!is_auto_generated_title("chat"));
        QVERIFY(!is_auto_generated_title("Session AB12"));      // display fallback, not stored title
        QVERIFY(!is_auto_generated_title("Atlas Brief"));        // missing hex suffix
        QVERIFY(!is_auto_generated_title("Atlas Brief d99e"));    // lowercase hex
        QVERIFY(!is_auto_generated_title("Atlas Brief D99E2"));   // 5 hex digits
        QVERIFY(!is_auto_generated_title("Zebra Brief D99E"));    // prefix not in pool
    }

    // ── Derivation from the first message ───────────────────────────────────
    void short_message_used_verbatim() {
        QCOMPARE(derive_session_title_from_message("DCF for AAPL"), QString("DCF for AAPL"));
    }

    void whitespace_and_newlines_collapse() {
        QCOMPARE(derive_session_title_from_message("  hello\n\n  world\t! "),
                 QString("hello world !"));
    }

    void empty_or_blank_yields_empty() {
        QVERIFY(derive_session_title_from_message("").isEmpty());
        QVERIFY(derive_session_title_from_message("    \n\t  ").isEmpty());
    }

    void long_message_is_truncated_with_ellipsis() {
        const QString msg =
            "what is your read on the latest AAPL earnings and forward guidance for next year";
        const QString out = derive_session_title_from_message(msg, 40);
        QVERIFY(out.endsWith(QStringLiteral("…")));
        // body (sans ellipsis) stays within the budget and breaks on a word boundary
        const QString body = out.left(out.size() - 1);
        QVERIFY(body.size() <= 40);
        QVERIFY(!body.endsWith(' '));
        QVERIFY(msg.startsWith(body));        // it's a real prefix of the message
    }

    void hard_cut_when_no_space_in_range() {
        const QString msg = QString(60, 'x');   // one giant token, no spaces
        const QString out = derive_session_title_from_message(msg, 40);
        QCOMPARE(out, QString(40, 'x') + QStringLiteral("…"));
    }

    // A derived title must NOT look auto-generated, or sending a first message
    // could leave the session eligible for re-naming on the next send.
    void derived_title_is_not_mistaken_for_a_placeholder() {
        QVERIFY(!is_auto_generated_title(derive_session_title_from_message("Atlas Brief on AAPL")));
        QVERIFY(!is_auto_generated_title(derive_session_title_from_message("quick question")));
    }
};

QTEST_MAIN(TstSessionTitle)
#include "tst_session_title.moc"
