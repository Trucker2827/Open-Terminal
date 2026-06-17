#pragma once
// Pure expansion of analysis slash commands (/comps, /dcf, /earnings) typed in the
// AI chat into a playbook instruction for the in-app LLM. The matching system-prompt
// playbooks (resources/ai_skills/*.md) carry the actual methodology; this just turns
// "/comps AAPL" into a clear request. Core-only so it's unit-testable.
#include <QString>

namespace openmarketterminal::ai_chat {

// If `raw` is a known analysis slash command WITH an argument, returns the expanded
// LLM instruction (non-empty). If it's a known command MISSING its argument, returns
// empty and sets *usage_out to a "Usage: …" string. Otherwise (not a recognized
// analysis command) returns empty and leaves *usage_out untouched — caller sends the
// text unchanged.
QString expand_analysis_slash_command(const QString& raw, QString* usage_out);

} // namespace openmarketterminal::ai_chat
