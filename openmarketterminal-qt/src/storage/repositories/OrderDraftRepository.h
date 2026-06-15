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

    /// Atomically claim a prepared draft for submission: flips status
    /// prepared → submitting ONLY if it is still "prepared", in a single
    /// compare-and-set UPDATE. Returns ok(true) if THIS call won the claim
    /// (exactly one row changed), ok(false) if the draft was already
    /// used/not-prepared (lost the race), err on DB failure. This is the guard
    /// that makes double-submit impossible — a check-then-act on get()+status
    /// would have a TOCTOU window under concurrent submits.
    Result<bool> reserve_for_submit(const QString& draft_id);

  private:
    OrderDraftRepository() = default;
    static OrderDraft map_draft(QSqlQuery& q);
};

} // namespace openmarketterminal
