#include "screens/about/AboutScreen.h"

#include "core/config/AppPaths.h"
#include "ui/theme/Theme.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

// ── Style constants ───────────────────────────────────────────────────────────

static QString SECTION_LABEL() {
    return QString("color: %1; font-size: 11px; font-weight: bold; letter-spacing: 0.5px; "
                   "text-transform: uppercase; background: transparent; "
                   "font-family: 'Consolas','Courier New',monospace;")
        .arg(ui::colors::TEXT_SECONDARY());
}

static QString BODY() {
    return QString("color: %1; font-size: 13px; background: transparent; "
                   "font-family: 'Consolas','Courier New',monospace;")
        .arg(ui::colors::TEXT_PRIMARY());
}

static QString MUTED() {
    return QString("color: %1; font-size: 12px; background: transparent; "
                   "font-family: 'Consolas','Courier New',monospace;")
        .arg(ui::colors::TEXT_TERTIARY());
}

static QString LINK_STYLE() {
    return QString("color: %1; font-size: 13px; background: transparent; "
                   "font-family: 'Consolas','Courier New',monospace;")
        .arg(ui::colors::CYAN());
}

static QString PANEL() {
    return QString("background: %1; border: 1px solid %2; border-radius: 2px;")
        .arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM());
}

static QString LINK_BTN() {
    return QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                   "border-radius: 2px; padding: 8px 12px; font-size: 12px; text-align: left; "
                   "font-family: 'Consolas','Courier New',monospace; }"
                   "QPushButton:hover { background: %4; color: #38bdf8; }")
        .arg(ui::colors::BG_RAISED(), ui::colors::CYAN(), ui::colors::BORDER_DIM(), ui::colors::BG_HOVER());
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static QWidget* makePanel() {
    auto* w = new QWidget(nullptr);
    w->setStyleSheet(PANEL());
    return w;
}

static QLabel* makePanelHeader(const QString& icon, const QString& title, const QString& iconColor) {
    auto* lbl = new QLabel(icon.isEmpty() ? title : QString("%1  %2").arg(icon, title));
    lbl->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; letter-spacing: 0.5px; "
                               "background: %2; padding: 10px 14px; border-bottom: 1px solid %3; "
                               "font-family: 'Consolas','Courier New',monospace;")
                           .arg(iconColor, ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    return lbl;
}

static QLabel* makeBullet(const QString& text) {
    auto* lbl = new QLabel(QString("■  %1").arg(text));
    lbl->setStyleSheet(QString("color: %1; font-size: 12px; background: transparent; "
                               "font-family: 'Consolas','Courier New',monospace;")
                           .arg(ui::colors::TEXT_SECONDARY()));
    lbl->setWordWrap(true);
    return lbl;
}

// Re-apply icon + title to a panel header (mirrors makePanelHeader's text format).
static void setPanelHeaderText(QLabel* lbl, const QString& icon, const QString& title) {
    if (lbl) lbl->setText(icon.isEmpty() ? title : QString("%1  %2").arg(icon, title));
}

// Re-apply bullet text (mirrors makeBullet's "■  %1" format).
static void setBulletText(QLabel* lbl, const QString& text) {
    if (lbl) lbl->setText(QString("■  %1").arg(text));
}

// ── Constructor ───────────────────────────────────────────────────────────────

AboutScreen::AboutScreen(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("QWidget#AboutRoot { background: %1; }").arg(ui::colors::BG_BASE()));
    setObjectName("AboutRoot");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Scrollable content ────────────────────────────────────────────────────
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");

    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(24, 24, 24, 24);
    vl->setSpacing(12);

    // ── Version Information ───────────────────────────────────────────────────
    {
        auto* panel = makePanel();
        auto* pvl = new QVBoxLayout(panel);
        pvl->setContentsMargins(0, 0, 0, 0);
        pvl->setSpacing(0);

        version_header_ = makePanelHeader("ℹ", tr("VERSION INFORMATION"), ui::colors::AMBER);
        pvl->addWidget(version_header_);

        auto* body = new QWidget(this);
        body->setStyleSheet("background: transparent;");
        auto* bhl = new QHBoxLayout(body);
        bhl->setContentsMargins(14, 12, 14, 12);

        // OpenMarketTerminal brand mark (embedded Qt resource).
        auto* logo = new QLabel(body);
        logo->setPixmap(QPixmap(QStringLiteral(":/openmarket/about-logo.png"))
                            .scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        logo->setFixedSize(56, 56);
        logo->setStyleSheet(QStringLiteral("background: transparent;"));
        bhl->addWidget(logo);
        bhl->addSpacing(12);

        auto* left = new QVBoxLayout;
        left->setSpacing(4);
        app_name_ = new QLabel(QStringLiteral("Open Terminal"));
        app_name_->setStyleSheet(QString("color: %1; font-size: 15px; font-weight: bold; background: transparent; "
                                          "font-family: 'Consolas','Courier New',monospace;")
                                     .arg(ui::colors::TEXT_PRIMARY()));
        left->addWidget(app_name_);
        app_subtitle_ = new QLabel(tr("NATIVE DESKTOP FINANCIAL INTELLIGENCE TERMINAL"));
        app_subtitle_->setStyleSheet(MUTED());
        left->addWidget(app_subtitle_);
        bhl->addLayout(left);
        bhl->addStretch();

        auto* right = new QVBoxLayout;
        right->setSpacing(4);
        right->setAlignment(Qt::AlignRight);
        auto* ver = new QLabel(QStringLiteral("v%1").arg(QApplication::applicationVersion()));
        ver->setStyleSheet(QString("color: %1; font-size: 18px; font-weight: bold; background: transparent; "
                                   "font-family: 'Consolas','Courier New',monospace;")
                               .arg(ui::colors::AMBER()));
        ver->setAlignment(Qt::AlignRight);
        right->addWidget(ver);

        // (No "Check for Updates" button — the auto-updater was removed.
        // OpenMarketTerminal does not phone home or fetch/run remote installers.)
        bhl->addLayout(right);

        pvl->addWidget(body);

        // Footer bar
        copyright_ = new QLabel(tr("© 2026 Open Terminal contributors. MIT License."));
        copyright_->setStyleSheet(QString("color: %1; font-size: 11px; background: %2; "
                                          "padding: 6px 14px; border-top: 1px solid %3; "
                                          "font-family: 'Consolas','Courier New',monospace;")
                                      .arg(ui::colors::TEXT_TERTIARY(), ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
        pvl->addWidget(copyright_);

        vl->addWidget(panel);
    }

    // ── License — two columns ─────────────────────────────────────────────────
    {
        auto* row = new QWidget(this);
        row->setStyleSheet("background: transparent;");
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(12);

        // Open Source
        {
            auto* panel = makePanel();
            auto* pvl = new QVBoxLayout(panel);
            pvl->setContentsMargins(0, 0, 0, 0);
            pvl->setSpacing(0);
            oss_header_ = makePanelHeader("", tr("OPEN SOURCE LICENSE"), ui::colors::POSITIVE);
            pvl->addWidget(oss_header_);

            auto* body = new QWidget(this);
            body->setStyleSheet("background: transparent;");
            auto* bvl = new QVBoxLayout(body);
            bvl->setContentsMargins(14, 10, 14, 10);
            bvl->setSpacing(6);
            // First bullet is the license SPDX identifier (a code value) — not translated.
            auto* oss_b0 = makeBullet(QStringLiteral("MIT"));
            oss_bullets_ = {oss_b0,
                            makeBullet(tr("Free for personal, educational, and commercial use")),
                            makeBullet(tr("Fork, modify, redistribute, and package your own builds")),
                            makeBullet(tr("Keep copyright and license notices with redistributed copies"))};
            for (auto* b : oss_bullets_) bvl->addWidget(b);
            pvl->addWidget(body);

            // Footer link
            auto* foot = new QLabel("apache.org/licenses/LICENSE-2.0");
            foot->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent; "
                                        "padding: 6px 14px; border-top: 1px solid %2; "
                                        "font-family: 'Consolas','Courier New',monospace;")
                                    .arg(ui::colors::CYAN(), ui::colors::BORDER_DIM()));
            pvl->addWidget(foot);

            rl->addWidget(panel, 1);
        }

        // Open source rights
        {
            auto* panel = makePanel();
            auto* pvl = new QVBoxLayout(panel);
            pvl->setContentsMargins(0, 0, 0, 0);
            pvl->setSpacing(0);
            commercial_header_ = makePanelHeader("★", tr("OPEN SOURCE RIGHTS"), ui::colors::AMBER);
            pvl->addWidget(commercial_header_);

            auto* body = new QWidget(this);
            body->setStyleSheet("background: transparent;");
            auto* bvl = new QVBoxLayout(body);
            bvl->setContentsMargins(14, 10, 14, 10);
            bvl->setSpacing(6);
            commercial_bullets_ = {makeBullet(tr("No separate commercial license required")),
                                   makeBullet(tr("Private customizations are allowed")),
                                   makeBullet(tr("Contributions are welcome, not mandatory")),
                                   makeBullet(tr("You control your local data and configuration"))};
            for (auto* b : commercial_bullets_) bvl->addWidget(b);
            pvl->addWidget(body);

            // Footer link
            auto* foot = new QLabel("github.com/your-org/open-terminal");
            foot->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent; "
                                        "padding: 6px 14px; border-top: 1px solid %2; "
                                        "font-family: 'Consolas','Courier New',monospace;")
                                    .arg(ui::colors::CYAN(), ui::colors::BORDER_DIM()));
            pvl->addWidget(foot);

            rl->addWidget(panel, 1);
        }

        vl->addWidget(row);
    }

    // ── Diagnostics ───────────────────────────────────────────────────────────
    // Surfaces the crash-dump folder path so users filing bug reports can
    // attach minidumps without hunting through %LOCALAPPDATA% (see issue #215).
    {
        auto* panel = makePanel();
        auto* pvl = new QVBoxLayout(panel);
        pvl->setContentsMargins(0, 0, 0, 0);
        pvl->setSpacing(0);
        diag_header_ = makePanelHeader("⚙", tr("DIAGNOSTICS"), ui::colors::AMBER);
        pvl->addWidget(diag_header_);

        auto* body = new QWidget(this);
        body->setStyleSheet("background: transparent;");
        auto* bhl = new QHBoxLayout(body);
        bhl->setContentsMargins(14, 10, 14, 10);
        bhl->setSpacing(12);

        auto* left = new QVBoxLayout;
        left->setSpacing(2);
        crash_dumps_label_ = new QLabel(tr("CRASH DUMPS"));
        crash_dumps_label_->setStyleSheet(SECTION_LABEL());
        left->addWidget(crash_dumps_label_);
        const QString crash_dir = QDir::toNativeSeparators(AppPaths::crashdumps());
        auto* path = new QLabel(crash_dir);
        path->setStyleSheet(MUTED());
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        path->setWordWrap(true);
        left->addWidget(path);
        bhl->addLayout(left, 1);

        open_folder_btn_ = new QPushButton(tr("Open Folder"));
        open_folder_btn_->setStyleSheet(LINK_BTN());
        open_folder_btn_->setCursor(Qt::PointingHandCursor);
        connect(open_folder_btn_, &QPushButton::clicked, this, [crash_dir]() {
            // mkpath is idempotent; ensures the folder exists before
            // QDesktopServices::openUrl on a freshly installed terminal
            // that hasn't crashed yet.
            QDir().mkpath(crash_dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(crash_dir));
        });
        bhl->addWidget(open_folder_btn_, 0, Qt::AlignTop);

        pvl->addWidget(body);
        vl->addWidget(panel);
    }

    // ── Trademarks ────────────────────────────────────────────────────────────
    {
        auto* panel = makePanel();
        auto* pvl = new QVBoxLayout(panel);
        pvl->setContentsMargins(0, 0, 0, 0);
        pvl->setSpacing(0);
        trademarks_header_ = makePanelHeader("", tr("TRADEMARKS"), ui::colors::AMBER);
        pvl->addWidget(trademarks_header_);

        auto* body = new QWidget(this);
        body->setStyleSheet("background: transparent;");
        auto* bvl = new QVBoxLayout(body);
        bvl->setContentsMargins(14, 10, 14, 12);
        bvl->setSpacing(6);

        trademarks_desc_ = new QLabel(tr("\"Open Terminal\" and associated community-created logos identify this "
                                         "open-source project and its contributors."));
        trademarks_desc_->setStyleSheet(BODY());
        trademarks_desc_->setWordWrap(true);
        bvl->addWidget(trademarks_desc_);

        trademarks_perm_ =
            new QLabel(tr("Permission is granted to use the Open Terminal name and logos to identify, discuss, "
                          "fork, package, and redistribute this open-source project, provided the use is truthful "
                          "and does not misrepresent affiliation or endorsement."));
        trademarks_perm_->setStyleSheet(MUTED());
        trademarks_perm_->setWordWrap(true);
        bvl->addWidget(trademarks_perm_);

        pvl->addWidget(body);
        vl->addWidget(panel);
    }

    // ── Resources — 3×2 grid ─────────────────────────────────────────────────
    {
        auto* panel = makePanel();
        auto* pvl = new QVBoxLayout(panel);
        pvl->setContentsMargins(0, 0, 0, 0);
        pvl->setSpacing(0);
        resources_header_ = makePanelHeader("", tr("RESOURCES"), ui::colors::AMBER);
        pvl->addWidget(resources_header_);

        auto* body = new QWidget(this);
        body->setStyleSheet("background: transparent;");
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(14, 10, 14, 12);
        grid->setSpacing(8);

        struct Link {
            QString label;
            QString url;
        };
        const Link links[] = {
            {tr("GitHub Repository"), "https://github.com/your-org/open-terminal"},
            {tr("License (MIT)"), "https://github.com/your-org/open-terminal/blob/main/LICENSE"},
            {tr("Documentation"), "https://github.com/your-org/open-terminal/tree/main/docs"},
            {tr("Issue Tracker"), "https://github.com/your-org/open-terminal/issues"},
            {tr("Release Notes"), "https://github.com/your-org/open-terminal/releases"},
            {tr("Source Code"), "https://github.com/your-org/open-terminal"},
        };

        for (int i = 0; i < 6; ++i) {
            auto* btn = new QPushButton(links[i].label);
            btn->setStyleSheet(LINK_BTN());
            btn->setFixedHeight(36);
            const QString url = links[i].url;
            connect(btn, &QPushButton::clicked, this, [url]() { QDesktopServices::openUrl(QUrl(url)); });
            grid->addWidget(btn, i / 3, i % 3);
            resource_btns_.append(btn);
        }

        pvl->addWidget(body);
        vl->addWidget(panel);
    }

    // ── Contact ───────────────────────────────────────────────────────────────
    {
        auto* panel = makePanel();
        auto* pvl = new QVBoxLayout(panel);
        pvl->setContentsMargins(0, 0, 0, 0);
        pvl->setSpacing(0);
        contact_header_ = makePanelHeader("✉", tr("CONTACT"), ui::colors::AMBER);
        pvl->addWidget(contact_header_);

        auto* body = new QWidget(this);
        body->setStyleSheet("background: transparent;");
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(14, 10, 14, 12);
        grid->setSpacing(12);

        struct Contact {
            QString label;
            QString email;
        };
        const Contact contacts[] = {
            {tr("PROJECT"), "github.com/your-org/open-terminal"},
            {tr("ISSUES"), "github.com/your-org/open-terminal/issues"},
            {tr("SECURITY"), "github.com/your-org/open-terminal/security"},
            {tr("LICENSE"), "github.com/your-org/open-terminal/blob/main/LICENSE"},
        };

        for (int i = 0; i < 4; ++i) {
            auto* col = new QWidget(this);
            col->setStyleSheet("background: transparent;");
            auto* cvl = new QVBoxLayout(col);
            cvl->setContentsMargins(0, 0, 0, 0);
            cvl->setSpacing(2);

            auto* lbl = new QLabel(contacts[i].label);
            lbl->setStyleSheet(SECTION_LABEL());
            cvl->addWidget(lbl);
            contact_labels_.append(lbl);

            auto* email = new QLabel(contacts[i].email);
            email->setStyleSheet(LINK_STYLE());
            cvl->addWidget(email);

            grid->addWidget(col, 0, i);
        }

        pvl->addWidget(body);
        vl->addWidget(panel);
    }

    vl->addStretch();
    scroll->setWidget(page);
    root->addWidget(scroll, 1);
}

void AboutScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void AboutScreen::retranslateUi() {
    // Version panel
    setPanelHeaderText(version_header_, "ℹ", tr("VERSION INFORMATION"));
    if (app_subtitle_) app_subtitle_->setText(tr("NATIVE DESKTOP FINANCIAL INTELLIGENCE TERMINAL"));
    if (copyright_) copyright_->setText(tr("© 2026 Open Terminal contributors. MIT License."));

    // Open source license — bullet 0 is the SPDX identifier (untranslated).
    setPanelHeaderText(oss_header_, "", tr("OPEN SOURCE LICENSE"));
    if (oss_bullets_.size() == 4) {
        setBulletText(oss_bullets_[1], tr("Free for personal, educational, and commercial use"));
        setBulletText(oss_bullets_[2], tr("Fork, modify, redistribute, and package your own builds"));
        setBulletText(oss_bullets_[3], tr("Keep copyright and license notices with redistributed copies"));
    }

    // Open source rights
    setPanelHeaderText(commercial_header_, "★", tr("OPEN SOURCE RIGHTS"));
    if (commercial_bullets_.size() == 4) {
        setBulletText(commercial_bullets_[0], tr("No separate commercial license required"));
        setBulletText(commercial_bullets_[1], tr("Private customizations are allowed"));
        setBulletText(commercial_bullets_[2], tr("Contributions are welcome, not mandatory"));
        setBulletText(commercial_bullets_[3], tr("You control your local data and configuration"));
    }

    // Diagnostics
    setPanelHeaderText(diag_header_, "⚙", tr("DIAGNOSTICS"));
    if (crash_dumps_label_) crash_dumps_label_->setText(tr("CRASH DUMPS"));
    if (open_folder_btn_)   open_folder_btn_->setText(tr("Open Folder"));

    // Trademarks
    setPanelHeaderText(trademarks_header_, "", tr("TRADEMARKS"));
    if (trademarks_desc_)
        trademarks_desc_->setText(tr("\"Open Terminal\" and associated community-created logos identify this "
                                     "open-source project and its contributors."));
    if (trademarks_perm_)
        trademarks_perm_->setText(tr("Permission is granted to use the Open Terminal name and logos to identify, discuss, "
                                     "fork, package, and redistribute this open-source project, provided the use is truthful "
                                     "and does not misrepresent affiliation or endorsement."));

    // Resources
    setPanelHeaderText(resources_header_, "", tr("RESOURCES"));
    if (resource_btns_.size() == 6) {
        resource_btns_[0]->setText(tr("GitHub Repository"));
        resource_btns_[1]->setText(tr("License (MIT)"));
        resource_btns_[2]->setText(tr("Documentation"));
        resource_btns_[3]->setText(tr("Issue Tracker"));
        resource_btns_[4]->setText(tr("Release Notes"));
        resource_btns_[5]->setText(tr("Source Code"));
    }

    // Contact
    setPanelHeaderText(contact_header_, "✉", tr("CONTACT"));
    if (contact_labels_.size() == 4) {
        contact_labels_[0]->setText(tr("PROJECT"));
        contact_labels_[1]->setText(tr("ISSUES"));
        contact_labels_[2]->setText(tr("SECURITY"));
        contact_labels_[3]->setText(tr("LICENSE"));
    }
}

} // namespace openmarketterminal::screens
