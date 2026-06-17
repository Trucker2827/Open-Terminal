#pragma once
// Read-only Dashboard widgets for ownership data (Insiders / Institutions /
// Politicians). One BaseWidget subclass driven by config + a row-mapper; the
// factories below wire each variant. Data via GovDataService (async + cached);
// display only, never trades.
#include "screens/dashboard/widgets/BaseWidget.h"
#include "services/gov_data/GovDataService.h"

#include <QJsonObject>
#include <QStringList>
#include <functional>

class QStackedWidget;
class QLabel;

namespace openmarketterminal::ui { class DataTable; }

namespace openmarketterminal::screens::widgets {

using OwnershipRowMapper = std::function<QVector<QStringList>(const QJsonObject&)>;

class OwnershipWidget : public BaseWidget {
    Q_OBJECT
  public:
    OwnershipWidget(const QString& title, const QString& script, const QString& command,
                    const QStringList& args, const QStringList& headers, const QString& request_id,
                    OwnershipRowMapper mapper, const QString& empty_msg, QWidget* parent = nullptr);

  protected:
    void showEvent(QShowEvent* e) override;

  private slots:
    void on_result(const QString& request_id, const services::GovDataResult& result);

  private:
    void refresh();

    QString script_;
    QString command_;
    QString request_id_;
    QString empty_msg_;
    QStringList args_;
    QStringList headers_;
    OwnershipRowMapper mapper_;

    QStackedWidget* stack_ = nullptr;
    QLabel* status_ = nullptr;
    ui::DataTable* table_ = nullptr;
    bool loaded_once_ = false;
};

BaseWidget* create_insider_widget(const QJsonObject& cfg = {});
BaseWidget* create_institution_widget(const QJsonObject& cfg = {});
BaseWidget* create_politician_widget(const QJsonObject& cfg = {});

} // namespace openmarketterminal::screens::widgets
