#pragma once
// Crypto Credentials — dialog for entering exchange API keys

#include <QDialog>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>

namespace openmarketterminal::screens::crypto {

class CryptoCredentials : public QDialog {
    Q_OBJECT
  public:
    explicit CryptoCredentials(const QString& exchange_id, QWidget* parent = nullptr);

    QString api_key() const;
    QString api_secret() const;
    QString password() const;
    QString wallet_address() const;
    QString private_key() const;

    void set_values(const QString& key, const QString& secret, const QString& password,
                    const QString& wallet_address, const QString& private_key);

    // Put the dialog into "already connected" mode: pre-fill the non-secret
    // fields (key/password/wallet), show a green connected status, and let the
    // user save without re-typing the secret (left blank → keep current). The
    // secret/private-key are deliberately NOT pre-filled so they're never shown.
    void mark_connected(const QString& key, const QString& password, const QString& wallet_address);

  signals:
    void credentials_saved(const QString& api_key, const QString& api_secret, const QString& password,
                           const QString& wallet_address, const QString& private_key);

  protected:
    void changeEvent(QEvent* event) override;

  private slots:
    void on_save();
    void on_clear();
    void on_totp_tick();

  private:
    void retranslateUi();
    void refresh_totp();
    void set_status(const QString& text, const QString& object_name);

    QLineEdit* key_edit_ = nullptr;
    QLineEdit* secret_edit_ = nullptr;
    QPlainTextEdit* secret_multiline_ = nullptr;
    QLineEdit* password_edit_ = nullptr;
    QLineEdit* wallet_edit_ = nullptr;
    QLineEdit* private_key_edit_ = nullptr;
    QLabel* status_label_ = nullptr;
    QString exchange_id_;
    bool has_saved_secret_ = false; // connected mode — secret may be left blank to keep current

    // Text-bearing widgets (cached for retranslateUi)
    QLabel* title_label_ = nullptr;
    QLabel* info_label_ = nullptr;
    QLabel* key_field_label_ = nullptr;
    QLabel* secret_field_label_ = nullptr;
    QLabel* password_field_label_ = nullptr;
    QLabel* wallet_field_label_ = nullptr;
    QLabel* private_key_field_label_ = nullptr;
    QLabel* totp_field_label_ = nullptr;
    QPushButton* clear_btn_ = nullptr;
    QPushButton* save_btn_ = nullptr;

    // TOTP section
    QLineEdit* totp_secret_edit_ = nullptr;
    QLabel* totp_code_label_ = nullptr;
    QLabel* totp_countdown_label_ = nullptr;
    QTimer* totp_timer_ = nullptr;
};

} // namespace openmarketterminal::screens::crypto
