#pragma once

// Pure (Core-only, no QtWidgets) view-selection logic for the Observer panel: maps a
// view choice to the Markdown the panel should render, by reading ObserverJournalService.
// Split out from the QWidget shell so it is unit-testable without a GUI/markdown link.

#include <QString>

namespace openmarketterminal::services { class ObserverJournalService; }

namespace openmarketterminal::screens::observer {

enum class JournalView { Latest, Weekly, Alerts };

// Markdown for the given view. Returns a friendly "not found" / "nothing yet" message
// rather than empty when the journal is missing or a section has no entries.
QString markdown_for(JournalView view, const services::ObserverJournalService& svc);

} // namespace openmarketterminal::screens::observer
