// src/screens/geopolitics/RelationshipPanel.cpp
#include "screens/geopolitics/RelationshipPanel.h"

#include "ui/theme/Theme.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollArea>

namespace openmarketterminal::screens {

using namespace openmarketterminal::services::geo;

// Map a GoldsteinScale value (−10 conflict … +10 cooperation) to the severity
// keyword severity_color() understands. Conflict edges read red/orange,
// cooperation reads green.
static QString goldstein_severity(double g) {
    if (g <= -5.0) return QStringLiteral("critical");
    if (g < -2.0)  return QStringLiteral("high");
    if (g < 0.0)   return QStringLiteral("medium");
    return QStringLiteral("low");  // cooperation → green
}

static QColor severity_color(const QString& sev) {
    if (sev == "critical")
        return QColor(ui::colors::NEGATIVE());
    if (sev == "high")
        return QColor(ui::colors::WARNING());
    if (sev == "medium")
        return QColor(ui::colors::WARNING()).lighter(120);
    if (sev == "low")
        return QColor(ui::colors::POSITIVE());
    return QColor(ui::colors::INFO()); // organization
}

RelationshipPanel::RelationshipPanel(QWidget* parent) : QWidget(parent) {
    build_ui();
}

void RelationshipPanel::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header
    auto* header = new QWidget(this);
    header->setFixedHeight(48);
    header->setStyleSheet(
        QString("background:%1; border-bottom:1px solid %2;").arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    auto* hhl = new QHBoxLayout(header);
    hhl->setContentsMargins(16, 0, 16, 0);
    hhl->setSpacing(12);

    title_lbl_ = new QLabel(tr("GEOPOLITICAL RELATIONSHIP NETWORK"), header);
    title_lbl_->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3; letter-spacing:1px;")
                             .arg(ui::colors::INFO())
                             .arg(ui::fonts::TINY)
                             .arg(ui::fonts::DATA_FAMILY()));
    hhl->addWidget(title_lbl_);
    hhl->addStretch();

    stats_lbl_ = new QLabel(tr("ACTORS: —  |  RELATIONSHIPS: —"), header);
    stats_lbl_->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3; padding:2px 8px;"
                                 "background:rgba(255,255,255,0.04); border:1px solid %4;")
                             .arg(ui::colors::TEXT_TERTIARY())
                             .arg(ui::fonts::TINY)
                             .arg(ui::fonts::DATA_FAMILY)
                             .arg(ui::colors::BORDER_DIM()));
    hhl->addWidget(stats_lbl_);

    refresh_btn_ = new QPushButton(tr("⟳ REFRESH"), header);
    refresh_btn_->setCursor(Qt::PointingHandCursor);
    refresh_btn_->setStyleSheet(QString("QPushButton { color:%1; font-size:%2px; font-family:%3; font-weight:700;"
                                        "padding:3px 10px; background:transparent; border:1px solid %4; }"
                                        "QPushButton:hover { background:rgba(255,255,255,0.06); color:%5; }")
                                    .arg(ui::colors::TEXT_SECONDARY())
                                    .arg(ui::fonts::TINY)
                                    .arg(ui::fonts::DATA_FAMILY)
                                    .arg(ui::colors::BORDER_DIM())
                                    .arg(ui::colors::INFO()));
    connect(refresh_btn_, &QPushButton::clicked, this, [this]() {
        if (provenance_lbl_)
            provenance_lbl_->setText(tr("Refreshing live GDELT events…"));
        emit refresh_requested();
    });
    hhl->addWidget(refresh_btn_);
    root->addWidget(header);

    // Provenance: real source + freshness, set for real once data arrives.
    provenance_lbl_ = new QLabel(tr("Live actor→actor events from GDELT (CAMEO-coded)."), this);
    provenance_lbl_->setWordWrap(true);
    provenance_lbl_->setStyleSheet(QString("color:%1; background:%2; padding:4px 16px; font-size:%3px;")
                                       .arg(ui::colors::TEXT_TERTIARY(), ui::colors::BG_RAISED())
                                       .arg(ui::fonts::TINY));
    root->addWidget(provenance_lbl_);

    // Scrollable network view — populated by render_network() from nodes_.
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setStyleSheet(QString("QScrollArea { border:none; background:%1; }"
                                   "QScrollBar:vertical { background:%1; width:6px; }"
                                   "QScrollBar::handle:vertical { background:%2; border-radius:3px; }"
                                   "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }")
                               .arg(ui::colors::BG_BASE(), ui::colors::BORDER_MED()));
    root->addWidget(scroll_, 1);

    render_network();  // initial placeholder until set_event_network() arrives
}

// Rebuild the scroll contents from nodes_ (directed actor→actor edges grouped
// by conflict / cooperation). Called on data arrival and on language change.
void RelationshipPanel::render_network() {
    if (!scroll_)
        return;

    auto* content = new QWidget(scroll_);
    content->setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    auto* cvl = new QVBoxLayout(content);
    cvl->setContentsMargins(16, 16, 16, 16);
    cvl->setSpacing(16);

    if (nodes_.isEmpty()) {
        auto* empty = new QLabel(tr("Loading live GDELT events…"), content);
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3; padding:40px;")
                                 .arg(ui::colors::TEXT_TERTIARY())
                                 .arg(ui::fonts::SMALL)
                                 .arg(ui::fonts::DATA_FAMILY));
        cvl->addWidget(empty);
        cvl->addStretch();
        scroll_->setWidget(content);
        return;
    }

    auto add_section = [&](const QString& title, const QString& color, const QString& type) {
        // Skip empty sections so we never show a header with no cards.
        bool any = false;
        for (const auto& n : nodes_)
            if (n.type == type) { any = true; break; }
        if (!any)
            return;

        auto* sec_lbl = new QLabel(title, content);
        sec_lbl->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3;"
                                       "letter-spacing:2px; padding-bottom:4px; border-bottom:1px solid rgba(%4,0.3);")
                                   .arg(color)
                                   .arg(ui::fonts::SMALL)
                                   .arg(ui::fonts::DATA_FAMILY)
                                   .arg(color.mid(1)));
        cvl->addWidget(sec_lbl);

        auto* grid_w = new QWidget(content);
        auto* gl = new QGridLayout(grid_w);
        gl->setContentsMargins(0, 0, 0, 0);
        gl->setSpacing(10);
        gl->setColumnStretch(0, 1);
        gl->setColumnStretch(1, 1);
        gl->setColumnStretch(2, 1);

        int col = 0, row = 0;
        for (const auto& n : nodes_) {
            if (n.type != type)
                continue;
            gl->addWidget(build_node_card(n, grid_w), row, col);
            if (++col >= 3) {
                col = 0;
                row++;
            }
        }
        while (col > 0 && col < 3) {
            auto* spacer = new QWidget(grid_w);
            spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            gl->addWidget(spacer, row, col++);
        }
        cvl->addWidget(grid_w);
    };

    add_section(tr("MATERIAL CONFLICT"), ui::colors::NEGATIVE(), "conflict");
    add_section(tr("COOPERATION"), ui::colors::POSITIVE(), "cooperation");

    cvl->addStretch();
    scroll_->setWidget(content);  // replaces (and deletes) any prior content
}

void RelationshipPanel::set_event_network(const QJsonObject& data) {
    const QJsonArray edges = data.value("edges").toArray();
    nodes_.clear();
    nodes_.reserve(edges.size());
    int conflict_edges = 0;
    for (const auto& e : edges) {
        const QJsonObject o = e.toObject();
        const double g = o.value("goldstein").toDouble();
        const QString polarity = o.value("polarity").toString();
        if (polarity == "conflict")
            ++conflict_edges;

        RelationshipNode n;
        n.id = o.value("from_code").toString() + ">" + o.value("to_code").toString();
        n.label = o.value("from").toString() + " → " + o.value("to").toString();
        n.type = polarity;                       // "conflict" | "cooperation"
        n.severity = goldstein_severity(g);
        n.dataset_count = o.value("count").toInt();
        n.connections = {tr("Goldstein %1").arg(g, 0, 'f', 1),
                         tr("%1 mentions").arg(o.value("mentions").toInt())};
        nodes_.append(n);
    }

    node_count_ = data.value("actors").toInt();
    conflict_count_ = conflict_edges;
    org_count_ = data.value("relationships").toInt();

    if (stats_lbl_)
        stats_lbl_->setText(tr("ACTORS: %1  |  RELATIONSHIPS: %2  |  CONFLICT: %3")
                                .arg(node_count_).arg(org_count_).arg(conflict_count_));
    if (provenance_lbl_)
        provenance_lbl_->setText(tr("Live actor→actor events from GDELT (CAMEO-coded) — %1.")
                                     .arg(data.value("window").toString(tr("recent exports"))));

    render_network();
}

QWidget* RelationshipPanel::build_node_card(const RelationshipNode& node, QWidget* parent) {
    auto color = severity_color(node.severity.isEmpty() ? "organization" : node.severity);
    const QString col_hex = color.name();
    const QString col_rgb = QString("%1,%2,%3").arg(color.red()).arg(color.green()).arg(color.blue());

    auto* card = new QWidget(parent);
    card->setObjectName("nodeCard");
    card->setMinimumHeight(110);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    card->setStyleSheet(QString("#nodeCard { background:%1; border:1px solid rgba(%2,0.25);"
                                "border-left:3px solid %3; }")
                            .arg(ui::colors::BG_RAISED())
                            .arg(col_rgb)
                            .arg(col_hex));

    auto* vl = new QVBoxLayout(card);
    vl->setContentsMargins(12, 10, 12, 10);
    vl->setSpacing(4);

    // Name row
    auto* name_row = new QWidget(card);
    auto* name_hl = new QHBoxLayout(name_row);
    name_hl->setContentsMargins(0, 0, 0, 0);
    name_hl->setSpacing(6);

    auto* name = new QLabel(node.label.toUpper(), name_row);
    name->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3;")
                            .arg(col_hex)
                            .arg(ui::fonts::SMALL)
                            .arg(ui::fonts::DATA_FAMILY));
    name_hl->addWidget(name, 1);

    const QString badge_text = node.severity.isEmpty() ? node.type.toUpper() : node.severity.toUpper();
    auto* badge = new QLabel(badge_text, name_row);
    badge->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3;"
                                 "padding:2px 6px; background:rgba(%4,0.12); border:1px solid rgba(%4,0.3);"
                                 "border-radius:8px; letter-spacing:1px;")
                             .arg(col_hex)
                             .arg(ui::fonts::TINY)
                             .arg(ui::fonts::DATA_FAMILY)
                             .arg(col_rgb));
    name_hl->addWidget(badge);
    vl->addWidget(name_row);

    // Type label
    auto* type_lbl = new QLabel(node.type.toUpper(), card);
    type_lbl->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3;")
                                .arg(ui::colors::TEXT_TERTIARY())
                                .arg(ui::fonts::TINY)
                                .arg(ui::fonts::DATA_FAMILY));
    vl->addWidget(type_lbl);

    // Dataset count row
    auto* ds_row = new QWidget(card);
    auto* ds_hl = new QHBoxLayout(ds_row);
    ds_hl->setContentsMargins(0, 0, 0, 0);
    ds_hl->setSpacing(4);

    auto* ds_num = new QLabel(QString::number(node.dataset_count), ds_row);
    ds_num->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700; font-family:%3;")
                              .arg(ui::colors::TEXT_PRIMARY())
                              .arg(ui::fonts::SMALL)
                              .arg(ui::fonts::DATA_FAMILY));
    ds_hl->addWidget(ds_num);

    auto* ds_lbl = new QLabel(tr("events"), ds_row);
    ds_lbl->setStyleSheet(QString("color:%1; font-size:%2px; font-family:%3;")
                              .arg(ui::colors::TEXT_TERTIARY())
                              .arg(ui::fonts::SMALL)
                              .arg(ui::fonts::DATA_FAMILY));
    ds_hl->addWidget(ds_lbl);
    ds_hl->addStretch();
    vl->addWidget(ds_row);

    // Connection pills (max 3)
    if (!node.connections.isEmpty()) {
        auto* pills_row = new QWidget(card);
        auto* pills_hl = new QHBoxLayout(pills_row);
        pills_hl->setContentsMargins(0, 2, 0, 0);
        pills_hl->setSpacing(4);

        const int max_pills = 3;
        int shown = qMin(node.connections.size(), max_pills);
        for (int i = 0; i < shown; ++i) {
            auto* pill = new QLabel(node.connections[i], pills_row);
            pill->setStyleSheet(QString("color:%1; font-size:%2px; padding:1px 6px;"
                                        "background:rgba(255,255,255,0.05); border:1px solid %3;"
                                        "border-radius:8px; font-family:%4;")
                                    .arg(ui::colors::TEXT_SECONDARY())
                                    .arg(ui::fonts::TINY)
                                    .arg(ui::colors::BORDER_DIM())
                                    .arg(ui::fonts::DATA_FAMILY));
            pills_hl->addWidget(pill);
        }

        if (node.connections.size() > max_pills) {
            auto* more = new QLabel(QString("+%1").arg(node.connections.size() - max_pills), pills_row);
            more->setStyleSheet(QString("color:%1; font-size:%2px; padding:1px 6px;"
                                        "background:rgba(255,255,255,0.05); border:1px solid %3;"
                                        "border-radius:8px; font-family:%4;")
                                    .arg(ui::colors::TEXT_TERTIARY())
                                    .arg(ui::fonts::TINY)
                                    .arg(ui::colors::BORDER_DIM())
                                    .arg(ui::fonts::DATA_FAMILY));
            pills_hl->addWidget(more);
        }

        pills_hl->addStretch();
        vl->addWidget(pills_row);
    }

    return card;
}

void RelationshipPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void RelationshipPanel::retranslateUi() {
    if (title_lbl_) title_lbl_->setText(tr("GEOPOLITICAL RELATIONSHIP NETWORK"));
    if (refresh_btn_) refresh_btn_->setText(tr("⟳ REFRESH"));
    if (stats_lbl_) {
        if (org_count_ > 0)
            stats_lbl_->setText(tr("ACTORS: %1  |  RELATIONSHIPS: %2  |  CONFLICT: %3")
                                    .arg(node_count_).arg(org_count_).arg(conflict_count_));
        else
            stats_lbl_->setText(tr("ACTORS: —  |  RELATIONSHIPS: —"));
    }
    // Section titles + the empty/loading placeholder are rebuilt with the new
    // translator by re-rendering. Edge labels (country names) are data, not
    // translated.
    render_network();
}

} // namespace openmarketterminal::screens
