#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QtMath>

#include <cmath>

namespace openmarketterminal::screens {

enum class StrategyCockpitView {
    None = 0,
    EvidenceAll,
    DecisionEnvelopes,
    EvidenceChronos,
    EvidenceOpen,
    EvidenceResolved,
    EvidenceEligible,
    EvidenceNoEdge,
    EvidencePnl,
    EvidenceCoinbase,
    EvidenceKalshi,
    ResearchInputs,
    RiskSafety,
    PaperHandlers,
    Outcomes,
    EvidenceBook,
};

struct StrategyCockpitHit {
    StrategyCockpitView view = StrategyCockpitView::None;
    const char* label = nullptr;
    int book_index = -1;
};

inline bool strategy_evidence_matches(StrategyCockpitView view, const QString& kind,
                                      const QString& source, int samples, int open,
                                      const QString& proof_status, const QString& target_kind = {}) {
    switch (view) {
    case StrategyCockpitView::EvidenceChronos: return kind.startsWith(QStringLiteral("chronos2"));
    case StrategyCockpitView::EvidenceOpen: return open > 0;
    case StrategyCockpitView::EvidenceResolved: return samples > 0;
    case StrategyCockpitView::EvidenceEligible: return proof_status == QStringLiteral("PROMOTION READY");
    case StrategyCockpitView::EvidenceNoEdge: return proof_status == QStringLiteral("NO EDGE");
    case StrategyCockpitView::EvidenceCoinbase: return source.contains(QStringLiteral("coinbase"), Qt::CaseInsensitive);
    case StrategyCockpitView::EvidenceKalshi:
        return source.contains(QStringLiteral("kalshi"), Qt::CaseInsensitive) || kind == QStringLiteral("kalshi");
    case StrategyCockpitView::EvidenceBook: return kind == target_kind;
    default: return true;
    }
}

inline StrategyCockpitHit strategy_cockpit_hit(const QPointF& point, const QSizeF& size,
                                                qreal phase = 0.0, int book_count = 0) {
    static constexpr StrategyCockpitView chip_views[] = {
        StrategyCockpitView::EvidenceAll, StrategyCockpitView::DecisionEnvelopes,
        StrategyCockpitView::EvidenceChronos, StrategyCockpitView::EvidenceOpen,
        StrategyCockpitView::EvidenceResolved, StrategyCockpitView::EvidenceEligible,
        StrategyCockpitView::EvidenceNoEdge, StrategyCockpitView::EvidencePnl,
    };
    static constexpr const char* chip_labels[] = {
        "Inspect all proof books", "Inspect decision envelopes", "Inspect Chronos cohorts",
        "Inspect open paper positions", "Inspect resolved evidence", "Inspect promotion-ready books",
        "Inspect no-edge books", "Inspect cost-net P&L by book",
    };
    for (int i = 0; i < 8; ++i) {
        if (QRectF(18.0 + i * 134.0, 72.0, 126.0, 42.0).contains(point))
            return {chip_views[i], chip_labels[i], -1};
    }

    const qreal w = size.width();
    const qreal h = size.height();
    const QPointF center(w * 0.5, h * 0.5 + 42.0);
    const auto in_node = [&point](const QPointF& node, qreal radius) {
        return QRectF(node.x() - radius, node.y() - radius * 0.58,
                      radius * 2.0, radius * 1.16).contains(point);
    };

    if (in_node(QPointF(w * 0.82, h * 0.34), 42.0))
        return {StrategyCockpitView::RiskSafety, "Inspect deterministic risk and safety gates", -1};
    if (in_node(QPointF(w * 0.86, h * 0.52), 42.0))
        return {StrategyCockpitView::PaperHandlers, "Inspect paper executor handlers", -1};
    if (in_node(QPointF(w * 0.80, h * 0.70), 42.0))
        return {StrategyCockpitView::Outcomes, "Inspect outcomes, scores, and run history", -1};

    if (in_node(QPointF(w * 0.18, h * 0.34), 42.0))
        return {StrategyCockpitView::EvidenceCoinbase, "Inspect Coinbase-derived evidence", -1};
    if (in_node(QPointF(w * 0.15, h * 0.52), 42.0))
        return {StrategyCockpitView::EvidenceKalshi, "Inspect Kalshi and odds evidence", -1};
    if (in_node(QPointF(w * 0.20, h * 0.70), 42.0))
        return {StrategyCockpitView::ResearchInputs, "Open research notebooks for news and notes", -1};
    if (in_node(QPointF(center.x(), center.y() - 116.0), 48.0))
        return {StrategyCockpitView::EvidenceChronos, "Inspect Chronos forecast cohorts", -1};
    if (in_node(center, 64.0))
        return {StrategyCockpitView::DecisionEnvelopes, "Inspect the latest decision envelope", -1};

    const qreal orbit_radius = qMin(w, h) * 0.31;
    const int visible_books = qMin(book_count, 14);
    for (int i = 0; i < visible_books; ++i) {
        const qreal angle = -M_PI_2 + (2.0 * M_PI * i / qMax(1, visible_books)) + phase * 0.08;
        const qreal pulse = 1.0 + 0.06 * std::sin(phase * 4.0 + i);
        const QPointF node(center.x() + std::cos(angle) * orbit_radius,
                           center.y() + std::sin(angle) * orbit_radius * 0.68);
        if (in_node(node, 33.0 * pulse))
            return {StrategyCockpitView::EvidenceBook, "Inspect this exact proof-book cohort", i};
    }
    return {};
}

} // namespace openmarketterminal::screens
