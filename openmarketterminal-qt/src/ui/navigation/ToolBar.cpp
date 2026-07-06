#include "ui/navigation/ToolBar.h"

#include "auth/AuthManager.h"
#include "ui/pushpins/PushpinBar.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QAction>
#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>

namespace openmarketterminal::ui {

static QString menu_ss() {
    return QString("QMenuBar{background:transparent;color:%1;border:none;spacing:0;}"
                   "QMenuBar::item{background:transparent;padding:4px 7px;}"
                   "QMenuBar::item:selected{background:%2;color:%3;}")
        .arg(colors::TEXT_SECONDARY())
        .arg(colors::BG_RAISED())
        .arg(colors::TEXT_PRIMARY());
}

static QString popup_ss() {
    return QString("QMenu{background:%1;color:%2;border:1px solid %3;padding:4px 0;}"
                   "QMenu::item{padding:5px 24px 5px 12px;}"
                   "QMenu::item:selected{background:%4;}"
                   "QMenu::item:disabled{color:%5;}"
                   "QMenu::separator{background:%3;height:1px;margin:4px 8px;}")
        .arg(colors::BG_SURFACE())
        .arg(colors::TEXT_PRIMARY())
        .arg(colors::BORDER_DIM())
        .arg(colors::BG_RAISED())
        .arg(colors::TEXT_DIM());
}

ToolBar::ToolBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(32);
    auto* hl = new QHBoxLayout(this);
    hl->setContentsMargins(4, 0, 6, 0);
    hl->setSpacing(0);

    menu_bar_ = new QMenuBar(this);
    // Keep the menu inline in the toolbar on macOS. Without this, Qt migrates
    // the QMenuBar into the native screen-top application menu, leaving
    // the root menus missing from the toolbar row on Mac builds.
    menu_bar_->setNativeMenuBar(false);
    menu_bar_->setStyleSheet(menu_ss());
    rebuild_menus();
    hl->addWidget(menu_bar_);

    command_bar_ = new CommandBar(this);
    command_bar_->setMinimumWidth(80);
    command_bar_->setMaximumWidth(240);
    command_bar_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    connect(command_bar_, &CommandBar::navigate_to, this, &ToolBar::navigate_to);
    connect(command_bar_, &CommandBar::dock_command, this, &ToolBar::dock_command);
    hl->addWidget(command_bar_);

    auto sep = [&]() -> QLabel* {
        auto* s = new QLabel("|");
        s->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        hl->addWidget(s);
        separators_.append(s);
        return s;
    };

    auto mk = [](const QString& t) -> QLabel* {
        auto* l = new QLabel(t);
        l->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        return l;
    };

    live_dot_ = mk(QString::fromUtf8("\xe2\x97\x8f")); // U+25CF — pure icon, no translation
    hl->addWidget(live_dot_);
    live_label_ = mk({});
    hl->addWidget(live_label_);

    sep();

    // Inline pushpin chips — used to live in a separate toolbar row, now
    // embedded directly so symbol pins share the row with menus and clock.
    pushpin_bar_ = new PushpinBar(this);
    pushpin_bar_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(pushpin_bar_, &PushpinBar::symbol_activated, this, &ToolBar::symbol_pin_activated);
    hl->addWidget(pushpin_bar_, 1);

    sep();

    clock_label_ = mk("");
    clock_label_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    hl->addWidget(clock_label_);

    hl->addStretch(0);

    user_label_ = mk("---");
    user_label_->setMaximumWidth(120);
    hl->addWidget(user_label_);
    sep();

    logout_btn_ = new QPushButton;
    logout_btn_->setFixedHeight(20);
    logout_btn_->setCursor(Qt::PointingHandCursor);
    logout_btn_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(logout_btn_, &QPushButton::clicked, this, &ToolBar::logout_clicked);
    hl->addWidget(logout_btn_);

    retranslateUi();

    clock_timer_ = new QTimer(this);
    clock_timer_->setInterval(1000);
    connect(clock_timer_, &QTimer::timeout, this, &ToolBar::update_clock);
    clock_timer_->start();
    update_clock();

    connect(&auth::AuthManager::instance(), &auth::AuthManager::auth_state_changed, this,
            &ToolBar::refresh_user_display);

    connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
            [this](const ThemeTokens&) { refresh_theme(); });

    refresh_user_display();
    refresh_theme();
}

void ToolBar::changeEvent(QEvent* e) {
    if (e->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(e);
}

void ToolBar::retranslateUi() {
    if (live_label_)     live_label_->setText(tr(" LIVE"));
    if (logout_btn_) logout_btn_->setText(tr("LOCK"));
    // Rebuild menus so the new translator applies to every QAction label.
    rebuild_menus();
    // Refresh user display so "FREE" / "---" placeholders pick up new locale.
    refresh_user_display();
}

void ToolBar::rebuild_menus() {
    if (!menu_bar_) return;
    menu_bar_->clear();
    menu_bar_->addMenu(build_file_menu());
    menu_bar_->addMenu(build_markets_menu());
    menu_bar_->addMenu(build_trading_menu());
    menu_bar_->addMenu(build_research_menu());
    menu_bar_->addMenu(build_tools_menu());
    menu_bar_->addMenu(build_view_menu());
    menu_bar_->addMenu(build_help_menu());
    // Reapply popup stylesheet after rebuild — refresh_theme normally does
    // this on theme change, but a pure language switch should also style
    // the freshly created submenus.
    for (auto* action : menu_bar_->actions())
        if (action->menu())
            action->menu()->setStyleSheet(popup_ss());
}

void ToolBar::refresh_theme() {
    setStyleSheet(QString("background:%1;border-bottom:1px solid %2;").arg(colors::BG_BASE()).arg(colors::BORDER_DIM()));
    if (menu_bar_) {
        menu_bar_->setStyleSheet(menu_ss());
        for (auto* action : menu_bar_->actions())
            if (action->menu())
                action->menu()->setStyleSheet(popup_ss());
    }
    for (auto* s : separators_)
        s->setStyleSheet(QString("color:%1;background:transparent;padding:0 3px;").arg(colors::TEXT_DIM()));
    auto lbl = [](QLabel* l, const QString& c, bool b = false) {
        if (l)
            l->setStyleSheet(QString("color:%1;%2background:transparent;").arg(c, b ? "font-weight:700;" : ""));
    };
    lbl(openmarketterminal_label_, colors::AMBER(), true);
    lbl(branding_label_, colors::TEXT_PRIMARY(), true);
    lbl(subtitle_label_, colors::TEXT_SECONDARY());
    lbl(live_dot_, colors::POSITIVE());
    lbl(live_label_, colors::POSITIVE(), true);
    lbl(clock_label_, colors::TEXT_PRIMARY());
    lbl(user_label_, colors::AMBER());
    if (logout_btn_)
        logout_btn_->setStyleSheet(QString("QPushButton{background:transparent;color:%1;border:1px solid %1;"
                                           "padding:0 8px;font-weight:700;}"
                                           "QPushButton:hover{background:%1;color:%2;border-color:%1;}")
                                       .arg(colors::NEGATIVE())
                                       .arg(colors::TEXT_PRIMARY()));
}

void ToolBar::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    apply_responsive_layout(e->size().width());
}

void ToolBar::apply_responsive_layout(int w) {
    // Progressive disclosure thresholds: 1200=subtitle, 800=clock+LIVE, 650=credits+chat.
    bool show_subtitle = (w >= 1200);
    bool show_clock = (w >= 800);
    bool show_live = (w >= 800);
    bool show_credits = (w >= 650);
    bool show_chat = (w >= 650);

    if (subtitle_label_)
        subtitle_label_->setVisible(show_subtitle);
    if (clock_label_)
        clock_label_->setVisible(show_clock);
    if (live_dot_)
        live_dot_->setVisible(show_live);
    if (live_label_)
        live_label_->setVisible(show_live);

    if (separators_.size() >= 3) {
        separators_[1]->setVisible(show_credits);
        separators_[2]->setVisible(show_chat);
    }
}

void ToolBar::update_clock() {
    auto dt = QDateTime::currentDateTime();
    clock_label_->setText(dt.toString("dd MMM yy").toUpper() + " " + dt.toString("HH:mm:ss"));
}

void ToolBar::refresh_user_display() {
    const auto& s = auth::AuthManager::instance().session();
    if (!s.authenticated) {
        user_label_->setText("---");
        return;
    }

    QString name = s.user_info.username.isEmpty() ? s.user_info.email : s.user_info.username;
    QFontMetrics fm(user_label_->font());
    user_label_->setText(fm.elidedText(name, Qt::ElideRight, user_label_->maximumWidth() - 4));
    user_label_->setToolTip(name);
}

QMenu* ToolBar::build_file_menu() {
    auto* m = new QMenu(tr("File"), this);
    m->setStyleSheet(popup_ss());
    m->addAction(tr("New Window"), this, [this]() { emit action_triggered("new_window"); });

    // Rebuilt on popup so plug/unplug is reflected. Emits "move_to_monitor:<name>" — names are stable, indices aren't.
    auto* monitors = m->addMenu(tr("Move to Monitor"));
    monitors->setStyleSheet(popup_ss());
    connect(monitors, &QMenu::aboutToShow, this, [this, monitors]() {
        monitors->clear();
        const auto screens = QGuiApplication::screens();
        if (screens.size() <= 1) {
            auto* only = monitors->addAction(tr("(single monitor)"));
            only->setEnabled(false);
            return;
        }
        int idx = 1;
        for (QScreen* s : screens) {
            const QString name = s->name();
            const QSize size = s->size();
            // Resolution format (W×H) is locale-neutral.
            const QString label =
                QString("%1. %2  (%3×%4)").arg(idx++).arg(name).arg(size.width()).arg(size.height());
            monitors->addAction(label, this, [this, name]() {
                emit action_triggered(QString("move_to_monitor:%1").arg(name));
            });
        }
    });

    m->addAction(tr("Close Window"),      this, [this]() { emit action_triggered("close_window"); });
    m->addAction(tr("Close All Windows"), this, [this]() { emit action_triggered("close_all_windows"); });

    m->addSeparator();
    m->addAction(tr("New Layout"),       this, [this]() { emit action_triggered("layout_new"); });
    m->addAction(tr("Open Layout…"),     this, [this]() { emit action_triggered("layout_open"); });
    m->addAction(tr("Save Layout"),      this, [this]() { emit action_triggered("layout_save"); });
    m->addAction(tr("Save Layout As…"),  this, [this]() { emit action_triggered("layout_save_as"); });
    m->addSeparator();
    m->addAction(tr("Import Layout"), this, [this]() { emit action_triggered("import_data"); });
    m->addAction(tr("Export Layout"), this, [this]() { emit action_triggered("export_data"); });
    m->addSeparator();
    m->addAction(tr("File Manager"), this, [this]() { emit navigate_to("file_manager"); });
    m->addSeparator();
    m->addAction(tr("Refresh All"), this, [this]() { emit action_triggered("refresh"); });
    return m;
}

QMenu* ToolBar::build_markets_menu() {
    auto* m = new QMenu(tr("Markets"), this);
    m->setStyleSheet(popup_ss());
    auto nav = [this](QMenu* menu, const QString& label, const QString& id) {
        menu->addAction(label, this, [this, id]() { emit navigate_to(id); });
    };
    nav(m, tr("Markets"), "markets");
    nav(m, tr("Dashboard"), "dashboard");
    nav(m, tr("Watchlist"), "watchlist");
    nav(m, tr("News"), "news");
    m->addSeparator();
    nav(m, tr("Economics"), "economics");
    nav(m, tr("GOVT Data"), "gov_data");
    nav(m, tr("DBnomics"), "dbnomics");
    nav(m, tr("AKShare Data"), "akshare");
    nav(m, tr("Asia Markets"), "asia_markets");
    nav(m, tr("Relationship Map"), "relationship_map");
    return m;
}

QMenu* ToolBar::build_trading_menu() {
    auto* m = new QMenu(tr("Trading"), this);
    m->setStyleSheet(popup_ss());
    auto nav = [this](QMenu* menu, const QString& label, const QString& id) {
        menu->addAction(label, this, [this, id]() { emit navigate_to(id); });
    };
    nav(m, tr("Crypto"), "crypto_trading");
    nav(m, tr("Bitcoin"), "bitcoin");
    nav(m, tr("Equity"), "equity_trading");
    nav(m, tr("Portfolio"), "portfolio");
    nav(m, tr("Prediction Markets"), "polymarket");
    nav(m, tr("Edge Radar"), "edge_radar");
    m->addSeparator();
    nav(m, tr("Derivatives"), "derivatives");
    nav(m, tr("Algo Trading"), "algo_trading");
    nav(m, tr("Backtesting"), "backtesting");
    nav(m, tr("Alpha Arena"), "alpha_arena");
    return m;
}

QMenu* ToolBar::build_research_menu() {
    auto* m = new QMenu(tr("Research"), this);
    m->setStyleSheet(popup_ss());
    auto nav = [this](QMenu* menu, const QString& label, const QString& id) {
        menu->addAction(label, this, [this, id]() { emit navigate_to(id); });
    };
    nav(m, tr("Equity Research"), "equity_research");
    nav(m, tr("Research Sources"), "research_sources");
    nav(m, tr("M&A Analytics"), "ma_analytics");
    nav(m, tr("Alt. Investments"), "alt_investments");
    nav(m, tr("Geopolitics"), "geopolitics");
    nav(m, tr("Maritime"), "maritime");
    nav(m, tr("Surface Analytics"), "surface_analytics");
    m->addSeparator();
    nav(m, tr("AI Quant Lab"), "ai_quant_lab");
    nav(m, tr("QuantLib"), "quantlib");
    return m;
}

QMenu* ToolBar::build_tools_menu() {
    auto* m = new QMenu(tr("Tools"), this);
    m->setStyleSheet(popup_ss());
    auto nav = [this](QMenu* menu, const QString& label, const QString& id) {
        menu->addAction(label, this, [this, id]() { emit navigate_to(id); });
    };
    nav(m, tr("Tools Health"), "tools_health");
    m->addSeparator();
    nav(m, tr("Agent Studio"), "agent_config");
    nav(m, tr("MCP Servers"), "mcp_servers");
    nav(m, tr("Data Mapping"), "data_mapping");
    nav(m, tr("Data Sources"), "data_sources");
    nav(m, tr("Report Builder"), "report_builder");
    nav(m, tr("Excel"), "excel");
    nav(m, tr("Trade Intelligence"), "trade_viz");
    nav(m, tr("File Manager"), "file_manager");
    nav(m, tr("Notes"), "notes");
    return m;
}

QMenu* ToolBar::build_view_menu() {
    auto* m = new QMenu(tr("View"), this);
    m->setStyleSheet(popup_ss());
    m->addAction(tr("Component Browser\tCtrl+K"), this,
                 [this]() { emit action_triggered("browse_components"); });
    m->addSeparator();
    m->addAction(tr("Fullscreen\tF11"), this, [this]() { emit action_triggered("fullscreen"); });
    m->addSeparator();
    m->addAction(tr("Focus Mode\tF10"), this, [this]() { emit action_triggered("focus_mode"); });
    // Not checkable — state lives on WindowFrame::always_on_top_; a checkable QAction would drift on focus changes.
    m->addAction(tr("Always on Top\tCtrl+Shift+T"), this,
                 [this]() { emit action_triggered("always_on_top"); });
    m->addSeparator();

    auto* panels = m->addMenu(tr("Float Panel"));
    panels->setStyleSheet(popup_ss());
    panels->addAction(tr("Dashboard"), this, [this]() { emit action_triggered("panel_dashboard"); });
    panels->addAction(tr("Watchlist"), this, [this]() { emit action_triggered("panel_watchlist"); });
    panels->addAction(tr("News Feed"), this, [this]() { emit action_triggered("panel_news"); });
    panels->addAction(tr("Portfolio"), this, [this]() { emit action_triggered("panel_portfolio"); });
    panels->addAction(tr("Markets"), this, [this]() { emit action_triggered("panel_markets"); });
    panels->addSeparator();
    panels->addAction(tr("Bitcoin"), this, [this]() { emit action_triggered("panel_bitcoin"); });
    panels->addAction(tr("Crypto Trading"), this, [this]() { emit action_triggered("panel_crypto"); });
    panels->addAction(tr("Equity Trading"), this, [this]() { emit action_triggered("panel_equity"); });
    panels->addAction(tr("Algo Trading"), this, [this]() { emit action_triggered("panel_algo"); });
    panels->addSeparator();
    panels->addAction(tr("Equity Research"), this, [this]() { emit action_triggered("panel_research"); });
    panels->addAction(tr("Economics"), this, [this]() { emit action_triggered("panel_economics"); });
    panels->addAction(tr("Geopolitics"), this, [this]() { emit action_triggered("panel_geopolitics"); });
    panels->addAction(tr("AI Chat"), this, [this]() { emit action_triggered("panel_ai_chat"); });
    m->addSeparator();

    auto* persp = m->addMenu(tr("Quick Switch"));
    persp->setStyleSheet(popup_ss());
    persp->addAction(tr("Save Workspace"), this, [this]() { emit action_triggered("perspective_save"); });
    persp->addSeparator();

    auto* qs_trading = persp->addMenu(tr("Trading"));
    qs_trading->setStyleSheet(popup_ss());
    qs_trading->addAction(tr("Crypto Trading"), this, [this]() { emit action_triggered("perspective_trading"); });
    qs_trading->addAction(tr("Bitcoin"), this, [this]() { emit action_triggered("perspective_bitcoin"); });
    qs_trading->addAction(tr("Equity Trading"), this, [this]() { emit action_triggered("perspective_equity"); });
    qs_trading->addAction(tr("Algo Trading"), this, [this]() { emit action_triggered("perspective_algo"); });

    auto* qs_research = persp->addMenu(tr("Research"));
    qs_research->setStyleSheet(popup_ss());
    qs_research->addAction(tr("Equity Research"), this, [this]() { emit action_triggered("perspective_research"); });
    qs_research->addAction(tr("Derivatives"), this, [this]() { emit action_triggered("perspective_derivatives"); });
    qs_research->addAction(tr("M&&A Analytics"), this, [this]() { emit action_triggered("perspective_ma"); });

    persp->addAction(tr("Portfolio View"), this, [this]() { emit action_triggered("perspective_portfolio"); });
    persp->addAction(tr("Markets View"), this, [this]() { emit action_triggered("perspective_markets"); });
    persp->addAction(tr("News View"), this, [this]() { emit action_triggered("perspective_news"); });

    auto* qs_econ = persp->addMenu(tr("Economics && Data"));
    qs_econ->setStyleSheet(popup_ss());
    qs_econ->addAction(tr("Economics"), this, [this]() { emit action_triggered("perspective_economics"); });
    qs_econ->addAction(tr("Data Sources"), this, [this]() { emit action_triggered("perspective_data"); });

    persp->addAction(tr("Geopolitics View"), this, [this]() { emit action_triggered("perspective_geopolitics"); });

    auto* qs_ai = persp->addMenu(tr("AI && Quant"));
    qs_ai->setStyleSheet(popup_ss());
    qs_ai->addAction(tr("Quant Lab"), this, [this]() { emit action_triggered("perspective_quant"); });
    qs_ai->addAction(tr("AI Chat"), this, [this]() { emit action_triggered("perspective_ai"); });

    persp->addAction(tr("Tools View"), this, [this]() { emit action_triggered("perspective_tools"); });
    m->addSeparator();

    m->addAction(tr("Refresh Screen\tF5"), this, [this]() { emit action_triggered("refresh"); });
    m->addAction(tr("Take Screenshot\tCtrl+P"), this, [this]() { emit action_triggered("screenshot"); });
    return m;
}

QMenu* ToolBar::build_help_menu() {
    auto* m = new QMenu(tr("Help"), this);
    m->setStyleSheet(popup_ss());
    m->addAction(tr("Documentation"), this, [this]() { emit navigate_to("docs"); });
    m->addAction(tr("Help Center"), this, [this]() { emit navigate_to("help"); });
    m->addAction(tr("About"), this, [this]() { emit navigate_to("about"); });
    return m;
}

} // namespace openmarketterminal::ui
