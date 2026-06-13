#include "ui/components/ProvenanceBadge.h"

#include <QHBoxLayout>
#include <QLabel>

namespace openmarketterminal::ui {

ProvenanceBadge::ProvenanceBadge(QWidget* parent) : QWidget(parent) {
    auto* hl = new QHBoxLayout(this);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(5);

    dot_ = new QLabel(QStringLiteral("●"), this);
    dot_->setStyleSheet(QStringLiteral("color:#5a5f68; font-size:10px;"));
    text_ = new QLabel(this);
    text_->setStyleSheet(QStringLiteral("color:#8a8f98; font-size:10px;"));

    hl->addWidget(dot_);
    hl->addWidget(text_);
    hl->addStretch();

    clear_provenance();
}

void ProvenanceBadge::clear_provenance() {
    if (text_)
        text_->setText(tr("no data source"));
    if (dot_)
        dot_->setStyleSheet(QStringLiteral("color:#5a5f68; font-size:10px;"));
    setToolTip({});
}

void ProvenanceBadge::set_provenance(const connectors::Provenance& prov) {
    const QString fresh = prov.freshness_label();

    QString dot_color = QStringLiteral("#5a5f68");
    if (fresh == QLatin1String("LIVE"))
        dot_color = QStringLiteral("#2ecc71"); // green
    else if (fresh == QLatin1String("STALE"))
        dot_color = QStringLiteral("#e0a526"); // amber
    else if (fresh == QLatin1String("CACHED"))
        dot_color = QStringLiteral("#8a8f98"); // grey
    else
        dot_color = QStringLiteral("#c0392b"); // red — no data

    dot_->setStyleSheet(QStringLiteral("color:%1; font-size:10px;").arg(dot_color));

    QString cache_note;
    const qint64 age = prov.age_seconds();
    if (prov.from_cache && age >= 0)
        cache_note = QStringLiteral(" · cache %1s").arg(age);

    text_->setText(QStringLiteral("%1 · %2 · %3 · %4%5")
                       .arg(prov.source, connectors::key_requirement_label(prov.key_req, prov.key_present),
                            prov.timestamp_label(), fresh, cache_note));
    setToolTip(prov.source_url);
}

} // namespace openmarketterminal::ui
