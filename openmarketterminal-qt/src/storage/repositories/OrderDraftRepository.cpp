#include "storage/repositories/OrderDraftRepository.h"

namespace openmarketterminal {

OrderDraftRepository& OrderDraftRepository::instance() {
    static OrderDraftRepository s;
    return s;
}

static const char* kDraftColumns =
    "draft_id, intent_json, risk_verdict_json, account, mode_hint, status, created_at, expires_at";

OrderDraft OrderDraftRepository::map_draft(QSqlQuery& q) {
    OrderDraft d;
    d.draft_id = q.value(0).toString();
    d.intent_json = q.value(1).toString();
    d.risk_verdict_json = q.value(2).toString();
    d.account = q.value(3).toString();
    d.mode_hint = q.value(4).toString();
    d.status = q.value(5).toString();
    d.created_at = q.value(6).toString();
    d.expires_at = q.value(7).toString();
    return d;
}

Result<void> OrderDraftRepository::insert(const OrderDraft& d) {
    return exec_write("INSERT INTO order_drafts "
                      "(draft_id, intent_json, risk_verdict_json, account, mode_hint, status, "
                      " created_at, expires_at) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                      {d.draft_id, d.intent_json, d.risk_verdict_json, d.account, d.mode_hint, d.status,
                       d.created_at, d.expires_at});
}

Result<OrderDraft> OrderDraftRepository::get(const QString& draft_id) {
    return query_one(QString("SELECT %1 FROM order_drafts WHERE draft_id = ?").arg(kDraftColumns), {draft_id},
                     map_draft);
}

Result<void> OrderDraftRepository::update_status(const QString& draft_id, const QString& status) {
    return exec_write("UPDATE order_drafts SET status = ? WHERE draft_id = ?", {status, draft_id});
}

Result<bool> OrderDraftRepository::reserve_for_submit(const QString& draft_id) {
    // Compare-and-set: the WHERE clause makes the claim atomic — only the first
    // submit of a prepared draft changes a row; any concurrent/duplicate submit
    // matches zero rows and loses the race.
    auto r = db().execute("UPDATE order_drafts SET status = 'submitting' "
                          "WHERE draft_id = ? AND status = 'prepared'",
                          {draft_id});
    if (r.is_err())
        return Result<bool>::err(r.error());
    return Result<bool>::ok(r.value().numRowsAffected() == 1);
}

} // namespace openmarketterminal
