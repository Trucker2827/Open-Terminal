#pragma once
#include <QLabel>
#include <QPushButton>
#include <QWidget>

namespace openmarketterminal::screens {

/// Guest-entry launcher.
///
/// LOCAL-FIRST FORK: the email/password/MFA/Google login form was removed
/// together with the remote-account backend ("Phase 2"). This screen now only
/// presents branding and a "Continue as Guest" action that boots the local
/// shell (the only runtime entry point). It still emits continue_as_guest,
/// which WindowFrame_Setup connects to WindowFrame::continue_as_guest.
///
/// Internationalised: user-facing strings flow through tr(); on
/// QEvent::LanguageChange retranslateUi() reapplies translated text.
class LoginScreen : public QWidget {
    Q_OBJECT
  public:
    explicit LoginScreen(QWidget* parent = nullptr);

  signals:
    void continue_as_guest();

  private:
    QLabel* login_title_ = nullptr;
    QLabel* login_subtitle_ = nullptr;
    QPushButton* guest_btn_ = nullptr;

    /// Reapply translated text to every widget cached as a member pointer.
    void retranslateUi();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
};

} // namespace openmarketterminal::screens
