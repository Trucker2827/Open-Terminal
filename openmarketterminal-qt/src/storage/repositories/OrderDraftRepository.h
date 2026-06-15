#pragma once
#include "storage/repositories/BaseRepository.h"

namespace openmarketterminal {

/// A prepared-but-not-submitted order intent plus its risk verdict (v049).
/// Backs the AI-trading two-phase order flow: a draft is written in the prepare
/// phase and its status walks prepared → submitted/cancelled/expired.
struct OrderDraft {
    QString draft_id;
    QString intent_json;
    QString risk_verdict_json;
    QString account;
    QString mode_hint;
    QString status;
    QString created_at;
    QString expires_at;
};

/// Data-access layer for the order_drafts table.
class OrderDraftRepository : public BaseRepository<OrderDraft> {
  public:
    static OrderDraftRepository& instance();

    Result<void> insert(const OrderDraft& d);
    Result<OrderDraft> get(const QString& draft_id);
    Result<void> update_status(const QString& draft_id, const QString& status);

  private:
    OrderDraftRepository() = default;
    static OrderDraft map_draft(QSqlQuery& q);
};

} // namespace openmarketterminal
