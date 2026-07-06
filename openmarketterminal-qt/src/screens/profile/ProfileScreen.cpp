#include "screens/profile/ProfileScreen.h"

#include "auth/AuthManager.h"
#include "core/config/ProfileManager.h"
#include "core/currency/Currency.h"
#include "core/logging/Logger.h"
#include "services/llm/LlmService.h"
#include "storage/repositories/LlmConfigRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/secure/SecureStorage.h"
#include "ui/theme/Theme.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTableWidgetItem>
#include <QTimer>

namespace openmarketterminal::screens {

static const char* MF = "font-family:'Consolas',monospace;";
static QString PANEL_SS() {
    return QString("background:%1;border:1px solid %2;").arg(ui::colors::BG_BASE(), ui::colors::BORDER_DIM());
}
static QString HDR_SS() {
    return QString("background:%1;border-bottom:1px solid %2;").arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM());
}

QWidget* ProfileScreen::make_panel(const QString& title) {
    auto* w = new QWidget(this);
    w->setStyleSheet(PANEL_SS());
    auto* vl = new QVBoxLayout(w);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);
    auto* hdr = new QWidget(this);
    hdr->setFixedHeight(34);
    hdr->setStyleSheet(HDR_SS());
    auto* hl = new QHBoxLayout(hdr);
    hl->setContentsMargins(12, 0, 12, 0);
    auto* t = new QLabel(title);
    t->setStyleSheet(QString("color:%1;font-size:12px;font-weight:700;background:transparent;letter-spacing:0.5px;%2")
                         .arg(ui::colors::AMBER(), MF));
    hl->addWidget(t);
    hl->addStretch();
    vl->addWidget(hdr);
    return w;
}

QWidget* ProfileScreen::make_data_row(const QString& label, QLabel*& value_out) {
    auto* row = new QWidget(this);
    row->setStyleSheet(QString("background:transparent;border-bottom:1px solid %1;").arg(ui::colors::BORDER_DIM()));
    auto* hl = new QHBoxLayout(row);
    hl->setContentsMargins(12, 6, 12, 6);
    auto* lbl = new QLabel(label);
    lbl->setStyleSheet(QString("color:%1;font-size:12px;font-weight:700;background:transparent;letter-spacing:0.5px;%2")
                           .arg(ui::colors::TEXT_SECONDARY(), MF));
    hl->addWidget(lbl);
    hl->addStretch();
    value_out = new QLabel("\xe2\x80\x94");
    value_out->setStyleSheet(
        QString("color:%1;font-size:13px;font-weight:700;background:transparent;%2").arg(ui::colors::TEXT_PRIMARY(), MF));
    hl->addWidget(value_out);
    return row;
}

QWidget* ProfileScreen::make_stat_box(const QString& label, QLabel*& value_out, const QString& color) {
    auto* w = new QWidget(this);
    w->setStyleSheet(QString("background:%1;border:1px solid %2;").arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    auto* vl = new QVBoxLayout(w);
    vl->setContentsMargins(12, 14, 12, 14);
    vl->setAlignment(Qt::AlignCenter);
    value_out = new QLabel("0");
    value_out->setAlignment(Qt::AlignCenter);
    value_out->setStyleSheet(
        QString("color:%1;font-size:28px;font-weight:700;background:transparent;%2").arg(color, MF));
    vl->addWidget(value_out);
    auto* l = new QLabel(label);
    l->setAlignment(Qt::AlignCenter);
    l->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;letter-spacing:0.5px;%2")
                         .arg(ui::colors::TEXT_SECONDARY(), MF));
    vl->addWidget(l);
    return w;
}

ProfileScreen::ProfileScreen(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    build_header(root);
    sections_ = new QStackedWidget;
    sections_->addWidget(build_overview());
    sections_->addWidget(build_security());
    sections_->addWidget(build_automation());
    build_tab_nav(root);
    root->addWidget(sections_, 1);
    connect(&auth::AuthManager::instance(), &auth::AuthManager::auth_state_changed, this, &ProfileScreen::refresh_all);
    refresh_all();
    // Style the first tab as active on load
    on_section_changed(0);
}

void ProfileScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        rebuild_sections();
    }
    QWidget::changeEvent(event);
}

void ProfileScreen::retranslateUi() {
    if (header_title_)       header_title_->setText(tr("LOCAL PROFILE & ACCOUNT"));
    for (int i = 0; i < nav_buttons_.size() && i < nav_source_keys_.size(); ++i) {
        if (nav_buttons_[i])
            nav_buttons_[i]->setText(tr(nav_source_keys_[i].toUtf8().constData()));
    }
}

void ProfileScreen::rebuild_sections() {
    if (!sections_) return;
    const int current = sections_->currentIndex();

    // Build factories in the same order they were originally added so the
    // index remains stable for restore_state() and on_section_changed().
    QList<QWidget*> fresh;
    fresh << build_overview()
          << build_security()
          << build_automation();

    for (int i = 0; i < fresh.size(); ++i) {
        sections_->insertWidget(i, fresh[i]);
        QWidget* old = sections_->widget(i + 1);
        if (old) {
            sections_->removeWidget(old);
            old->deleteLater();
        }
    }
    if (current >= 0 && current < sections_->count())
        sections_->setCurrentIndex(current);
    // Refresh dynamic fields against the new section widgets (overview, usage etc.)
    refresh_all();
}

void ProfileScreen::build_header(QVBoxLayout* root) {
    auto* bar = new QWidget(this);
    bar->setFixedHeight(38);
    bar->setStyleSheet(
        QString("background:%1;border-bottom:1px solid %2;").arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));
    auto* hl = new QHBoxLayout(bar);
    hl->setContentsMargins(14, 0, 14, 0);
    hl->setSpacing(8);
    header_title_ = new QLabel(tr("LOCAL PROFILE & ACCOUNT"));
    header_title_->setStyleSheet(
        QString("color:%1;font-size:14px;font-weight:700;background:transparent;%2").arg(ui::colors::AMBER(), MF));
    hl->addWidget(header_title_);
    hl->addStretch();
    // LOCAL-FIRST FORK: the REFRESH button drove AuthManager::refresh_user_data()
    // (a remote profile/subscription poll) which was removed — there is no
    // remote account to refresh, so the button is gone.
    root->addWidget(bar);
}

void ProfileScreen::build_tab_nav(QVBoxLayout* root) {
    auto* nav = new QWidget(this);
    nav->setFixedHeight(32);
    nav->setStyleSheet(
        QString("background:%1;border-bottom:1px solid %2;").arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    auto* hl = new QHBoxLayout(nav);
    hl->setContentsMargins(4, 0, 4, 0);
    hl->setSpacing(0);
    // Keep English source keys aligned with nav_buttons_ so retranslateUi
    // can reapply tr() without rebuilding the nav bar (preserves the
    // currently-active highlight).
    nav_source_keys_ = {QStringLiteral("OVERVIEW"), QStringLiteral("SECURITY"), QStringLiteral("AUTOMATION")};
    for (int i = 0; i < nav_source_keys_.size(); i++) {
        auto* btn = new QPushButton(tr(nav_source_keys_[i].toUtf8().constData()));
        btn->setFixedHeight(32);
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, [this, i]() { on_section_changed(i); });
        hl->addWidget(btn);
        nav_buttons_.append(btn);
    }
    hl->addStretch();
    root->addWidget(nav);
}

void ProfileScreen::on_section_changed(int index) {
    sections_->setCurrentIndex(index);
    for (int i = 0; i < nav_buttons_.size(); ++i) {
        nav_buttons_[i]->setStyleSheet(
            i == index
                ? QString("QPushButton{background:%1;color:%2;border:none;padding:0 14px;"
                          "font-size:12px;font-weight:700;letter-spacing:0.5px;font-family:'Consolas',monospace;}")
                      .arg(ui::colors::AMBER(), ui::colors::BG_BASE())
                : QString("QPushButton{background:transparent;color:%1;border:none;padding:0 14px;"
                          "font-size:12px;font-weight:700;letter-spacing:0.5px;font-family:'Consolas',monospace;}"
                          "QPushButton:hover{color:%2;}")
                      .arg(ui::colors::TEXT_TERTIARY(), ui::colors::TEXT_SECONDARY()));
    }
}

QWidget* ProfileScreen::build_overview() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea{border:none;background:transparent;}");
    auto* page = new QWidget(this);
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(14, 14, 14, 14);
    vl->setSpacing(10);

    auto* acct = make_panel(tr("LOCAL USER PROFILE"));
    auto* avl = qobject_cast<QVBoxLayout*>(acct->layout());

    auto* note = new QLabel(
        tr("This is your local OpenTerminal identity. It is stored only in this machine profile and is used to personalize the app."));
    note->setWordWrap(true);
    note->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;padding:10px 12px 2px 12px;%2")
                            .arg(ui::colors::TEXT_SECONDARY(), MF));
    avl->addWidget(note);

    auto field_style = []() {
        return QString("QLineEdit{background:%1;color:%2;border:1px solid %3;padding:6px 8px;font-size:13px;%4}"
                       "QLineEdit:focus{border:1px solid %5;}")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
                 ui::colors::AMBER());
    };
    auto label_style = []() {
        return QString("color:%1;font-size:11px;font-weight:700;background:transparent;%2")
            .arg(ui::colors::TEXT_SECONDARY(), MF);
    };

    auto* form = new QWidget(this);
    form->setStyleSheet("background:transparent;");
    auto* grid = new QGridLayout(form);
    grid->setContentsMargins(12, 8, 12, 10);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    auto add_field = [&](const QString& label, QLineEdit*& field, int row, int col, const QString& placeholder) {
        auto* lbl = new QLabel(label);
        lbl->setStyleSheet(label_style());
        field = new QLineEdit;
        field->setPlaceholderText(placeholder);
        field->setStyleSheet(field_style());
        grid->addWidget(lbl, row * 2, col);
        grid->addWidget(field, row * 2 + 1, col);
    };

    add_field(tr("NAME"), profile_name_, 0, 0, tr("Your name"));
    add_field(tr("NICKNAME"), profile_nickname_, 0, 1, tr("How OpenTerminal should address you"));
    add_field(tr("EMAIL"), profile_email_, 1, 0, tr("you@example.com"));
    add_field(tr("PHONE"), profile_phone_, 1, 1, tr("Optional"));
    add_field(tr("COUNTRY"), profile_country_, 2, 0, tr("Optional"));

    auto* active_lbl = new QLabel(tr("ACTIVE APP PROFILE"));
    active_lbl->setStyleSheet(label_style());
    auto* active_val = new QLabel(ProfileManager::instance().active());
    active_val->setStyleSheet(QString("color:%1;font-size:13px;font-weight:700;background:%2;border:1px solid %3;padding:6px 8px;%4")
                                  .arg(ui::colors::AMBER(), ui::colors::BG_RAISED(), ui::colors::BORDER_DIM(), MF));
    grid->addWidget(active_lbl, 4, 1);
    grid->addWidget(active_val, 5, 1);

    avl->addWidget(form);

    auto* action_row = new QWidget(this);
    action_row->setStyleSheet(QString("background:transparent;border-top:1px solid %1;").arg(ui::colors::BORDER_DIM()));
    auto* arl = new QHBoxLayout(action_row);
    arl->setContentsMargins(12, 8, 12, 8);
    arl->setSpacing(10);
    profile_status_ = new QLabel;
    profile_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                       .arg(ui::colors::TEXT_TERTIARY(), MF));
    arl->addWidget(profile_status_, 1);

    auto* save_btn = new QPushButton(tr("SAVE LOCAL PROFILE"));
    save_btn->setFixedHeight(28);
    save_btn->setCursor(Qt::PointingHandCursor);
    save_btn->setStyleSheet(
        QString("QPushButton{background:rgba(217,119,6,0.12);color:%1;border:1px solid rgba(217,119,6,0.4);padding:0 12px;"
                "font-size:11px;font-weight:700;font-family:'Consolas',monospace;}QPushButton:hover{background:%1;color:%2;}")
            .arg(ui::colors::AMBER(), ui::colors::BG_BASE()));
    connect(save_btn, &QPushButton::clicked, this, &ProfileScreen::save_local_profile);
    arl->addWidget(save_btn);
    avl->addWidget(action_row);

    vl->addWidget(acct);
    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

namespace {
LlmConfig find_llm_provider(const QString& provider) {
    auto r = LlmConfigRepository::instance().list_providers();
    if (r.is_ok()) {
        for (const auto& p : r.value()) {
            if (p.provider.compare(provider, Qt::CaseInsensitive) == 0)
                return p;
        }
    }
    LlmConfig c;
    c.provider = provider.toLower();
    return c;
}

QString masked_key_placeholder(const LlmConfig& cfg, const QString& empty_hint) {
    const QString secure_key = ProfileManager::instance().secure_key_prefix() + QStringLiteral("llm.") +
                               cfg.provider.toLower() + QStringLiteral(".api_key");
    const bool has_secure_key = SecureStorage::instance().retrieve(secure_key).is_ok();
    return (cfg.api_key.isEmpty() && !has_secure_key) ? empty_hint
                                                      : QObject::tr("Saved locally - leave blank to keep");
}

void set_combo_text(QComboBox* combo, const QString& text) {
    if (!combo)
        return;
    const int idx = combo->findText(text);
    if (idx >= 0)
        combo->setCurrentIndex(idx);
    else
        combo->setCurrentText(text);
}
} // namespace

QWidget* ProfileScreen::build_ai_accounts() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea{border:none;background:transparent;}");
    auto* page = new QWidget(this);
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(14, 14, 14, 14);
    vl->setSpacing(10);

    auto* notice = make_panel(tr("AI PROVIDER SECURITY"));
    auto* nvl = qobject_cast<QVBoxLayout*>(notice->layout());
    auto* note = new QLabel(
        tr("Choose the AI engine for this local profile. OpenAI and Anthropic keys are encrypted on this machine; "
           "OpenTerminal does not sync, proxy, or upload them. Ollama uses your local endpoint and needs no API key."));
    note->setWordWrap(true);
    note->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;padding:10px 12px;%2")
                            .arg(ui::colors::TEXT_SECONDARY(), MF));
    nvl->addWidget(note);
    auto* profile_row = new QWidget(this);
    profile_row->setStyleSheet(QString("background:transparent;border-top:1px solid %1;").arg(ui::colors::BORDER_DIM()));
    auto* prl = new QHBoxLayout(profile_row);
    prl->setContentsMargins(12, 7, 12, 7);
    auto* pl = new QLabel(tr("ACTIVE LOCAL PROFILE"));
    pl->setStyleSheet(QString("color:%1;font-size:11px;font-weight:700;background:transparent;%2")
                          .arg(ui::colors::TEXT_TERTIARY(), MF));
    auto* pv = new QLabel(ProfileManager::instance().active());
    pv->setStyleSheet(QString("color:%1;font-size:12px;font-weight:700;background:transparent;%2")
                          .arg(ui::colors::AMBER(), MF));
    prl->addWidget(pl);
    prl->addStretch();
    prl->addWidget(pv);
    nvl->addWidget(profile_row);
    vl->addWidget(notice);

    auto field_style = []() {
        return QString("QLineEdit{background:%1;color:%2;border:1px solid %3;padding:5px 8px;font-size:12px;%4}"
                       "QLineEdit:focus{border:1px solid %5;}"
                       "QLineEdit:disabled{color:%6;}")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
                 ui::colors::AMBER(), ui::colors::TEXT_TERTIARY());
    };
    auto combo_style = []() {
        return QString("QComboBox{background:%1;color:%2;border:1px solid %3;padding:5px 8px;font-size:12px;%4}"
                       "QComboBox QAbstractItemView{background:%1;color:%2;selection-background-color:%5;}")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
                 ui::colors::BG_HOVER());
    };
    auto label = [&](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet(QString("color:%1;font-size:11px;font-weight:700;background:transparent;%2")
                             .arg(ui::colors::TEXT_SECONDARY(), MF));
        return l;
    };
    auto button_style = []() {
        return QString("QPushButton{background:rgba(217,119,6,0.12);color:%1;border:1px solid rgba(217,119,6,0.4);"
                       "padding:0 12px;font-size:11px;font-weight:700;font-family:'Consolas',monospace;}"
                       "QPushButton:hover{background:%1;color:%2;}")
            .arg(ui::colors::AMBER(), ui::colors::BG_BASE());
    };

    auto add_provider_panel = [&](const QString& provider, const QString& title, const QString& description,
                                  const QStringList& models, bool local) {
        auto* panel = make_panel(title);
        auto* pvl = qobject_cast<QVBoxLayout*>(panel->layout());

        auto* desc = new QLabel(description);
        desc->setWordWrap(true);
        desc->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;padding:8px 12px 0 12px;%2")
                                .arg(ui::colors::TEXT_SECONDARY(), MF));
        pvl->addWidget(desc);

        auto* form = new QWidget(this);
        form->setStyleSheet("background:transparent;");
        auto* grid = new QGridLayout(form);
        grid->setContentsMargins(12, 8, 12, 8);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(8);

        QLineEdit* key = nullptr;
        QLineEdit* base = nullptr;
        QComboBox* model = nullptr;
        QLabel* status = nullptr;

        int row = 0;
        if (!local) {
            key = new QLineEdit;
            key->setEchoMode(QLineEdit::Password);
            key->setStyleSheet(field_style());
            grid->addWidget(label(tr("API KEY")), row, 0);
            grid->addWidget(key, row, 1, 1, 2);
            ++row;
        }

        model = new QComboBox;
        model->setEditable(true);
        model->addItems(models);
        model->setStyleSheet(combo_style());
        grid->addWidget(label(tr("MODEL")), row, 0);
        grid->addWidget(model, row, 1, 1, 2);
        ++row;

        base = new QLineEdit;
        base->setPlaceholderText(local ? tr("http://localhost:11434") : tr("Optional - leave empty for provider default"));
        base->setStyleSheet(field_style());
        grid->addWidget(label(local ? tr("LOCAL URL") : tr("BASE URL")), row, 0);
        grid->addWidget(base, row, 1, 1, 2);
        ++row;

        auto* save = new QPushButton(tr("SAVE & USE"));
        save->setFixedHeight(26);
        save->setCursor(Qt::PointingHandCursor);
        save->setStyleSheet(button_style());
        connect(save, &QPushButton::clicked, this, [this, provider]() { save_ai_provider(provider); });

        status = new QLabel;
        status->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                  .arg(ui::colors::TEXT_TERTIARY(), MF));
        grid->addWidget(status, row, 0, 1, 2);
        grid->addWidget(save, row, 2, Qt::AlignRight);

        pvl->addWidget(form);
        vl->addWidget(panel);

        if (provider == "openai") {
            ai_openai_key_ = key;
            ai_openai_model_ = model;
            ai_openai_base_url_ = base;
            ai_openai_status_ = status;
        } else if (provider == "anthropic") {
            ai_anthropic_key_ = key;
            ai_anthropic_model_ = model;
            ai_anthropic_base_url_ = base;
            ai_anthropic_status_ = status;
        } else if (provider == "ollama") {
            ai_ollama_model_ = model;
            ai_ollama_base_url_ = base;
            ai_ollama_status_ = status;
        }
    };

    add_provider_panel(QStringLiteral("openai"), tr("OPENAI"),
                       tr("Use your own OpenAI API key for AI Chat, reports, agents, and CLI ask workflows."),
                       {"gpt-4o", "gpt-4o-mini", "gpt-4.1", "o3-mini"}, false);
    add_provider_panel(QStringLiteral("anthropic"), tr("ANTHROPIC"),
                       tr("Use your own Anthropic key. Claude Sonnet is the strongest default for long tool workflows."),
                       {"claude-sonnet-4-5-20250514", "claude-opus-4-5", "claude-3-5-sonnet-20241022",
                        "claude-3-haiku-20240307"},
                       false);
    add_provider_panel(QStringLiteral("ollama"), tr("LOCAL MODEL (OLLAMA)"),
                       tr("Use a local Ollama server. No API key is required; model traffic stays on your local endpoint."),
                       {"llama3.3:70b", "llama3.1:8b", "qwen2.5:14b", "mistral"}, true);

    vl->addStretch();
    scroll->setWidget(page);
    refresh_ai_accounts();
    return scroll;
}

QWidget* ProfileScreen::build_security() {
    return build_ai_accounts();
}

QWidget* ProfileScreen::build_automation() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea{border:none;background:transparent;}");
    auto* page = new QWidget(this);
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(14, 14, 14, 14);
    vl->setSpacing(10);

    auto button_style = []() {
        return QString("QPushButton{background:rgba(217,119,6,0.12);color:%1;border:1px solid rgba(217,119,6,0.4);"
                       "padding:0 12px;font-size:11px;font-weight:700;font-family:'Consolas',monospace;}"
                       "QPushButton:hover{background:%1;color:%2;}"
                       "QPushButton:disabled{color:%3;border-color:%4;background:%5;}")
            .arg(ui::colors::AMBER(), ui::colors::BG_BASE(), ui::colors::TEXT_TERTIARY(),
                 ui::colors::BORDER_DIM(), ui::colors::BG_RAISED());
    };
    auto field_style = []() {
        return QString("QLineEdit{background:%1;color:%2;border:1px solid %3;padding:5px 8px;font-size:12px;%4}"
                       "QLineEdit:focus{border:1px solid %5;}")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
                 ui::colors::AMBER());
    };
    auto combo_style = []() {
        return QString("QComboBox,QSpinBox{background:%1;color:%2;border:1px solid %3;padding:5px 8px;font-size:12px;%4}"
                       "QComboBox QAbstractItemView{background:%1;color:%2;selection-background-color:%5;}")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
                 ui::colors::BG_HOVER());
    };

    auto* status_panel = make_panel(tr("LOCAL DAEMON"));
    auto* svl = qobject_cast<QVBoxLayout*>(status_panel->layout());
    auto* note = new QLabel(
        tr("The daemon keeps this local profile alive for scheduled research, paper strategy tests, notebooks, and notifications. "
           "It runs as your user account and uses the same local AI/provider settings."));
    note->setWordWrap(true);
    note->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;padding:10px 12px 2px 12px;%2")
                            .arg(ui::colors::TEXT_SECONDARY(), MF));
    svl->addWidget(note);

    auto* grid = new QWidget(this);
    grid->setStyleSheet("background:transparent;");
    auto* gl = new QGridLayout(grid);
    gl->setContentsMargins(12, 8, 12, 8);
    gl->setHorizontalSpacing(10);
    gl->setVerticalSpacing(8);
    gl->addWidget(make_stat_box(tr("INSTALLED"), daemon_installed_, ui::colors::AMBER()), 0, 0);
    gl->addWidget(make_stat_box(tr("OWNER"), daemon_owner_, ui::colors::AMBER()), 0, 1);
    gl->addWidget(make_stat_box(tr("SCHEDULER"), daemon_running_, ui::colors::POSITIVE()), 0, 2);
    gl->addWidget(make_stat_box(tr("JOBS"), daemon_jobs_summary_, ui::colors::TEXT_PRIMARY()), 0, 3);
    svl->addWidget(grid);

    auto* controls = new QWidget(this);
    controls->setStyleSheet(QString("background:transparent;border-top:1px solid %1;").arg(ui::colors::BORDER_DIM()));
    auto* cl = new QHBoxLayout(controls);
    cl->setContentsMargins(12, 8, 12, 8);
    cl->setSpacing(8);
    daemon_status_ = new QLabel(tr("Checking daemon..."));
    daemon_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                      .arg(ui::colors::TEXT_TERTIARY(), MF));
    cl->addWidget(daemon_status_, 1);
    auto add_action_button = [&](const QString& text, const QString& action, const QStringList& extra = {}) {
        auto* btn = new QPushButton(text);
        btn->setFixedHeight(26);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(button_style());
        connect(btn, &QPushButton::clicked, this, [this, action, extra]() { run_daemon_action(action, extra); });
        cl->addWidget(btn);
    };
    add_action_button(tr("INSTALL"), QStringLiteral("install"), {QStringLiteral("--replace")});
    add_action_button(tr("START"), QStringLiteral("start"));
    add_action_button(tr("STOP"), QStringLiteral("stop"));
    add_action_button(tr("RESTART"), QStringLiteral("restart"));
    auto* refresh_btn = new QPushButton(tr("REFRESH"));
    refresh_btn->setFixedHeight(26);
    refresh_btn->setCursor(Qt::PointingHandCursor);
    refresh_btn->setStyleSheet(button_style());
    connect(refresh_btn, &QPushButton::clicked, this, &ProfileScreen::refresh_daemon);
    cl->addWidget(refresh_btn);
    svl->addWidget(controls);
    vl->addWidget(status_panel);

    auto* collectors_panel = make_panel(tr("DATA COLLECTORS"));
    auto* cvl = qobject_cast<QVBoxLayout*>(collectors_panel->layout());
    auto* collectors_top = new QWidget(this);
    collectors_top->setStyleSheet("background:transparent;");
    auto* ctl = new QHBoxLayout(collectors_top);
    ctl->setContentsMargins(12, 8, 12, 8);
    ctl->setSpacing(8);
    daemon_collectors_summary_ = new QLabel(tr("Checking collectors..."));
    daemon_collectors_summary_->setStyleSheet(QString("color:%1;font-size:13px;font-weight:700;background:transparent;%2")
                                                  .arg(ui::colors::TEXT_SECONDARY(), MF));
    ctl->addWidget(daemon_collectors_summary_, 1);
    auto add_collector_action = [&](const QString& text, const QString& action) {
        auto* btn = new QPushButton(text);
        btn->setFixedHeight(26);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(button_style());
        connect(btn, &QPushButton::clicked, this, [this, action]() {
            if (daemon_collectors_status_)
                daemon_collectors_status_->setText(tr("Running collectors %1...").arg(action));
            run_daemon_cli({QStringLiteral("collectors"), action},
                           [this, action](const QJsonObject&, const QString&) {
                               if (daemon_collectors_status_)
                                   daemon_collectors_status_->setText(tr("Collectors %1 complete").arg(action));
                               refresh_daemon();
                           },
                           [this](const QString& msg) {
                               if (daemon_collectors_status_)
                                   daemon_collectors_status_->setText(msg);
                               refresh_daemon();
                           });
        });
        ctl->addWidget(btn);
    };
    add_collector_action(tr("REPAIR"), QStringLiteral("repair"));
    add_collector_action(tr("RUN NOW"), QStringLiteral("run"));
    auto* collectors_refresh = new QPushButton(tr("REFRESH"));
    collectors_refresh->setFixedHeight(26);
    collectors_refresh->setCursor(Qt::PointingHandCursor);
    collectors_refresh->setStyleSheet(button_style());
    connect(collectors_refresh, &QPushButton::clicked, this, [this]() {
        run_daemon_cli({QStringLiteral("collectors"), QStringLiteral("status")},
                       [this](const QJsonObject& o, const QString&) { populate_daemon_collectors(o); },
                       [this](const QString& msg) {
                           if (daemon_collectors_status_)
                               daemon_collectors_status_->setText(msg);
                       });
    });
    ctl->addWidget(collectors_refresh);
    cvl->addWidget(collectors_top);

    daemon_collectors_table_ = new QTableWidget;
    daemon_collectors_table_->setColumnCount(6);
    daemon_collectors_table_->setHorizontalHeaderLabels(
        {tr("Name"), tr("State"), tr("Every"), tr("Last"), tr("Runs"), tr("Next")});
    daemon_collectors_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    daemon_collectors_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    daemon_collectors_table_->setAlternatingRowColors(true);
    daemon_collectors_table_->verticalHeader()->setVisible(false);
    daemon_collectors_table_->horizontalHeader()->setStretchLastSection(true);
    daemon_collectors_table_->setMinimumHeight(118);
    daemon_collectors_table_->setStyleSheet(QString("QTableWidget{background:%1;color:%2;border:none;gridline-color:%3;font-size:11px;%4}"
                                                    "QHeaderView::section{background:%5;color:%6;border:1px solid %3;padding:4px;font-size:10px;font-weight:700;%4}"
                                                    "QTableWidget::item{padding:3px 6px;border-bottom:1px solid %3;}"
                                                    "QTableWidget::item:selected{background:rgba(217,119,6,0.18);color:%2;}")
                                            .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
                                                 ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY()));
    cvl->addWidget(daemon_collectors_table_);
    daemon_collectors_status_ = new QLabel;
    daemon_collectors_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;padding:6px 12px;%2")
                                                 .arg(ui::colors::TEXT_TERTIARY(), MF));
    cvl->addWidget(daemon_collectors_status_);
    vl->addWidget(collectors_panel);

    auto* add_panel = make_panel(tr("ADD SCHEDULED JOB"));
    auto* avl = qobject_cast<QVBoxLayout*>(add_panel->layout());
    auto* presets = new QWidget(this);
    presets->setStyleSheet(QString("background:transparent;border-bottom:1px solid %1;").arg(ui::colors::BORDER_DIM()));
    auto* psl = new QHBoxLayout(presets);
    psl->setContentsMargins(12, 8, 12, 8);
    psl->setSpacing(8);
    auto* preset_label = new QLabel(tr("PRESETS"));
    preset_label->setStyleSheet(QString("color:%1;font-size:11px;font-weight:700;background:transparent;%2")
                                    .arg(ui::colors::TEXT_TERTIARY(), MF));
    psl->addWidget(preset_label);
    auto add_preset = [&](const QString& text, const QString& kind, const QString& target, int interval_sec) {
        auto* btn = new QPushButton(text);
        btn->setFixedHeight(24);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(button_style());
        connect(btn, &QPushButton::clicked, this,
                [this, kind, target, interval_sec]() { apply_daemon_job_preset(kind, target, interval_sec); });
        psl->addWidget(btn);
    };
    add_preset(tr("DAILY BRIEF"), QStringLiteral("brief"), QStringLiteral("SPY"), 86400);
    add_preset(tr("NEWS RADAR"), QStringLiteral("radar"), QStringLiteral("AI semiconductors"), 3600);
    add_preset(tr("WEEKLY NOTEBOOK"), QStringLiteral("notebook"), QStringLiteral("trading-sma-crossover-backtest"), 604800);
    add_preset(tr("PAPER WATCH"), QStringLiteral("paper"), QStringLiteral("meanrev"), 300);
    add_preset(tr("HEALTH CHECK"), QStringLiteral("health"), QStringLiteral("daemon health"), 300);
    add_preset(tr("NOTIFY TEST"), QStringLiteral("notify"), QStringLiteral("Daemon is alive"), 3600);
    psl->addStretch();
    avl->addWidget(presets);

    auto* form = new QWidget(this);
    form->setStyleSheet("background:transparent;");
    auto* fl = new QGridLayout(form);
    fl->setContentsMargins(12, 8, 12, 8);
    fl->setHorizontalSpacing(10);
    fl->setVerticalSpacing(8);
    auto label = [&](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet(QString("color:%1;font-size:11px;font-weight:700;background:transparent;%2")
                             .arg(ui::colors::TEXT_SECONDARY(), MF));
        return l;
    };
    daemon_job_kind_ = new QComboBox;
    daemon_job_kind_->addItem(tr("AI Brief"), QStringLiteral("brief"));
    daemon_job_kind_->addItem(tr("AI Radar"), QStringLiteral("radar"));
    daemon_job_kind_->addItem(tr("Risk Review"), QStringLiteral("risk"));
    daemon_job_kind_->addItem(tr("Thesis Monitor"), QStringLiteral("thesis"));
    daemon_job_kind_->addItem(tr("Notebook Run"), QStringLiteral("notebook"));
    daemon_job_kind_->addItem(tr("Paper Strategy"), QStringLiteral("paper"));
    daemon_job_kind_->addItem(tr("Daemon Health Check"), QStringLiteral("health"));
    daemon_job_kind_->addItem(tr("Notification"), QStringLiteral("notify"));
    daemon_job_kind_->setStyleSheet(combo_style());
    daemon_job_target_ = new QLineEdit;
    daemon_job_target_->setPlaceholderText(tr("Ticker, topic, notebook id, or strategy name"));
    daemon_job_target_->setStyleSheet(field_style());
    daemon_job_interval_ = new QSpinBox;
    daemon_job_interval_->setRange(0, 31536000);
    daemon_job_interval_->setValue(86400);
    daemon_job_interval_->setSingleStep(3600);
    daemon_job_interval_->setSuffix(tr(" sec"));
    daemon_job_interval_->setStyleSheet(combo_style());
    auto* add_btn = new QPushButton(tr("ADD JOB"));
    add_btn->setFixedHeight(28);
    add_btn->setCursor(Qt::PointingHandCursor);
    add_btn->setStyleSheet(button_style());
    connect(add_btn, &QPushButton::clicked, this, &ProfileScreen::add_daemon_job);
    daemon_action_status_ = new QLabel;
    daemon_action_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                             .arg(ui::colors::TEXT_TERTIARY(), MF));
    fl->addWidget(label(tr("TYPE")), 0, 0);
    fl->addWidget(label(tr("TARGET")), 0, 1);
    fl->addWidget(label(tr("INTERVAL")), 0, 2);
    fl->addWidget(daemon_job_kind_, 1, 0);
    fl->addWidget(daemon_job_target_, 1, 1);
    fl->addWidget(daemon_job_interval_, 1, 2);
    fl->addWidget(daemon_action_status_, 2, 0, 1, 2);
    fl->addWidget(add_btn, 2, 2, Qt::AlignRight);
    avl->addWidget(form);
    vl->addWidget(add_panel);

    auto* jobs_panel = make_panel(tr("SCHEDULED JOBS"));
    auto* jvl = qobject_cast<QVBoxLayout*>(jobs_panel->layout());
    daemon_jobs_table_ = new QTableWidget;
    daemon_jobs_table_->setColumnCount(6);
    daemon_jobs_table_->setHorizontalHeaderLabels({tr("Name"), tr("Kind"), tr("Enabled"), tr("Every"), tr("Last"), tr("Command")});
    daemon_jobs_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    daemon_jobs_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    daemon_jobs_table_->setAlternatingRowColors(true);
    daemon_jobs_table_->verticalHeader()->setVisible(false);
    daemon_jobs_table_->horizontalHeader()->setStretchLastSection(true);
    daemon_jobs_table_->setMinimumHeight(220);
    daemon_jobs_table_->setStyleSheet(QString("QTableWidget{background:%1;color:%2;border:none;gridline-color:%3;font-size:11px;%4}"
                                              "QHeaderView::section{background:%5;color:%6;border:1px solid %3;padding:4px;font-size:10px;font-weight:700;%4}"
                                              "QTableWidget::item{padding:3px 6px;border-bottom:1px solid %3;}"
                                              "QTableWidget::item:selected{background:rgba(217,119,6,0.18);color:%2;}")
                                      .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
                                           ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY()));
    connect(daemon_jobs_table_, &QTableWidget::itemSelectionChanged, this,
            &ProfileScreen::refresh_selected_daemon_job_detail);
    jvl->addWidget(daemon_jobs_table_);
    auto* job_controls = new QWidget(this);
    job_controls->setStyleSheet(QString("background:transparent;border-top:1px solid %1;").arg(ui::colors::BORDER_DIM()));
    auto* jcl = new QHBoxLayout(job_controls);
    jcl->setContentsMargins(12, 8, 12, 8);
    jcl->setSpacing(8);
    jcl->addStretch();
    auto add_job_action = [&](const QString& text, const QString& action) {
        auto* btn = new QPushButton(text);
        btn->setFixedHeight(26);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(button_style());
        connect(btn, &QPushButton::clicked, this, [this, action]() { run_selected_daemon_job_action(action); });
        jcl->addWidget(btn);
    };
    add_job_action(tr("RUN"), QStringLiteral("run"));
    add_job_action(tr("ENABLE"), QStringLiteral("enable"));
    add_job_action(tr("DISABLE"), QStringLiteral("disable"));
    add_job_action(tr("REMOVE"), QStringLiteral("remove"));
    jvl->addWidget(job_controls);

    daemon_job_detail_ = new QPlainTextEdit;
    daemon_job_detail_->setReadOnly(true);
    daemon_job_detail_->setMinimumHeight(140);
    daemon_job_detail_->setPlaceholderText(tr("Select a job to inspect its run state, command, and last output."));
    daemon_job_detail_->setStyleSheet(QString("QPlainTextEdit{background:%1;color:%2;border-top:1px solid %3;padding:8px;font-size:11px;%4}"
                                             "QPlainTextEdit:focus{border-top:1px solid %3;}")
                                         .arg(ui::colors::BG_BASE(), ui::colors::TEXT_SECONDARY(),
                                              ui::colors::BORDER_DIM(), MF));
    jvl->addWidget(daemon_job_detail_);
    vl->addWidget(jobs_panel, 1);

    auto* audit_panel = make_panel(tr("SAFETY AUDIT"));
    auto* auvl = qobject_cast<QVBoxLayout*>(audit_panel->layout());
    daemon_audit_status_ = new QLabel(tr("Checking daemon safety..."));
    daemon_audit_status_->setWordWrap(true);
    daemon_audit_status_->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;padding:10px 12px;%2")
                                            .arg(ui::colors::TEXT_SECONDARY(), MF));
    auvl->addWidget(daemon_audit_status_);
    vl->addWidget(audit_panel);

    auto* logs_panel = make_panel(tr("RECENT DAEMON LOG"));
    auto* lvl = qobject_cast<QVBoxLayout*>(logs_panel->layout());
    daemon_logs_ = new QPlainTextEdit;
    daemon_logs_->setReadOnly(true);
    daemon_logs_->setMinimumHeight(160);
    daemon_logs_->setPlaceholderText(tr("No daemon job log data yet."));
    daemon_logs_->setStyleSheet(QString("QPlainTextEdit{background:%1;color:%2;border:none;padding:8px;font-size:11px;%3}"
                                        "QPlainTextEdit:focus{border:none;}")
                                    .arg(ui::colors::BG_BASE(), ui::colors::TEXT_SECONDARY(), MF));
    lvl->addWidget(daemon_logs_);
    auto* log_controls = new QWidget(this);
    log_controls->setStyleSheet(QString("background:transparent;border-top:1px solid %1;").arg(ui::colors::BORDER_DIM()));
    auto* lcl = new QHBoxLayout(log_controls);
    lcl->setContentsMargins(12, 8, 12, 8);
    lcl->addStretch();
    auto* log_refresh = new QPushButton(tr("REFRESH LOG"));
    log_refresh->setFixedHeight(26);
    log_refresh->setCursor(Qt::PointingHandCursor);
    log_refresh->setStyleSheet(button_style());
    connect(log_refresh, &QPushButton::clicked, this, [this]() {
        run_daemon_cli({QStringLiteral("logs"), QStringLiteral("jobs"), QStringLiteral("--lines"), QStringLiteral("80")},
                       [this](const QJsonObject& o, const QString&) { populate_daemon_logs(o); },
                       [this](const QString& msg) {
                           if (daemon_logs_)
                               daemon_logs_->setPlainText(msg);
                       });
    });
    lcl->addWidget(log_refresh);
    lvl->addWidget(log_controls);
    vl->addWidget(logs_panel);

    scroll->setWidget(page);
    refresh_daemon();
    return scroll;
}

QString ProfileScreen::daemon_cli_path() const {
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QString exe = QStringLiteral("openterminalcli");
    const QStringList candidates = {
        app_dir + QDir::separator() + exe,
        QDir::cleanPath(app_dir + QStringLiteral("/../bin/") + exe),
        QDir::cleanPath(app_dir + QStringLiteral("/../../../") + exe),
        QDir::cleanPath(app_dir + QStringLiteral("/../../../bin/") + exe),
        QStandardPaths::findExecutable(exe),
        QStandardPaths::findExecutable(QStringLiteral("ot"))
    };
    for (const QString& path : candidates) {
        if (!path.isEmpty() && QFileInfo::exists(path) && QFileInfo(path).isExecutable())
            return path;
    }
    return {};
}

void ProfileScreen::run_daemon_cli(const QStringList& daemon_args,
                                   std::function<void(const QJsonObject&, const QString&)> on_success,
                                   std::function<void(const QString&)> on_error) {
    const QString cli = daemon_cli_path();
    if (cli.isEmpty()) {
        const QString msg = tr("openterminalcli not found next to the app or in PATH");
        if (on_error) on_error(msg);
        else if (daemon_status_) daemon_status_->setText(msg);
        return;
    }

    auto* proc = new QProcess(this);
    QStringList args{QStringLiteral("--json"), QStringLiteral("--profile"), ProfileManager::instance().active(),
                     QStringLiteral("daemon")};
    args << daemon_args;
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc, on_success, on_error](int code, QProcess::ExitStatus status) {
                const QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
                const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
                proc->deleteLater();
                QJsonParseError parse;
                const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8(), &parse);
                const bool ok = status == QProcess::NormalExit && code == 0 && parse.error == QJsonParseError::NoError && doc.isObject();
                if (ok) {
                    if (on_success) on_success(doc.object(), out);
                    return;
                }
                QString msg = err.isEmpty() ? out : err;
                if (msg.isEmpty())
                    msg = tr("daemon command failed");
                if (on_error) on_error(msg);
                else if (daemon_status_) daemon_status_->setText(msg);
            });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, on_error](QProcess::ProcessError) {
        const QString msg = tr("could not start openterminalcli");
        proc->deleteLater();
        if (on_error) on_error(msg);
        else if (daemon_status_) daemon_status_->setText(msg);
    });
    proc->start(cli, args);
}

void ProfileScreen::run_daemon_action(const QString& action, const QStringList& extra_args) {
    if (daemon_status_)
        daemon_status_->setText(tr("Running daemon %1...").arg(action));
    QStringList args{action};
    args << extra_args;
    run_daemon_cli(args,
                   [this, action](const QJsonObject&, const QString&) {
                       if (daemon_status_)
                           daemon_status_->setText(tr("Daemon %1 complete").arg(action));
                       refresh_daemon();
                   },
                   [this](const QString& msg) {
                       if (daemon_status_) {
                           daemon_status_->setText(msg);
                           daemon_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                                             .arg(ui::colors::NEGATIVE(), MF));
                       }
                       refresh_daemon();
                   });
}

void ProfileScreen::populate_daemon_health(const QJsonObject& health) {
    auto set = [](QLabel* l, const QString& text, const QString& color) {
        if (!l) return;
        l->setText(text);
        l->setStyleSheet(QString("color:%1;font-size:28px;font-weight:700;background:transparent;%2").arg(color, MF));
    };
    const bool installed = health.value("installed").toBool();
    const bool scheduler_running = health.value("scheduler_running").toBool(
        health.value("daemon_running").toBool(health.value("running").toBool()));
    const bool daemon_bridge_owner = health.value("daemon_bridge_owner").toBool(
        health.value("active_owner_kind").toString() == QStringLiteral("daemon"));
    const bool owner_live = health.value("profile_owner_reachable").toBool();
    const QString owner = health.value("active_owner_kind").toString(
        health.value("owner_kind").toString()).trimmed().toLower();
    const QJsonObject jobs = health.value("jobs").toObject();
    set(daemon_installed_, installed ? tr("YES") : tr("NO"), installed ? ui::colors::POSITIVE() : ui::colors::TEXT_TERTIARY());
    QString owner_text = tr("NONE");
    QString owner_color = ui::colors::TEXT_TERTIARY();
    if (owner_live) {
        owner_text = owner.isEmpty() ? tr("OTHER") : owner.toUpper();
        owner_color = owner == QStringLiteral("daemon") ? ui::colors::POSITIVE()
                    : owner == QStringLiteral("gui") ? ui::colors::AMBER()
                                                     : ui::colors::TEXT_PRIMARY();
    }
    set(daemon_owner_, owner_text, owner_color);
    set(daemon_running_, scheduler_running ? tr("ACTIVE") : tr("STANDBY"),
        scheduler_running ? ui::colors::POSITIVE() : ui::colors::TEXT_TERTIARY());
    set(daemon_jobs_summary_, QString::number(jobs.value("total").toInt()), ui::colors::TEXT_PRIMARY());
    if (daemon_status_) {
        const QString endpoint = health.value("endpoint").toString();
        const QString profile = health.value("profile").toString();
        if (daemon_bridge_owner) {
            daemon_status_->setText(endpoint.isEmpty()
                                        ? tr("Sync: daemon owns profile '%1'; scheduler and unattended jobs are active").arg(profile)
                                        : tr("Sync: daemon owns profile '%1' at %2; scheduler and unattended jobs are active")
                                              .arg(profile, endpoint));
        } else if (scheduler_running && owner == QStringLiteral("gui") && !endpoint.isEmpty()) {
            daemon_status_->setText(
                tr("Sync: GUI owns profile '%1' at %2; daemon scheduler is warm and background jobs can run")
                    .arg(profile, endpoint));
        } else if (owner_live && !endpoint.isEmpty()) {
            if (owner == QStringLiteral("gui")) {
                daemon_status_->setText(
                    tr("Sync: GUI owns profile '%1' at %2; CLI can attach/read, daemon scheduler can be started warm")
                        .arg(profile, endpoint));
            } else {
                daemon_status_->setText(tr("Sync: profile '%1' is owned by %2 at %3; daemon scheduler is standby")
                                            .arg(profile, owner.toUpper(), endpoint));
            }
        } else {
            daemon_status_->setText(
                tr("Sync: no active owner for profile '%1'; start the GUI for interactive work or daemon for unattended jobs")
                    .arg(profile));
        }
        daemon_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                          .arg(scheduler_running ? ui::colors::POSITIVE() : ui::colors::TEXT_TERTIARY(), MF));
    }
}

void ProfileScreen::populate_daemon_jobs(const QJsonArray& jobs) {
    if (!daemon_jobs_table_)
        return;
    daemon_jobs_table_->setRowCount(jobs.size());
    for (int r = 0; r < jobs.size(); ++r) {
        const QJsonObject j = jobs.at(r).toObject();
        const QString id = j.value("id").toString();
        const QStringList command = [&]() {
            QStringList out;
            for (const QJsonValue& v : j.value("command").toArray())
                out << v.toString();
            return out;
        }();
        const QString every = j.value("interval_sec").toInt() > 0
                                  ? tr("%1s").arg(j.value("interval_sec").toInt())
                                  : tr("manual");
        const QString last = j.value("last_status").toString(QStringLiteral("-"));
        const QStringList cols = {
            j.value("name").toString(),
            j.value("kind").toString(),
            j.value("enabled").toBool() ? tr("yes") : tr("no"),
            every,
            last,
            command.join(' ')
        };
        for (int c = 0; c < cols.size(); ++c) {
            auto* item = new QTableWidgetItem(cols.at(c));
            item->setData(Qt::UserRole, id);
            if (c == 2)
                item->setForeground(j.value("enabled").toBool() ? QColor(ui::colors::POSITIVE()) : QColor(ui::colors::TEXT_TERTIARY()));
            daemon_jobs_table_->setItem(r, c, item);
        }
    }
    daemon_jobs_table_->resizeColumnsToContents();
    daemon_jobs_table_->horizontalHeader()->setStretchLastSection(true);
    if (jobs.isEmpty()) {
        if (daemon_job_detail_)
            daemon_job_detail_->setPlainText({});
    } else if (daemon_jobs_table_->selectionModel() && daemon_jobs_table_->selectionModel()->selectedRows().isEmpty()) {
        daemon_jobs_table_->selectRow(0);
    } else {
        refresh_selected_daemon_job_detail();
    }
}

void ProfileScreen::populate_daemon_collectors(const QJsonObject& collectors) {
    const int total = collectors.value("total").toInt();
    const int found = collectors.value("found").toInt();
    const int enabled = collectors.value("enabled").toInt();
    const int failed = collectors.value("failed").toInt();
    const int stale = collectors.value("stale").toInt();
    const bool ok = total > 0 && found == total && enabled == total && failed == 0 && stale == 0;
    if (daemon_collectors_summary_) {
        daemon_collectors_summary_->setText(
            ok ? tr("COLLECTORS READY  %1/%2").arg(found).arg(total)
               : tr("COLLECTORS NEED ATTENTION  %1/%2  failed %3  stale %4").arg(found).arg(total).arg(failed).arg(stale));
        daemon_collectors_summary_->setStyleSheet(
            QString("color:%1;font-size:13px;font-weight:700;background:transparent;%2")
                .arg(ok ? ui::colors::POSITIVE() : ui::colors::AMBER(), MF));
    }

    const QJsonArray rows = collectors.value("collectors").toArray();
    if (daemon_collectors_table_) {
        daemon_collectors_table_->setRowCount(rows.size());
        for (int r = 0; r < rows.size(); ++r) {
            const QJsonObject row = rows.at(r).toObject();
            QString state = row.value("status").toString(QStringLiteral("missing"));
            if (state == QStringLiteral("found")) {
                if (row.value("running").toBool())
                    state = QStringLiteral("running");
                else
                    state = row.value("last_status").toString(QStringLiteral("-"));
            }
            const QString every = row.value("interval_sec").toInt() > 0
                                      ? tr("%1s").arg(row.value("interval_sec").toInt())
                                      : tr("manual");
            const QStringList cols = {
                row.value("name").toString(),
                state,
                every,
                row.value("last_status").toString(QStringLiteral("-")),
                QString::number(row.value("run_count").toInt()),
                row.value("next_run_at").toString(QStringLiteral("-"))
            };
            const QColor state_color =
                state == QStringLiteral("ok") ? QColor(ui::colors::POSITIVE())
                : state == QStringLiteral("running") ? QColor(ui::colors::AMBER())
                : state == QStringLiteral("missing") || state == QStringLiteral("failed") || state == QStringLiteral("timeout")
                    ? QColor(ui::colors::NEGATIVE())
                    : QColor(ui::colors::TEXT_SECONDARY());
            for (int c = 0; c < cols.size(); ++c) {
                auto* item = new QTableWidgetItem(cols.at(c));
                item->setData(Qt::UserRole, row.value("id").toString());
                if (c == 1 || c == 3)
                    item->setForeground(state_color);
                daemon_collectors_table_->setItem(r, c, item);
            }
        }
        daemon_collectors_table_->resizeColumnsToContents();
        daemon_collectors_table_->horizontalHeader()->setStretchLastSection(true);
    }

    if (daemon_collectors_status_) {
        daemon_collectors_status_->setText(
            ok ? tr("BTC ticks and prediction snapshots are being kept warm.")
               : tr("Use REPAIR to recreate missing managed feed jobs, or RUN NOW to force an immediate refresh."));
        daemon_collectors_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;padding:6px 12px;%2")
                                                    .arg(ok ? ui::colors::TEXT_SECONDARY() : ui::colors::AMBER(), MF));
    }
}

void ProfileScreen::populate_daemon_audit(const QJsonObject& audit) {
    if (!daemon_audit_status_)
        return;
    const bool local_keys = !audit.value("api_keys_leave_machine").toBool();
    const bool no_live = !audit.value("unattended_live_trading").toBool();
    const bool no_destructive = !audit.value("daemon_bridge_destructive_tools").toBool();
    const bool no_settings = !audit.value("daemon_settings_write_tools").toBool();
    const bool ok = local_keys && no_live && no_destructive && no_settings;
    QStringList guardrails;
    for (const QJsonValue& v : audit.value("guardrails").toArray())
        guardrails << v.toString();
    daemon_audit_status_->setText(
        tr("%1\nAPI keys leave machine: %2    unattended live trading: %3    destructive bridge tools: %4\n%5")
            .arg(ok ? tr("Safety status: OK") : tr("Safety status: review needed"),
                 local_keys ? tr("no") : tr("yes"),
                 no_live ? tr("no") : tr("yes"),
                 no_destructive ? tr("no") : tr("yes"),
                 guardrails.join(QStringLiteral("  |  "))));
    daemon_audit_status_->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;padding:10px 12px;%2")
                                            .arg(ok ? ui::colors::POSITIVE() : ui::colors::NEGATIVE(), MF));
}

void ProfileScreen::populate_daemon_logs(const QJsonObject& logs) {
    if (!daemon_logs_)
        return;
    const QString text = logs.value("text").toString().trimmed();
    daemon_logs_->setPlainText(text.isEmpty() ? tr("No daemon job log data yet.") : text);
}

void ProfileScreen::populate_daemon_job_detail(const QJsonObject& job) {
    if (!daemon_job_detail_)
        return;
    QStringList command;
    for (const QJsonValue& v : job.value("command").toArray())
        command << v.toString();
    const QJsonObject spec = job.value("spec").toObject();
    const QString spec_text = spec.isEmpty()
                                  ? QStringLiteral("{}")
                                  : QString::fromUtf8(QJsonDocument(spec).toJson(QJsonDocument::Compact));
    const QString output = job.value("last_output_tail").toString().trimmed();
    QStringList lines;
    lines << tr("id: %1").arg(job.value("id").toString())
          << tr("name: %1").arg(job.value("name").toString())
          << tr("kind: %1").arg(job.value("kind").toString())
          << tr("enabled: %1    schedule: %2    interval: %3s")
                 .arg(job.value("enabled").toBool() ? tr("yes") : tr("no"),
                      job.value("schedule").toString(QStringLiteral("-")),
                      QString::number(job.value("interval_sec").toInt()))
          << tr("status: %1    runs: %2    failures: %3    exit: %4")
                 .arg(job.value("last_status").toString(QStringLiteral("-")),
                      QString::number(job.value("run_count").toInt()),
                      QString::number(job.value("fail_count").toInt()),
                      job.contains("last_exit_code") ? QString::number(job.value("last_exit_code").toInt()) : QStringLiteral("-"))
          << tr("next: %1").arg(job.value("next_run_at").toString(QStringLiteral("-")))
          << tr("last started: %1").arg(job.value("last_started_at").toString(QStringLiteral("-")))
          << tr("last run: %1").arg(job.value("last_run_at").toString(QStringLiteral("-")))
          << tr("command: %1").arg(command.join(' '))
          << tr("spec: %1").arg(spec_text);
    if (!output.isEmpty())
        lines << QString() << tr("last output:") << output;
    daemon_job_detail_->setPlainText(lines.join('\n'));
}

void ProfileScreen::refresh_selected_daemon_job_detail() {
    const QString id = selected_daemon_job_id();
    if (id.isEmpty()) {
        if (daemon_job_detail_)
            daemon_job_detail_->setPlainText({});
        return;
    }
    run_daemon_cli({QStringLiteral("jobs"), QStringLiteral("show"), id},
                   [this](const QJsonObject& job, const QString&) { populate_daemon_job_detail(job); },
                   [this](const QString& msg) {
                       if (daemon_job_detail_)
                           daemon_job_detail_->setPlainText(msg);
                   });
}

QString ProfileScreen::selected_daemon_job_id() const {
    if (!daemon_jobs_table_)
        return {};
    const auto selected = daemon_jobs_table_->selectionModel() ? daemon_jobs_table_->selectionModel()->selectedRows() : QModelIndexList{};
    if (selected.isEmpty())
        return {};
    const int row = selected.first().row();
    auto* item = daemon_jobs_table_->item(row, 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void ProfileScreen::run_selected_daemon_job_action(const QString& action) {
    const QString id = selected_daemon_job_id();
    if (id.isEmpty()) {
        if (daemon_action_status_)
            daemon_action_status_->setText(tr("Select a job first"));
        return;
    }
    if (daemon_action_status_)
        daemon_action_status_->setText(tr("Running job %1...").arg(action));
    run_daemon_cli({QStringLiteral("jobs"), action, id},
                   [this, action](const QJsonObject&, const QString&) {
                       if (daemon_action_status_)
                           daemon_action_status_->setText(tr("Job %1 complete").arg(action));
                       refresh_daemon();
                   },
                   [this](const QString& msg) {
                       if (daemon_action_status_)
                           daemon_action_status_->setText(msg);
                       refresh_daemon();
                   });
}

void ProfileScreen::apply_daemon_job_preset(const QString& kind, const QString& target, int interval_sec) {
    if (!daemon_job_kind_ || !daemon_job_target_ || !daemon_job_interval_)
        return;
    const int idx = daemon_job_kind_->findData(kind);
    if (idx >= 0)
        daemon_job_kind_->setCurrentIndex(idx);
    daemon_job_target_->setText(target);
    daemon_job_interval_->setValue(interval_sec);
    if (daemon_action_status_)
        daemon_action_status_->setText(tr("Preset loaded"));
}

void ProfileScreen::refresh_daemon() {
    if (!daemon_status_ && !daemon_jobs_table_)
        return;
    if (daemon_status_)
        daemon_status_->setText(tr("Checking daemon..."));
    run_daemon_cli({QStringLiteral("health")},
                   [this](const QJsonObject& health, const QString&) {
                       populate_daemon_health(health);
                       run_daemon_cli({QStringLiteral("jobs"), QStringLiteral("list")},
                                      [this](const QJsonObject& o, const QString&) {
                                          populate_daemon_jobs(o.value("jobs").toArray());
                                      });
                       run_daemon_cli({QStringLiteral("collectors"), QStringLiteral("status")},
                                      [this](const QJsonObject& o, const QString&) { populate_daemon_collectors(o); },
                                      [this](const QString& msg) {
                                          if (daemon_collectors_status_)
                                              daemon_collectors_status_->setText(msg);
                                      });
                       run_daemon_cli({QStringLiteral("audit")},
                                      [this](const QJsonObject& o, const QString&) { populate_daemon_audit(o); });
                       run_daemon_cli({QStringLiteral("logs"), QStringLiteral("jobs"), QStringLiteral("--lines"), QStringLiteral("80")},
                                      [this](const QJsonObject& o, const QString&) { populate_daemon_logs(o); },
                                      [this](const QString& msg) {
                                          if (daemon_logs_)
                                              daemon_logs_->setPlainText(msg);
                                      });
                   },
                   [this](const QString& msg) {
                       if (daemon_status_) {
                           daemon_status_->setText(msg);
                           daemon_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                                             .arg(ui::colors::NEGATIVE(), MF));
                       }
                   });
}

void ProfileScreen::add_daemon_job() {
    if (!daemon_job_kind_ || !daemon_job_target_ || !daemon_job_interval_)
        return;
    const QString kind = daemon_job_kind_->currentData().toString();
    const QString target = daemon_job_target_->text().trimmed();
    if (target.isEmpty() && kind != "health") {
        if (daemon_action_status_)
            daemon_action_status_->setText(tr("Target required"));
        return;
    }
    const int interval = daemon_job_interval_->value();
    QStringList args;
    if (kind == "notebook") {
        args << QStringLiteral("jobs") << QStringLiteral("add") << QStringLiteral("notebook") << target;
    } else if (kind == "paper") {
        args << QStringLiteral("paper") << target;
    } else if (kind == "health") {
        args << QStringLiteral("jobs") << QStringLiteral("add") << QStringLiteral("health-check");
    } else if (kind == "notify") {
        args << QStringLiteral("notify") << QStringLiteral("--job")
             << QStringLiteral("--title") << QStringLiteral("OpenTerminal")
             << QStringLiteral("--message") << target;
    } else {
        args << QStringLiteral("ai") << kind << target;
    }
    if (interval > 0)
        args << QStringLiteral("--every-sec") << QString::number(interval);
    if (daemon_action_status_)
        daemon_action_status_->setText(tr("Adding job..."));
    run_daemon_cli(args,
                   [this](const QJsonObject&, const QString&) {
                       if (daemon_action_status_)
                           daemon_action_status_->setText(tr("Job added"));
                       if (daemon_job_target_)
                           daemon_job_target_->clear();
                       refresh_daemon();
                   },
                   [this](const QString& msg) {
                       if (daemon_action_status_)
                           daemon_action_status_->setText(msg);
                   });
}

void ProfileScreen::refresh_all() {
    // LOCAL-FIRST: populate Overview from local SettingsRepository (no login required).
    auto& repo = openmarketterminal::SettingsRepository::instance();
    const auto r_name    = repo.get("profile.name",     "");
    const auto r_nick    = repo.get("profile.nickname", "");
    const auto r_uname   = repo.get("profile.username", "");
    const auto r_email   = repo.get("profile.email",    "");
    const auto r_phone   = repo.get("profile.phone",    "");
    const auto r_country = repo.get("profile.country",  "");
    const QString name    = (r_name.is_ok()    && !r_name.value().isEmpty())    ? r_name.value()    : QString();
    const QString nick    = (r_nick.is_ok()    && !r_nick.value().isEmpty())    ? r_nick.value()    : QString();
    const QString legacy  = (r_uname.is_ok()   && !r_uname.value().isEmpty())   ? r_uname.value()   : QString();
    const QString email   = (r_email.is_ok()   && !r_email.value().isEmpty())   ? r_email.value()   : QString();
    const QString phone   = (r_phone.is_ok()   && !r_phone.value().isEmpty())   ? r_phone.value()   : QString();
    const QString country = (r_country.is_ok() && !r_country.value().isEmpty()) ? r_country.value() : QString();

    const QString display = !nick.isEmpty() ? nick : (!name.isEmpty() ? name : legacy);
    if (username_header_)
        username_header_->setText(display.isEmpty() ? tr("Local Profile") : display);

    if (profile_name_)
        profile_name_->setText(name.isEmpty() ? legacy : name);
    if (profile_nickname_)
        profile_nickname_->setText(nick);
    if (profile_email_)
        profile_email_->setText(email);
    if (profile_phone_)
        profile_phone_->setText(phone);
    if (profile_country_)
        profile_country_->setText(country);
    if (profile_status_)
        profile_status_->setText(tr("Stored locally in profile '%1'").arg(ProfileManager::instance().active()));

    refresh_ai_accounts();
    refresh_daemon();
}

void ProfileScreen::refresh_ai_accounts() {
    auto apply_cloud = [&](const QString& provider, QLineEdit* key, QComboBox* model, QLineEdit* base,
                           QLabel* status, const QString& default_model) {
        if (!model || !base || !status)
            return;
        const LlmConfig cfg = find_llm_provider(provider);
        const QString secure_key = ProfileManager::instance().secure_key_prefix() + QStringLiteral("llm.") +
                                   provider.toLower() + QStringLiteral(".api_key");
        const bool has_key = !cfg.api_key.isEmpty() || SecureStorage::instance().retrieve(secure_key).is_ok();
        if (key) {
            key->clear();
            key->setPlaceholderText(masked_key_placeholder(cfg, tr("Paste API key - stored in this local profile")));
        }
        set_combo_text(model, cfg.model.isEmpty() ? default_model : cfg.model);
        base->setText(cfg.base_url);
        const QString state = has_key ? tr("Configured locally") : tr("Not configured");
        status->setText(cfg.is_active ? tr("Active - %1").arg(state) : state);
        status->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                  .arg(cfg.is_active ? ui::colors::POSITIVE() : ui::colors::TEXT_TERTIARY(), MF));
    };
    apply_cloud(QStringLiteral("openai"), ai_openai_key_, ai_openai_model_, ai_openai_base_url_, ai_openai_status_,
                QStringLiteral("gpt-4o"));
    apply_cloud(QStringLiteral("anthropic"), ai_anthropic_key_, ai_anthropic_model_, ai_anthropic_base_url_,
                ai_anthropic_status_, QStringLiteral("claude-sonnet-4-5-20250514"));

    if (ai_ollama_model_ && ai_ollama_base_url_ && ai_ollama_status_) {
        const LlmConfig cfg = find_llm_provider(QStringLiteral("ollama"));
        set_combo_text(ai_ollama_model_, cfg.model.isEmpty() ? QStringLiteral("llama3.3:70b") : cfg.model);
        ai_ollama_base_url_->setText(cfg.base_url.isEmpty() ? QStringLiteral("http://localhost:11434") : cfg.base_url);
        ai_ollama_status_->setText(cfg.is_active ? tr("Active - local endpoint") : tr("Local endpoint"));
        ai_ollama_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                             .arg(cfg.is_active ? ui::colors::POSITIVE() : ui::colors::TEXT_TERTIARY(),
                                                  MF));
    }
}

void ProfileScreen::save_ai_provider(const QString& provider) {
    const QString id = provider.toLower();
    LlmConfig prior = find_llm_provider(id);
    LlmConfig cfg;
    cfg.provider = id;
    cfg.tools_enabled = true;
    cfg.is_active = true;

    QString new_key;
    if (id == "openai") {
        new_key = ai_openai_key_ ? ai_openai_key_->text().trimmed() : QString();
        cfg.api_key.clear();
        cfg.model = ai_openai_model_ ? ai_openai_model_->currentText().trimmed() : QString();
        cfg.base_url = ai_openai_base_url_ ? ai_openai_base_url_->text().trimmed() : QString();
    } else if (id == "anthropic") {
        new_key = ai_anthropic_key_ ? ai_anthropic_key_->text().trimmed() : QString();
        cfg.api_key.clear();
        cfg.model = ai_anthropic_model_ ? ai_anthropic_model_->currentText().trimmed() : QString();
        cfg.base_url = ai_anthropic_base_url_ ? ai_anthropic_base_url_->text().trimmed() : QString();
    } else if (id == "ollama") {
        cfg.api_key.clear();
        cfg.model = ai_ollama_model_ ? ai_ollama_model_->currentText().trimmed() : QString();
        cfg.base_url = ai_ollama_base_url_ ? ai_ollama_base_url_->text().trimmed() : QStringLiteral("http://localhost:11434");
    } else {
        return;
    }

    QLabel* status = id == "openai" ? ai_openai_status_ : (id == "anthropic" ? ai_anthropic_status_ : ai_ollama_status_);
    auto show = [&](const QString& msg, bool error) {
        if (!status)
            return;
        status->setText(msg);
        status->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                  .arg(error ? ui::colors::NEGATIVE() : ui::colors::POSITIVE(), MF));
    };

    const QString secure_key = ProfileManager::instance().secure_key_prefix() + QStringLiteral("llm.") + id +
                               QStringLiteral(".api_key");
    const bool has_saved_key = SecureStorage::instance().retrieve(secure_key).is_ok() || !prior.api_key.isEmpty();
    if (id != "ollama" && new_key.isEmpty() && !has_saved_key) {
        show(tr("API key required"), true);
        return;
    }
    if (cfg.model.isEmpty()) {
        show(tr("Model required"), true);
        return;
    }

    if (id != "ollama") {
        const QString key_to_store = new_key.isEmpty() ? prior.api_key : new_key;
        if (!key_to_store.isEmpty()) {
            auto stored = SecureStorage::instance().store(secure_key, key_to_store);
            if (stored.is_err()) {
                show(tr("Secure key save failed"), true);
                LOG_ERROR("Profile", "AI provider secure key save failed: " + QString::fromStdString(stored.error()));
                return;
            }
        }
    }

    auto saved = LlmConfigRepository::instance().save_provider(cfg);
    if (saved.is_err()) {
        show(tr("Save failed"), true);
        LOG_ERROR("Profile", "AI provider save failed: " + QString::fromStdString(saved.error()));
        return;
    }
    auto active = LlmConfigRepository::instance().set_active(id);
    if (active.is_err()) {
        show(tr("Activation failed"), true);
        LOG_ERROR("Profile", "AI provider activation failed: " + QString::fromStdString(active.error()));
        return;
    }

    auto verify = LlmConfigRepository::instance().get_active_provider();
    if (!verify.is_ok() || verify.value().provider.toLower() != id) {
        const QString detail = verify.is_ok()
                                   ? tr("active is '%1' not '%2'").arg(verify.value().provider, id)
                                   : QString::fromStdString(verify.error());
        show(tr("Save verification failed"), true);
        LOG_ERROR("Profile", "AI provider save verification failed: " + detail);
        return;
    }

    ai_chat::LlmService::instance().reload_config();
    refresh_ai_accounts();
    show(tr("Saved locally and set active"), false);
}

void ProfileScreen::save_local_profile() {
    auto& repo = openmarketterminal::SettingsRepository::instance();
    const QString name = profile_name_ ? profile_name_->text().trimmed() : QString();
    const QString nickname = profile_nickname_ ? profile_nickname_->text().trimmed() : QString();
    const QString email = profile_email_ ? profile_email_->text().trimmed() : QString();
    const QString phone = profile_phone_ ? profile_phone_->text().trimmed() : QString();
    const QString country = profile_country_ ? profile_country_->text().trimmed() : QString();

    auto r1 = repo.set("profile.name", name, "profile");
    auto r2 = repo.set("profile.nickname", nickname, "profile");
    auto r3 = repo.set("profile.username", nickname.isEmpty() ? name : nickname, "profile");
    auto r4 = repo.set("profile.email", email, "profile");
    auto r5 = repo.set("profile.phone", phone, "profile");
    auto r6 = repo.set("profile.country", country, "profile");

    const bool ok = r1.is_ok() && r2.is_ok() && r3.is_ok() && r4.is_ok() && r5.is_ok() && r6.is_ok();
    if (ok)
        refresh_all();
    if (profile_status_) {
        profile_status_->setText(ok ? tr("Saved locally") : tr("Save failed"));
        profile_status_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                           .arg(ok ? ui::colors::POSITIVE() : ui::colors::NEGATIVE(), MF));
    }
}

} // namespace openmarketterminal::screens
