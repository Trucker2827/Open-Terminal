#include "screens/observer/ObserverJournalPanel.h"

#include "screens/observer/ObserverJournalView.h"
#include "services/observer/ObserverJournalService.h"
#include "ui/markdown/MarkdownRenderer.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

using observer::JournalView;

ObserverJournalPanel::ObserverJournalPanel(QWidget* parent) : QWidget(parent) {
    build_ui();
    // The journal is written out-of-process by the LaunchAgents, so poll instead of
    // relying on a signal. Cheap (a file read) and bounded.
    timer_ = new QTimer(this);
    timer_->setInterval(5000);
    connect(timer_, &QTimer::timeout, this, &ObserverJournalPanel::refresh);
    timer_->start();
    refresh();
}

void ObserverJournalPanel::build_ui() {
    auto* root = new QVBoxLayout(this);

    auto* bar = new QHBoxLayout();
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    const struct { const char* label; JournalView v; } items[] = {
        {QT_TR_NOOP("Latest"), JournalView::Latest},
        {QT_TR_NOOP("Weekly"), JournalView::Weekly},
        {QT_TR_NOOP("Alerts"), JournalView::Alerts},
    };
    for (const auto& it : items) {
        auto* b = new QPushButton(tr(it.label), this);
        b->setCheckable(true);
        if (it.v == view_) b->setChecked(true);
        const JournalView v = it.v;
        connect(b, &QPushButton::clicked, this, [this, v] { set_view(v); });
        group->addButton(b);
        bar->addWidget(b);
    }
    bar->addStretch();
    root->addLayout(bar);

    display_ = new QTextEdit(this);
    display_->setReadOnly(true);
    root->addWidget(display_, 1);
}

void ObserverJournalPanel::set_view(JournalView v) {
    view_ = v;
    refresh();
}

void ObserverJournalPanel::refresh() {
    const QString md = observer::markdown_for(view_, services::ObserverJournalService::instance());
    // The 5s poll would otherwise reset scroll position/selection on every tick;
    // only re-render when the content (or the selected view) actually changed.
    if (md == last_md_) return;
    last_md_ = md;
    display_->setHtml(ui::MarkdownRenderer::render(md));
}

} // namespace openmarketterminal::screens
