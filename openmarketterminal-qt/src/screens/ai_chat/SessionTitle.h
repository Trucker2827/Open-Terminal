#pragma once
// Pure session-title helpers for the AI chat sidebar: the random placeholder
// name a brand-new session starts with, a predicate that recognizes that
// placeholder format, and the derivation of a human title from the first user
// message. Core-only (QString in, QString out) so it's unit-testable without
// the GUI — see tests/tst_session_title.cpp.

#include <QString>

namespace openmarketterminal::ai_chat {

// Random placeholder title for a brand-new session, e.g. "Atlas Brief D99E".
// Used until the session is auto-named from its first message (or renamed).
QString generate_session_title();

// True if `title` looks like generate_session_title()'s output (one of the
// known prefixes + a known noun + a 4-digit uppercase hex suffix). Used to
// decide whether auto-naming may overwrite the title — a user-renamed title
// won't match, so first-message naming never clobbers a deliberate name.
bool is_auto_generated_title(const QString& title);

// Derive a concise session title from the first user message: collapse runs of
// whitespace/newlines to single spaces, then truncate near `max_len` chars on a
// word boundary with an ellipsis. Returns empty if `msg` has no usable text
// (e.g. a file-only message) — the caller then keeps the placeholder.
QString derive_session_title_from_message(const QString& msg, int max_len = 40);

} // namespace openmarketterminal::ai_chat
