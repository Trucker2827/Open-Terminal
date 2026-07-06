#include "screens/dashboard/widgets/DataLakeHealthWidget.h"

#include "storage/LocalDataLake.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace openmarketterminal::screens::widgets {

namespace {
struct DatasetRow {
    QString name;
    int files = 0;
    qint64 bytes = 0;
    qint64 modified_ms = 0;
};

qint64 json_i64(const QJsonObject& o, const QString& key) {
    const QJsonValue v = o.value(key);
    if (v.isDouble())
        return static_cast<qint64>(v.toDouble());
    if (v.isString())
        return v.toString().toLongLong();
    return 0;
}

QString duckdb_path() {
    const QString found = QStandardPaths::findExecutable(QStringLiteral("duckdb"));
    if (!found.isEmpty())
        return found;
    for (const QString& p : {QStringLiteral("/opt/homebrew/bin/duckdb"),
                            QStringLiteral("/usr/local/bin/duckdb")}) {
        if (QFileInfo::exists(p))
            return p;
    }
    return {};
}
} // namespace

DataLakeHealthWidget::DataLakeHealthWidget(const QJsonObject& cfg, QWidget* parent)
    : BaseWidget(tr("DATA LAKE HEALTH"), parent, ui::colors::CYAN()) {
    auto* vl = content_layout();
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(6);

    status_label_ = new QLabel(this);
    status_label_->setAlignment(Qt::AlignCenter);
    status_label_->setWordWrap(true);
    vl->addWidget(status_label_);

    path_label_ = new QLabel(this);
    path_label_->setWordWrap(true);
    vl->addWidget(path_label_);

    table_ = new QTableWidget(0, 4, this);
    table_->setHorizontalHeaderLabels({tr("Dataset"), tr("Files"), tr("Size"), tr("Fresh")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setShowGrid(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    vl->addWidget(table_, 1);

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &DataLakeHealthWidget::refresh_data);
    connect(this, &BaseWidget::refresh_requested, this, &DataLakeHealthWidget::refresh_data);

    set_configurable(true);
    apply_styles();
    apply_config(cfg);
}

QJsonObject DataLakeHealthWidget::config() const {
    return QJsonObject{{"refresh_sec", refresh_sec_}};
}

void DataLakeHealthWidget::apply_config(const QJsonObject& cfg) {
    refresh_sec_ = qBound(5, cfg.value("refresh_sec").toInt(refresh_sec_), 300);
    if (timer_)
        timer_->setInterval(refresh_sec_ * 1000);
    refresh_data();
}

void DataLakeHealthWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    refresh_data();
    timer_->start(refresh_sec_ * 1000);
}

void DataLakeHealthWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    timer_->stop();
}

void DataLakeHealthWidget::refresh_data() {
    QString error;
    auto& lake = storage::LocalDataLake::instance();
    const bool ok = lake.ensure(&error);
    const QJsonObject status = lake.status();
    const QString duckdb = duckdb_path();

    const bool manifest_ok = status.value("manifest_exists").toBool();
    const QString state = ok && manifest_ok && !duckdb.isEmpty() ? tr("READY") : tr("CHECK");
    const QString duck = duckdb.isEmpty() ? tr("DuckDB missing") : tr("DuckDB ready");
    status_label_->setText(QString("%1  %2").arg(state, duck));
    status_label_->setStyleSheet(QString("color:%1;font-size:18px;font-weight:900;background:transparent;")
                                     .arg(ok && !duckdb.isEmpty() ? ui::colors::POSITIVE()
                                                                  : ui::colors::ORANGE()));

    path_label_->setText(error.isEmpty() ? status.value("root").toString() : error);

    QVector<DatasetRow> rows;
    for (const auto& v : status.value("datasets").toArray()) {
        const QJsonObject o = v.toObject();
        DatasetRow row;
        row.name = o.value("dataset").toString();
        row.files = o.value("files").toInt();
        row.bytes = json_i64(o, QStringLiteral("bytes"));
        row.modified_ms = json_i64(o, QStringLiteral("last_modified_ms"));
        rows.append(row);
    }
    std::sort(rows.begin(), rows.end(), [](const DatasetRow& a, const DatasetRow& b) {
        if (a.modified_ms != b.modified_ms)
            return a.modified_ms > b.modified_ms;
        if (a.files != b.files)
            return a.files > b.files;
        return a.name < b.name;
    });

    table_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        table_->setItem(i, 0, new QTableWidgetItem(r.name));
        table_->setItem(i, 1, new QTableWidgetItem(QString::number(r.files)));
        table_->setItem(i, 2, new QTableWidgetItem(fmt_bytes(r.bytes)));
        table_->setItem(i, 3, new QTableWidgetItem(fmt_age(r.modified_ms)));
    }
    set_loading(false);
}

QString DataLakeHealthWidget::fmt_bytes(qint64 bytes) {
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    const double kb = bytes / 1024.0;
    if (kb < 1024.0)
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(kb / 1024.0, 0, 'f', 2);
}

QString DataLakeHealthWidget::fmt_age(qint64 last_modified_ms) {
    if (last_modified_ms <= 0)
        return QStringLiteral("-");
    const qint64 age_sec = std::max<qint64>(0, (QDateTime::currentMSecsSinceEpoch() - last_modified_ms) / 1000);
    if (age_sec < 60)
        return QStringLiteral("%1s").arg(age_sec);
    if (age_sec < 3600)
        return QStringLiteral("%1m").arg(age_sec / 60);
    if (age_sec < 86400)
        return QStringLiteral("%1h").arg(age_sec / 3600);
    return QStringLiteral("%1d").arg(age_sec / 86400);
}

QDialog* DataLakeHealthWidget::make_config_dialog(QWidget* parent) {
    auto* dlg = new QDialog(parent);
    dlg->setWindowTitle(tr("Configure - Data Lake Health"));
    auto* form = new QFormLayout(dlg);

    auto* spin = new QSpinBox(dlg);
    spin->setRange(5, 300);
    spin->setSuffix(tr(" sec"));
    spin->setValue(refresh_sec_);
    form->addRow(tr("Refresh"), spin);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    form->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, dlg, [this, dlg, spin]() {
        QJsonObject cfg{{"refresh_sec", spin->value()}};
        apply_config(cfg);
        emit config_changed(cfg);
        dlg->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    return dlg;
}

void DataLakeHealthWidget::on_theme_changed() {
    apply_styles();
}

void DataLakeHealthWidget::apply_styles() {
    path_label_->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;")
                                   .arg(ui::colors::TEXT_TERTIARY()));
    table_->setStyleSheet(QString(
        "QTableWidget{background:transparent;color:%1;gridline-color:%2;font-size:10px;border:none;}"
        "QHeaderView::section{background:%3;color:%4;border:none;border-bottom:1px solid %2;"
        "padding:2px 4px;font-size:9px;font-weight:bold;}"
        "QTableWidget::item{padding:2px 4px;}")
        .arg(ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), ui::colors::BG_RAISED(),
             ui::colors::TEXT_TERTIARY()));
}

void DataLakeHealthWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("DATA LAKE HEALTH"));
    table_->setHorizontalHeaderLabels({tr("Dataset"), tr("Files"), tr("Size"), tr("Fresh")});
    refresh_data();
}

} // namespace openmarketterminal::screens::widgets
