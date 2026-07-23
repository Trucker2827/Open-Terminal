#pragma once

#include <QDialog>
#include <QHash>

class QLabel;
class QPushButton;

namespace openmarketterminal::screens::crypto {

// Unified crypto venue manager. Credentials remain owned by each
// ExchangeSession/SecureStorage entry; this dialog only presents connection
// state and selects which already-independent session is active for trading.
class CryptoExchangeAccountsDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit CryptoExchangeAccountsDialog(const QString& active_exchange, QWidget* parent = nullptr);

  signals:
    void active_exchange_requested(const QString& exchange_id);

  private:
    struct RowWidgets {
        QLabel* status = nullptr;
        QLabel* role = nullptr;
        QPushButton* use = nullptr;
        QPushButton* configure = nullptr;
    };

    void add_exchange_row(const QString& exchange_id, bool featured);
    void configure_exchange(const QString& exchange_id);
    void select_exchange(const QString& exchange_id);
    void refresh_rows();

    QString active_exchange_;
    QHash<QString, RowWidgets> rows_;
};

} // namespace openmarketterminal::screens::crypto
