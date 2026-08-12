#include "screens/kalshi/ContextIndexCockpitReader.h"

#include "core/config/AppPaths.h"
#include "services/provenance/TradeProvenanceIndex.h"

#include <QFileInfo>

namespace openmarketterminal::screens::kalshi {

ContextIndexCockpitInput read_context_index_cockpit(const QString& path) {
    ContextIndexCockpitInput input;
    if (!QFileInfo::exists(path))
        return input;

    provenance::TradeProvenanceIndex index;
    const auto opened = index.open_read_only(path);
    if (opened.is_err()) {
        input.read_state = ContextIndexReadState::Unreadable;
        input.error = QString::fromStdString(opened.error());
        return input;
    }
    const auto status = index.status();
    if (status.is_err()) {
        input.read_state = ContextIndexReadState::Unreadable;
        input.error = QString::fromStdString(status.error());
        return input;
    }
    const provenance::ContextIndexStatus& value = status.value();
    input.current_batch_id = value.current_batch_id;
    input.current_digest = value.current_digest;
    input.capture_finished_at_ms = value.capture_finished_at_ms;
    input.node_count = value.node_count;
    input.edge_count = value.edge_count;
    input.unresolved_count = value.unresolved_count;
    input.invariant_failure_count = value.invariant_failure_count;
    input.read_state = value.has_current_batch ? ContextIndexReadState::Current : ContextIndexReadState::Empty;
    return input;
}

ContextIndexCockpitInput read_default_context_index_cockpit() {
    return read_context_index_cockpit(provenance::TradeProvenanceIndex::default_path(AppPaths::root()));
}

} // namespace openmarketterminal::screens::kalshi
