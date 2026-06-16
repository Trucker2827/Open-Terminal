#include "screens/profile/ProfileScreen.h"

#include "auth/AuthManager.h"
#include "core/currency/Currency.h"
#include "core/logging/Logger.h"
#include "storage/repositories/SettingsRepository.h"
#include "ui/theme/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QMessageBox>
#include <QScrollArea>
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
    sections_->addWidget(build_support());
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
    if (header_title_)       header_title_->setText(tr("PROFILE & ACCOUNT"));
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
          << build_support();

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
    header_title_ = new QLabel(tr("PROFILE & ACCOUNT"));
    header_title_->setStyleSheet(
        QString("color:%1;font-size:14px;font-weight:700;background:transparent;%2").arg(ui::colors::AMBER(), MF));
    hl->addWidget(header_title_);
    username_header_ = new QLabel;
    username_header_->setStyleSheet(
        QString("color:%1;font-size:13px;background:transparent;%2").arg(ui::colors::TEXT_PRIMARY(), MF));
    hl->addWidget(username_header_);
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
    nav_source_keys_ = {QStringLiteral("OVERVIEW"), QStringLiteral("SECURITY"),
                        QStringLiteral("SUPPORT")};
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
    auto* grid = new QGridLayout;
    grid->setSpacing(10);

    auto* acct = make_panel(tr("ACCOUNT INFORMATION"));
    auto* avl = qobject_cast<QVBoxLayout*>(acct->layout());
    avl->addWidget(make_data_row(tr("USERNAME"), ov_username_));
    avl->addWidget(make_data_row(tr("EMAIL"), ov_email_));
    avl->addWidget(make_data_row(tr("PHONE"), ov_phone_));
    avl->addWidget(make_data_row(tr("COUNTRY"), ov_country_));
    // Edit Profile button row
    auto* ep_row = new QWidget(this);
    ep_row->setStyleSheet("background:transparent;");
    auto* ep_rl = new QHBoxLayout(ep_row);
    ep_rl->setContentsMargins(12, 8, 12, 8);
    ep_rl->setSpacing(0);
    auto* ep_btn = new QPushButton(tr("EDIT PROFILE"));
    ep_btn->setFixedHeight(26);
    ep_btn->setCursor(Qt::PointingHandCursor);
    ep_btn->setStyleSheet(
        QString("QPushButton{background:rgba(217,119,6,0.12);color:%1;border:1px solid rgba(217,119,6,0.4);padding:0 12px;"
                "font-size:11px;font-weight:700;font-family:'Consolas',monospace;}QPushButton:hover{background:%1;color:%2;}")
            .arg(ui::colors::AMBER(), ui::colors::BG_BASE()));
    connect(ep_btn, &QPushButton::clicked, this, &ProfileScreen::show_edit_profile);
    ep_rl->addWidget(ep_btn);
    ep_rl->addStretch();
    avl->addWidget(ep_row);
    grid->addWidget(acct, 0, 0, 1, 2);

    auto* actions = make_panel(tr("QUICK ACTIONS"));
    auto* act_vl = qobject_cast<QVBoxLayout*>(actions->layout());
    auto* ar = new QWidget(this);
    ar->setStyleSheet("background:transparent;");
    auto* arl = new QHBoxLayout(ar);
    arl->setContentsMargins(12, 8, 12, 8);
    arl->setSpacing(10);
    auto* lb = new QPushButton(tr("LOGOUT"));
    lb->setFixedHeight(26);
    lb->setStyleSheet(
        QString("QPushButton{background:rgba(220,38,38,0.1);color:%1;border:1px solid #7f1d1d;padding:0 12px;"
                "font-size:11px;font-weight:700;font-family:'Consolas',monospace;}QPushButton:hover{background:%1;"
                "color:%2;}")
            .arg(ui::colors::NEGATIVE(), ui::colors::TEXT_PRIMARY()));
    connect(lb, &QPushButton::clicked, this, &ProfileScreen::show_logout_confirm);
    arl->addWidget(lb);
    arl->addStretch();
    act_vl->addWidget(ar);
    grid->addWidget(actions, 1, 0, 1, 2);
    vl->addLayout(grid);
    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget* ProfileScreen::build_security() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea{border:none;background:transparent;}");
    auto* page = new QWidget(this);
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(14, 14, 14, 14);
    vl->setSpacing(10);
    auto* kp = make_panel(tr("API KEY"));
    auto* kvl = qobject_cast<QVBoxLayout*>(kp->layout());
    auto* kr = new QWidget(this);
    kr->setStyleSheet("background:transparent;");
    auto* krl = new QHBoxLayout(kr);
    krl->setContentsMargins(12, 10, 12, 10);
    krl->setSpacing(8);
    sec_api_key_ = new QLabel(QString(20, QChar(0x2022)));
    sec_api_key_->setStyleSheet(
        QString("color:%1;font-size:13px;background:transparent;%2").arg(ui::colors::TEXT_PRIMARY(), MF));
    krl->addWidget(sec_api_key_, 1);
    auto* sb = new QPushButton(tr("SHOW"));
    sb->setFixedHeight(22);
    sb->setStyleSheet(
        QString("QPushButton{background:%1;color:%2;border:1px solid %3;padding:0 10px;"
                "font-size:10px;font-weight:700;font-family:'Consolas',monospace;}QPushButton:hover{color:%4;}")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(), ui::colors::TEXT_PRIMARY()));
    connect(sb, &QPushButton::clicked, this, [this, sb]() {
        api_key_visible_ = !api_key_visible_;
        sb->setText(api_key_visible_ ? tr("HIDE") : tr("SHOW"));
        sec_api_key_->setText(api_key_visible_ ? auth::AuthManager::instance().session().api_key
                                               : QString(20, QChar(0x2022)));
    });
    krl->addWidget(sb);
    auto* cb = new QPushButton(tr("COPY"));
    cb->setFixedHeight(22);
    cb->setStyleSheet(
        QString("QPushButton{background:%1;color:%2;border:1px solid %3;padding:0 10px;"
                "font-size:10px;font-weight:700;font-family:'Consolas',monospace;}QPushButton:hover{color:%4;}")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(), ui::colors::TEXT_PRIMARY()));
    connect(cb, &QPushButton::clicked, this, [cb]() {
        auto key = auth::AuthManager::instance().session().api_key;
        if (!key.isEmpty()) {
            QApplication::clipboard()->setText(key);
            cb->setText(tr("COPIED"));
            QTimer::singleShot(1500, cb, [cb]() { cb->setText(tr("COPY")); });
        }
    });
    krl->addWidget(cb);
    // LOCAL-FIRST FORK: REGENERATE called UserApi::regenerate_api_key (remote);
    // removed with the remote-account backend. The key remains viewable/copyable.
    kvl->addWidget(kr);
    vl->addWidget(kp);
    auto* ssp = make_panel(tr("SECURITY STATUS"));
    auto* ssvl = qobject_cast<QVBoxLayout*>(ssp->layout());
    ssvl->addWidget(make_data_row(tr("EMAIL VERIFIED"), sec_verified_));
    ssvl->addWidget(make_data_row(tr("2FA (MFA)"), sec_mfa_));
    vl->addWidget(ssp);
    // LOCAL-FIRST FORK: the LOGIN HISTORY panel was fed by UserApi remote
    // queries (get_login_history); removed with the remote-account backend.
    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

QWidget* ProfileScreen::build_support() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea{border:none;background:transparent;}");
    auto* page = new QWidget(this);
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(14, 14, 14, 14);
    vl->setSpacing(10);
    auto* cp = make_panel(tr("CONTACT US"));
    auto* cvl2 = qobject_cast<QVBoxLayout*>(cp->layout());
    auto* cg = new QGridLayout;
    cg->setSpacing(12);
    cg->setContentsMargins(12, 10, 12, 10);
    auto add_c = [&](const QString& l, const QString& e, int r, int c2) {
        auto* w = new QWidget(this);
        w->setStyleSheet("background:transparent;");
        auto* wl = new QVBoxLayout(w);
        wl->setContentsMargins(0, 0, 0, 0);
        wl->setSpacing(2);
        auto* lb = new QLabel(l);
        lb->setStyleSheet(
            QString("color:%1;font-size:10px;font-weight:700;background:transparent;letter-spacing:0.5px;%2")
                .arg(ui::colors::TEXT_TERTIARY(), MF));
        wl->addWidget(lb);
        auto* em = new QLabel(e);
        em->setStyleSheet(QString("color:%1;font-size:13px;background:transparent;%2").arg(ui::colors::CYAN(), MF));
        wl->addWidget(em);
        cg->addWidget(w, r, c2);
    };
    add_c(tr("PROJECT"), "github.com/your-org/open-terminal", 0, 0);
    add_c(tr("ISSUES"), "github.com/your-org/open-terminal/issues", 0, 1);
    add_c(tr("SECURITY"), "github.com/your-org/open-terminal/issues", 1, 0);
    add_c(tr("LICENSE"), "github.com/your-org/open-terminal/blob/main/LICENSE", 1, 1);
    cvl2->addLayout(cg);
    vl->addWidget(cp);
    auto* lp = make_panel(tr("RESOURCES"));
    auto* lvl = qobject_cast<QVBoxLayout*>(lp->layout());
    auto* lr = new QWidget(this);
    lr->setStyleSheet("background:transparent;");
    auto* lrl = new QHBoxLayout(lr);
    lrl->setContentsMargins(12, 10, 12, 10);
    lrl->setSpacing(8);
    auto make_link_btn = [&](const QString& label, const QString& url) {
        auto* b = new QPushButton(label);
        b->setFixedHeight(26);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(
            QString("QPushButton{background:%1;color:%2;border:1px solid %3;padding:0 12px;"
                    "font-size:11px;font-family:'Consolas',monospace;}QPushButton:hover{color:%4;background:%5;}")
                .arg(ui::colors::BG_RAISED(), ui::colors::CYAN(), ui::colors::BORDER_DIM(), ui::colors::TEXT_PRIMARY(),
                     ui::colors::BG_HOVER()));
        connect(b, &QPushButton::clicked, this, [url]() { QDesktopServices::openUrl(QUrl(url)); });
        lrl->addWidget(b);
    };
    // DOCS / FAQ are localisable; GITHUB / DISCORD are brand names — left raw.
    make_link_btn(tr("DOCS"), "https://github.com/your-org/open-terminal/tree/main/docs");
    make_link_btn(QStringLiteral("GITHUB"), "https://github.com/your-org/open-terminal");
    make_link_btn(QStringLiteral("DISCORD"), "https://discord.gg/ae87a8ygbN");
    make_link_btn(tr("FAQ"), "https://github.com/your-org/open-terminal/wiki");
    lrl->addStretch();
    lvl->addWidget(lr);
    vl->addWidget(lp);
    vl->addStretch();
    scroll->setWidget(page);
    return scroll;
}

void ProfileScreen::refresh_all() {
    // LOCAL-FIRST: populate Overview from local SettingsRepository (no login required).
    auto& repo = openmarketterminal::SettingsRepository::instance();
    const auto r_uname   = repo.get("profile.username", "");
    const auto r_email   = repo.get("profile.email",    "");
    const auto r_phone   = repo.get("profile.phone",    "");
    const auto r_country = repo.get("profile.country",  "");
    const QString uname   = (r_uname.is_ok()   && !r_uname.value().isEmpty())   ? r_uname.value()   : QString();
    const QString email   = (r_email.is_ok()   && !r_email.value().isEmpty())   ? r_email.value()   : QString();
    const QString phone   = (r_phone.is_ok()   && !r_phone.value().isEmpty())   ? r_phone.value()   : QString();
    const QString country = (r_country.is_ok() && !r_country.value().isEmpty()) ? r_country.value() : QString();

    username_header_->setText(uname.isEmpty() ? tr("Local Profile") : uname);
    ov_username_->setText(uname.isEmpty()   ? tr("Not set") : uname);
    ov_email_->setText(email.isEmpty()      ? tr("Not set") : email);
    ov_phone_->setText(phone.isEmpty()      ? tr("Not set") : phone);
    ov_country_->setText(country.isEmpty()  ? tr("Not set") : country);

    // Security tab — still reads from auth session (those rows remain remote-backed).
    const auto& s = auth::AuthManager::instance().session();
    sec_verified_->setText(s.user_info.is_verified ? tr("YES") : tr("NO"));
    sec_verified_->setStyleSheet(QString("color:%1;font-size:13px;font-weight:700;background:transparent;%2")
                                     .arg(s.user_info.is_verified ? ui::colors::POSITIVE() : ui::colors::NEGATIVE())
                                     .arg(MF));
    sec_mfa_->setText(s.user_info.mfa_enabled ? tr("ENABLED") : tr("DISABLED"));
    sec_mfa_->setStyleSheet(QString("color:%1;font-size:13px;font-weight:700;background:transparent;%2")
                                .arg(s.user_info.mfa_enabled ? ui::colors::POSITIVE() : ui::colors::TEXT_SECONDARY())
                                .arg(MF));
}

void ProfileScreen::show_edit_profile() {
    auto& repo = openmarketterminal::SettingsRepository::instance();
    const auto r_uname   = repo.get("profile.username", "");
    const auto r_email   = repo.get("profile.email",    "");
    const auto r_phone   = repo.get("profile.phone",    "");
    const auto r_country = repo.get("profile.country",  "");
    const QString cur_uname   = (r_uname.is_ok()   && !r_uname.value().isEmpty())   ? r_uname.value()   : QString();
    const QString cur_email   = (r_email.is_ok()   && !r_email.value().isEmpty())   ? r_email.value()   : QString();
    const QString cur_phone   = (r_phone.is_ok()   && !r_phone.value().isEmpty())   ? r_phone.value()   : QString();
    const QString cur_country = (r_country.is_ok() && !r_country.value().isEmpty()) ? r_country.value() : QString();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Edit Profile"));
    dlg.setModal(true);
    dlg.setMinimumWidth(360);
    dlg.setStyleSheet(QString("background:%1;color:%2;")
                          .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY()));
    auto* vl = new QVBoxLayout(&dlg);
    vl->setContentsMargins(16, 16, 16, 16);
    vl->setSpacing(10);

    auto make_field = [&](const QString& label, const QString& value) -> QLineEdit* {
        auto* lbl = new QLabel(label);
        lbl->setStyleSheet(
            QString("color:%1;font-size:11px;font-weight:700;background:transparent;letter-spacing:0.5px;%2")
                .arg(ui::colors::TEXT_SECONDARY(), MF));
        vl->addWidget(lbl);
        auto* ed = new QLineEdit(value);
        ed->setStyleSheet(
            QString("QLineEdit{background:%1;color:%2;border:1px solid %3;padding:4px 8px;font-size:13px;%4}"
                    "QLineEdit:focus{border:1px solid %5;}")
                .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
                     ui::colors::AMBER()));
        vl->addWidget(ed);
        return ed;
    };

    auto* uname_edit   = make_field(tr("USERNAME"), cur_uname);
    auto* email_edit   = make_field(tr("EMAIL"),    cur_email);
    auto* phone_edit   = make_field(tr("PHONE"),    cur_phone);
    auto* country_edit = make_field(tr("COUNTRY"),  cur_country);

    auto* btn_row = new QWidget(&dlg);
    btn_row->setStyleSheet("background:transparent;");
    auto* brl = new QHBoxLayout(btn_row);
    brl->setContentsMargins(0, 4, 0, 0);
    brl->setSpacing(8);
    brl->addStretch();

    auto* cancel_btn = new QPushButton(tr("CANCEL"));
    cancel_btn->setFixedHeight(26);
    cancel_btn->setStyleSheet(
        QString("QPushButton{background:%1;color:%2;border:1px solid %3;padding:0 12px;"
                "font-size:11px;font-weight:700;font-family:'Consolas',monospace;}QPushButton:hover{color:%4;}")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(),
                 ui::colors::TEXT_PRIMARY()));
    connect(cancel_btn, &QPushButton::clicked, &dlg, &QDialog::reject);
    brl->addWidget(cancel_btn);

    auto* save_btn = new QPushButton(tr("SAVE"));
    save_btn->setFixedHeight(26);
    save_btn->setStyleSheet(
        QString("QPushButton{background:rgba(217,119,6,0.12);color:%1;border:1px solid rgba(217,119,6,0.4);padding:0 12px;"
                "font-size:11px;font-weight:700;font-family:'Consolas',monospace;}QPushButton:hover{background:%1;color:%2;}")
            .arg(ui::colors::AMBER(), ui::colors::BG_BASE()));
    connect(save_btn, &QPushButton::clicked, &dlg, [=, this, &dlg]() {
        auto& r = openmarketterminal::SettingsRepository::instance();
        r.set("profile.username", uname_edit->text().trimmed(),   "profile");
        r.set("profile.email",    email_edit->text().trimmed(),    "profile");
        r.set("profile.phone",    phone_edit->text().trimmed(),    "profile");
        r.set("profile.country",  country_edit->text().trimmed(),  "profile");
        refresh_all();
        dlg.accept();
    });
    brl->addWidget(save_btn);
    vl->addWidget(btn_row);

    dlg.exec();
}

void ProfileScreen::show_logout_confirm() {
    if (QMessageBox::question(this, tr("Confirm Logout"), tr("Are you sure you want to logout?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        auth::AuthManager::instance().logout();
}

} // namespace openmarketterminal::screens
