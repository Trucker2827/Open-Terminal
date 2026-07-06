// src/screens/excel/ExcelScreen.cpp
#include "screens/excel/ExcelScreen.h"

#include "core/logging/Logger.h"
#include "core/session/ScreenStateManager.h"
#include "python/PythonRunner.h"
#include "screens/excel/SpreadsheetWidget.h"
#include "services/llm/LlmService.h"
#include "services/file_manager/FileManagerService.h"
#include "ui/theme/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFuture>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTabBar>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#ifdef OPENMARKETTERMINAL_HAS_QXLSX
#include <xlsxdocument.h>
#endif

namespace openmarketterminal::screens {

using namespace openmarketterminal::ui;

static QString kAccent() {
    return QString("#ea580c");
} // Orange accent

static QJsonArray cells_to_json_trimmed(const QVector<QVector<QString>>& cells) {
    int last_row = -1;
    int last_col = -1;
    for (int r = 0; r < cells.size(); ++r) {
        for (int c = 0; c < cells[r].size(); ++c) {
            if (!cells[r][c].isEmpty()) {
                last_row = std::max(last_row, r);
                last_col = std::max(last_col, c);
            }
        }
    }

    QJsonArray rows;
    if (last_row < 0 || last_col < 0)
        return rows;

    for (int r = 0; r <= last_row; ++r) {
        QJsonArray row;
        for (int c = 0; c <= last_col; ++c)
            row.append(c < cells[r].size() ? cells[r][c] : QString());
        rows.append(row);
    }
    return rows;
}

static QVector<QVector<QString>> json_rows_to_cells(const QJsonArray& rows) {
    QVector<QVector<QString>> cells;
    cells.reserve(rows.size());
    for (const QJsonValue& row_value : rows) {
        const QJsonArray row_array = row_value.toArray();
        QVector<QString> row;
        row.reserve(row_array.size());
        for (const QJsonValue& cell : row_array) {
            if (cell.isNull() || cell.isUndefined())
                row.append(QString());
            else
                row.append(cell.toVariant().toString());
        }
        cells.append(row);
    }
    return cells;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

ExcelScreen::ExcelScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
}

// ─────────────────────────────────────────────────────────────────────────────
// Show / Hide
// ─────────────────────────────────────────────────────────────────────────────

void ExcelScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    LOG_INFO("ExcelScreen", "Screen shown");
}

void ExcelScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    LOG_INFO("ExcelScreen", "Screen hidden");
}

// ─────────────────────────────────────────────────────────────────────────────
// Build UI
// ─────────────────────────────────────────────────────────────────────────────

void ExcelScreen::build_ui() {
    setStyleSheet(QString("QWidget { background:%1; color:%2; }").arg(colors::BG_BASE(), colors::TEXT_PRIMARY()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Toolbar
    root->addWidget(build_toolbar());

    // Tab widget for sheets
    sheet_tabs_ = new QTabWidget(this);
    sheet_tabs_->setTabPosition(QTabWidget::South);
    sheet_tabs_->setMovable(true);
    sheet_tabs_->setTabsClosable(false); // We handle close via button
    sheet_tabs_->setStyleSheet(QString("QTabWidget::pane { border:none; background:%3; }"
                                       "QTabBar { background:%4; }"
                                       "QTabBar::tab { background:%5; color:%6; border:1px solid %7;"
                                       "  padding:4px 14px; font-family:%1; font-size:10px; margin-right:2px; }"
                                       "QTabBar::tab:selected { background:%2; color:%8; border-color:%2; }"
                                       "QTabBar::tab:hover { background:%7; }")
                                   .arg(fonts::DATA_FAMILY, kAccent(), colors::BG_HOVER(), colors::BORDER_MED(),
                                        colors::TEXT_DIM(), colors::TEXT_SECONDARY(), colors::TEXT_TERTIARY(),
                                        colors::TEXT_PRIMARY()));

    // Add initial sheet
    auto* sheet1 = new SpreadsheetWidget("Sheet1", 100, 26, sheet_tabs_);
    sheet_tabs_->addTab(sheet1, "Sheet1");

    connect(sheet_tabs_, &QTabWidget::currentChanged, this, &ExcelScreen::on_tab_changed);

    root->addWidget(sheet_tabs_, 1);

    // Status bar
    auto* status_bar = new QWidget(this);
    status_bar->setFixedHeight(24);
    status_bar->setStyleSheet(
        QString("background:%1; border-top:1px solid %2;").arg(colors::BG_HOVER(), colors::BORDER_MED()));

    auto* status_hl = new QHBoxLayout(status_bar);
    status_hl->setContentsMargins(12, 0, 12, 0);
    status_hl->setSpacing(12);

    status_label_ = new QLabel(this);
    status_label_->setStyleSheet(
        QString("color:%1; font-family:%2; font-size:9px;").arg(colors::TEXT_SECONDARY(), fonts::DATA_FAMILY));
    status_hl->addWidget(status_label_);
    status_hl->addStretch();

    root->addWidget(status_bar);

    update_status();
}

QWidget* ExcelScreen::build_toolbar() {
    auto* bar = new QWidget(this);
    bar->setFixedHeight(40);
    bar->setStyleSheet(QString("background:%1; border-bottom:1px solid %2;").arg(colors::BORDER_MED(), colors::TEXT_DIM()));

    auto* hl = new QHBoxLayout(bar);
    hl->setContentsMargins(12, 0, 12, 0);
    hl->setSpacing(4);

    // Title
    toolbar_title_ = new QLabel(tr("EXCEL SPREADSHEET"), bar);
    toolbar_title_->setStyleSheet(QString("color:%1; font-family:%2; font-size:11px; font-weight:700; margin-right:12px;")
                                      .arg(kAccent(), fonts::DATA_FAMILY));
    hl->addWidget(toolbar_title_);

    // Button factory
    auto make_btn = [&](const QString& text, const QString& tooltip = {}) -> QPushButton* {
        auto* btn = new QPushButton(text, bar);
        btn->setToolTip(tooltip);
        btn->setStyleSheet(
            QString("QPushButton { background:%3; color:%4; border:none;"
                    " font-family:%1; font-size:10px; font-weight:600; padding:6px 12px; }"
                    "QPushButton:hover { background:%5; }"
                    "QPushButton:pressed { background:%2; }")
                .arg(fonts::DATA_FAMILY, kAccent(), colors::TEXT_DIM(), colors::TEXT_PRIMARY(), colors::TEXT_TERTIARY()));
        return btn;
    };

    import_btn_ = make_btn(tr("IMPORT"), tr("Import XLSX/CSV file"));
    connect(import_btn_, &QPushButton::clicked, this, &ExcelScreen::on_import);
    hl->addWidget(import_btn_);

    export_btn_ = make_btn(tr("EXPORT"), tr("Export as XLSX"));
    connect(export_btn_, &QPushButton::clicked, this, &ExcelScreen::on_export);
    hl->addWidget(export_btn_);

    export_csv_btn_ = make_btn(tr("CSV"), tr("Export active sheet as CSV"));
    connect(export_csv_btn_, &QPushButton::clicked, this, &ExcelScreen::on_export_csv);
    hl->addWidget(export_csv_btn_);

    ai_analyze_btn_ = make_btn(tr("AI ANALYZE"), tr("Analyze the active sheet with the configured AI provider"));
    connect(ai_analyze_btn_, &QPushButton::clicked, this, &ExcelScreen::on_ai_analyze);
    hl->addWidget(ai_analyze_btn_);

    // Separator
    auto* sep1 = new QWidget(bar);
    sep1->setFixedSize(1, 20);
    sep1->setStyleSheet(QString("background:%1;").arg(colors::TEXT_TERTIARY()));
    hl->addWidget(sep1);

    add_sheet_btn_ = make_btn(tr("+ SHEET"), tr("Add new sheet"));
    connect(add_sheet_btn_, &QPushButton::clicked, this, &ExcelScreen::on_add_sheet);
    hl->addWidget(add_sheet_btn_);

    rename_btn_ = make_btn(tr("RENAME"), tr("Rename current sheet"));
    connect(rename_btn_, &QPushButton::clicked, this, &ExcelScreen::on_rename_sheet);
    hl->addWidget(rename_btn_);

    delete_btn_ = make_btn(tr("DELETE"), tr("Delete current sheet"));
    delete_btn_->setStyleSheet(QString("QPushButton { background:%2; color:%3; border:none;"
                                       " font-family:%1; font-size:10px; font-weight:600; padding:6px 12px; }"
                                       "QPushButton:hover { background:%4; }")
                                   .arg(fonts::DATA_FAMILY, colors::TEXT_DIM(), colors::TEXT_PRIMARY(), colors::NEGATIVE()));
    connect(delete_btn_, &QPushButton::clicked, this, &ExcelScreen::on_delete_sheet);
    hl->addWidget(delete_btn_);

    hl->addStretch();

    // File name label
    auto* fname_label = new QLabel(file_name_, bar);
    fname_label->setObjectName("excelFileName");
    fname_label->setStyleSheet(
        QString("color:%1; font-family:%2; font-size:10px;").arg(colors::TEXT_SECONDARY(), fonts::DATA_FAMILY));
    hl->addWidget(fname_label);

    return bar;
}

// ─────────────────────────────────────────────────────────────────────────────
// Import (XLSX/CSV via bundled Python spreadsheet helper)
// ─────────────────────────────────────────────────────────────────────────────

void ExcelScreen::on_import() {
    QString path = QFileDialog::getOpenFileName(this, tr("Import Spreadsheet"), {},
                                                tr("Spreadsheet Files (*.xlsx *.csv);;All Files (*)"));
    if (path.isEmpty())
        return;

    if (!python::PythonRunner::instance().is_available()) {
        QMessageBox::warning(this, tr("Import failed"), tr("Python environment is not ready, so XLSX/CSV import cannot run."));
        return;
    }

    QJsonObject args;
    args["source"] = "local";
    args["file_path"] = path;
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "csv") {
        args["operation"] = "read";
        args["sheet_name"] = "Sheet1";
        args["range"] = "A1:ZZ100000";
    } else {
        args["operation"] = "read_workbook";
    }

    const QString payload = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    QPointer<ExcelScreen> self = this;
    import_btn_->setEnabled(false);
    import_btn_->setText(tr("IMPORTING"));
    python::PythonRunner::instance().run(
        "spreadsheet.py", {"--args", payload},
        [self, path](const python::PythonResult& result) {
            if (!self)
                return;
            self->import_btn_->setEnabled(true);
            self->import_btn_->setText(self->tr("IMPORT"));
            if (!result.success) {
                QMessageBox::warning(self, self->tr("Import failed"),
                                     result.error.isEmpty() ? self->tr("Spreadsheet import failed.") : result.error);
                return;
            }

            const QString json = python::extract_json(result.output);
            const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
            if (!doc.isObject()) {
                QMessageBox::warning(self, self->tr("Import failed"), self->tr("Spreadsheet helper returned invalid JSON."));
                return;
            }
            const QJsonObject obj = doc.object();
            if (obj.contains("error")) {
                QMessageBox::warning(self, self->tr("Import failed"), obj.value("error").toString());
                return;
            }
            self->import_workbook_from_result(obj, path);
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Export (XLSX via bundled Python spreadsheet helper)
// ─────────────────────────────────────────────────────────────────────────────

void ExcelScreen::on_export() {
    QString path = QFileDialog::getSaveFileName(this, tr("Export as XLSX"), file_name_, tr("Excel Files (*.xlsx)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(".xlsx", Qt::CaseInsensitive))
        path += ".xlsx";

    if (!python::PythonRunner::instance().is_available()) {
        QMessageBox::warning(this, tr("Export failed"), tr("Python environment is not ready, so XLSX export cannot run."));
        return;
    }

    QJsonObject args;
    args["source"] = "local";
    args["operation"] = "write_workbook";
    args["file_path"] = path;
    args["sheets"] = workbook_to_json();

    const QString payload = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    QPointer<ExcelScreen> self = this;
    export_btn_->setEnabled(false);
    export_btn_->setText(tr("EXPORTING"));
    python::PythonRunner::instance().run(
        "spreadsheet.py", {"--args", payload},
        [self, path](const python::PythonResult& result) {
            if (!self)
                return;
            self->export_btn_->setEnabled(true);
            self->export_btn_->setText(self->tr("EXPORT"));
            if (!result.success) {
                QMessageBox::warning(self, self->tr("Export failed"),
                                     result.error.isEmpty() ? self->tr("Spreadsheet export failed.") : result.error);
                return;
            }
            const QString json = python::extract_json(result.output);
            const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
            const QJsonObject obj = doc.object();
            if (!doc.isObject() || obj.contains("error")) {
                QMessageBox::warning(self, self->tr("Export failed"),
                                     obj.value("error").toString(self->tr("Spreadsheet helper returned invalid JSON.")));
                return;
            }

            self->file_name_ = QFileInfo(path).fileName();
            self->file_path_ = path;
            if (auto* fname = self->findChild<QLabel*>("excelFileName"))
                fname->setText(self->file_name_);
            self->update_status();
            services::FileManagerService::instance().import_file(path, "excel");
            LOG_INFO("ExcelScreen", QString("Exported XLSX to %1").arg(path));
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Export CSV (active sheet)
// ─────────────────────────────────────────────────────────────────────────────

void ExcelScreen::on_export_csv() {
    auto* sheet = current_sheet();
    if (!sheet)
        return;

    QString path =
        QFileDialog::getSaveFileName(this, tr("Export CSV"), sheet->sheet_name() + ".csv", tr("CSV Files (*.csv)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export failed"), tr("Could not open file for writing:\n%1").arg(path));
        return;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    auto cells = sheet->get_data();

    // Find the last row/col with data to avoid huge trailing empty rows
    int last_row = 0;
    int last_col = 0;
    for (int r = 0; r < cells.size(); ++r) {
        for (int c = 0; c < cells[r].size(); ++c) {
            if (!cells[r][c].isEmpty()) {
                last_row = std::max(last_row, r);
                last_col = std::max(last_col, c);
            }
        }
    }

    for (int r = 0; r <= last_row; ++r) {
        QStringList row;
        for (int c = 0; c <= last_col; ++c) {
            QString val = (c < cells[r].size()) ? cells[r][c] : "";
            if (val.contains(',') || val.contains('"') || val.contains('\n'))
                val = "\"" + val.replace("\"", "\"\"") + "\"";
            row << val;
        }
        out << row.join(",") << "\n";
    }

    LOG_INFO("ExcelScreen", QString("Exported CSV to %1").arg(path));

    // Register with File Manager so it appears in the Files tab
    services::FileManagerService::instance().import_file(path, "excel");
}

void ExcelScreen::import_workbook_from_result(const QJsonObject& result, const QString& source_path) {
    QJsonArray sheets;
    if (result.value("operation").toString() == "read_workbook") {
        sheets = result.value("sheets").toArray();
    } else {
        QJsonObject sheet;
        sheet["name"] = QFileInfo(source_path).completeBaseName().isEmpty()
                            ? QStringLiteral("Sheet1")
                            : QFileInfo(source_path).completeBaseName();
        sheet["data"] = result.value("data").toArray();
        sheets.append(sheet);
    }

    if (sheets.isEmpty()) {
        QMessageBox::warning(this, tr("Import failed"), tr("No sheets or rows were found in the spreadsheet."));
        return;
    }

    while (sheet_tabs_->count() > 0) {
        auto* w = sheet_tabs_->widget(0);
        sheet_tabs_->removeTab(0);
        w->deleteLater();
    }

    for (const QJsonValue& sheet_value : sheets) {
        const QJsonObject sheet_obj = sheet_value.toObject();
        const QString name = sheet_obj.value("name").toString(QStringLiteral("Sheet%1").arg(sheet_tabs_->count() + 1));
        auto cells = json_rows_to_cells(sheet_obj.value("data").toArray());
        const int row_count = std::max(static_cast<int>(cells.size()), 100);
        auto* sheet = new SpreadsheetWidget(name, row_count, 26, sheet_tabs_);
        sheet->set_data(cells);
        sheet->set_sheet_name(name);
        sheet_tabs_->addTab(sheet, name);
    }

    file_name_ = QFileInfo(source_path).fileName();
    file_path_ = source_path;
    if (auto* fname = findChild<QLabel*>("excelFileName"))
        fname->setText(file_name_);
    sheet_counter_ = sheet_tabs_->count() + 1;
    sheet_tabs_->setCurrentIndex(0);
    update_status();
    services::FileManagerService::instance().import_file(source_path, "excel");
    LOG_INFO("ExcelScreen", QString("Imported spreadsheet %1 (%2 sheets)").arg(file_name_).arg(sheet_tabs_->count()));
}

QJsonArray ExcelScreen::workbook_to_json() const {
    QJsonArray sheets;
    if (!sheet_tabs_)
        return sheets;
    for (int i = 0; i < sheet_tabs_->count(); ++i) {
        auto* sheet = qobject_cast<SpreadsheetWidget*>(sheet_tabs_->widget(i));
        if (!sheet)
            continue;
        QJsonObject obj;
        obj["name"] = sheet->sheet_name();
        obj["data"] = cells_to_json_trimmed(sheet->get_data());
        sheets.append(obj);
    }
    return sheets;
}

QString ExcelScreen::active_sheet_preview_markdown(int max_rows, int max_cols) const {
    auto* sheet = current_sheet();
    if (!sheet)
        return {};
    const auto cells = sheet->get_data();
    const int rows = std::min(max_rows, static_cast<int>(cells.size()));
    int cols = 0;
    for (int r = 0; r < rows; ++r)
        cols = std::max(cols, std::min(max_cols, static_cast<int>(cells[r].size())));
    if (rows <= 0 || cols <= 0)
        return {};

    QString out = QString("Sheet: %1\nRows sampled: %2 of %3\n\n")
                      .arg(sheet->sheet_name())
                      .arg(rows)
                      .arg(sheet->row_count());
    for (int r = 0; r < rows; ++r) {
        QStringList row;
        for (int c = 0; c < cols; ++c) {
            QString value = (c < cells[r].size()) ? cells[r][c] : QString();
            value.replace('\n', ' ');
            if (value.length() > 80)
                value = value.left(77) + "...";
            row << value;
        }
        out += row.join(" | ") + "\n";
    }
    return out;
}

void ExcelScreen::show_text_dialog(const QString& title, const QString& text) {
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.resize(760, 520);
    dlg.setStyleSheet(QString("QDialog{background:%1;color:%2;}"
                              "QTextEdit{background:%3;color:%2;border:1px solid %4;"
                              "font-family:%5;font-size:12px;padding:10px;}"
                              "QPushButton{background:%3;color:%2;border:1px solid %4;"
                              "padding:6px 16px;font-family:%5;font-weight:700;}"
                              "QPushButton:hover{border-color:%6;}")
                          .arg(colors::BG_SURFACE(), colors::TEXT_PRIMARY(), colors::BG_BASE(),
                               colors::BORDER_DIM(), fonts::DATA_FAMILY, kAccent()));
    auto* vl = new QVBoxLayout(&dlg);
    auto* edit = new QTextEdit(&dlg);
    edit->setReadOnly(true);
    edit->setPlainText(text);
    vl->addWidget(edit, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto* copy_btn = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    connect(copy_btn, &QPushButton::clicked, this, [text]() { QApplication::clipboard()->setText(text); });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    vl->addWidget(buttons);
    dlg.exec();
}

void ExcelScreen::on_ai_analyze() {
    const QString preview = active_sheet_preview_markdown();
    if (preview.trimmed().isEmpty()) {
        QMessageBox::information(this, tr("AI Analyze"), tr("The active sheet is empty."));
        return;
    }
    if (!ai_chat::LlmService::instance().is_configured()) {
        QMessageBox::information(this, tr("AI Analyze"),
                                 tr("No AI provider is configured. Add Ollama, OpenAI, Anthropic, or another provider in Settings."));
        return;
    }

    const QString prompt =
        tr("You are a financial spreadsheet analyst. Analyze this spreadsheet preview.\n"
           "Give concise output with: structure, likely purpose, key figures, formula/data issues, risk flags, and next useful actions.\n\n%1")
            .arg(preview);

    ai_analyze_btn_->setEnabled(false);
    ai_analyze_btn_->setText(tr("AI..."));
    QPointer<ExcelScreen> self = this;
    auto future = QtConcurrent::run([self, prompt]() {
        auto response = ai_chat::LlmService::instance().chat(prompt, {}, false);
        QMetaObject::invokeMethod(qApp, [self, response]() {
            if (!self)
                return;
            self->ai_analyze_btn_->setEnabled(true);
            self->ai_analyze_btn_->setText(self->tr("AI ANALYZE"));
            if (!response.success) {
                QMessageBox::warning(self, self->tr("AI Analyze failed"),
                                     response.error.isEmpty() ? self->tr("AI request failed.") : response.error);
                return;
            }
            self->show_text_dialog(self->tr("AI Spreadsheet Analysis"), response.content);
        }, Qt::QueuedConnection);
    });
    Q_UNUSED(future);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sheet management
// ─────────────────────────────────────────────────────────────────────────────

void ExcelScreen::on_add_sheet() {
    QString name = generate_sheet_name();
    auto* sheet = new SpreadsheetWidget(name, 100, 26, sheet_tabs_);
    sheet_tabs_->addTab(sheet, name);
    sheet_tabs_->setCurrentIndex(sheet_tabs_->count() - 1);
    update_status();
}

void ExcelScreen::on_delete_sheet() {
    if (sheet_tabs_->count() <= 1) {
        QMessageBox::warning(this, tr("Cannot Delete"), tr("Cannot delete the last sheet."));
        return;
    }

    int idx = sheet_tabs_->currentIndex();
    QString name = sheet_tabs_->tabText(idx);

    auto reply = QMessageBox::question(this, tr("Delete Sheet"), tr("Delete \"%1\"? This cannot be undone.").arg(name),
                                       QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        auto* w = sheet_tabs_->widget(idx);
        sheet_tabs_->removeTab(idx);
        w->deleteLater();
        update_status();
    }
}

void ExcelScreen::on_rename_sheet() {
    int idx = sheet_tabs_->currentIndex();
    if (idx < 0)
        return;

    bool ok = false;
    QString current = sheet_tabs_->tabText(idx);
    QString name = QInputDialog::getText(this, tr("Rename Sheet"), tr("New name:"), QLineEdit::Normal, current, &ok);

    if (ok && !name.trimmed().isEmpty()) {
        sheet_tabs_->setTabText(idx, name.trimmed());
        auto* sheet = qobject_cast<SpreadsheetWidget*>(sheet_tabs_->widget(idx));
        if (sheet)
            sheet->set_sheet_name(name.trimmed());
        update_status();
    }
}

void ExcelScreen::on_tab_changed(int index) {
    Q_UNUSED(index);
    update_status();
    ScreenStateManager::instance().notify_changed(this);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

SpreadsheetWidget* ExcelScreen::current_sheet() const {
    return qobject_cast<SpreadsheetWidget*>(sheet_tabs_->currentWidget());
}

QString ExcelScreen::generate_sheet_name() const {
    int n = sheet_tabs_->count() + 1;
    QString name;
    do {
        name = QString("Sheet%1").arg(n++);
    } while (sheet_tabs_->indexOf(sheet_tabs_->findChild<QWidget*>(name)) >= 0 || n > 999);
    return name;
}

void ExcelScreen::update_status() {
    auto* sheet = current_sheet();
    QString info = tr("File: %1  |  Sheets: %2").arg(file_name_).arg(sheet_tabs_->count());
    if (sheet) {
        info += tr("  |  Active: %1  |  %2 rows x %3 cols")
                    .arg(sheet->sheet_name())
                    .arg(sheet->row_count())
                    .arg(sheet->col_count());
    }
    if (status_label_)
        status_label_->setText(info);
}

// ── Live language switch ─────────────────────────────────────────────────────

void ExcelScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void ExcelScreen::retranslateUi() {
    if (toolbar_title_) toolbar_title_->setText(tr("EXCEL SPREADSHEET"));
    if (import_btn_) {
        import_btn_->setText(tr("IMPORT"));
        import_btn_->setToolTip(tr("Import XLSX/CSV file"));
    }
    if (export_btn_) {
        export_btn_->setText(tr("EXPORT"));
        export_btn_->setToolTip(tr("Export as XLSX"));
    }
    if (export_csv_btn_) {
        export_csv_btn_->setText(tr("CSV"));
        export_csv_btn_->setToolTip(tr("Export active sheet as CSV"));
    }
    if (ai_analyze_btn_) {
        ai_analyze_btn_->setText(tr("AI ANALYZE"));
        ai_analyze_btn_->setToolTip(tr("Analyze the active sheet with the configured AI provider"));
    }
    if (add_sheet_btn_) {
        add_sheet_btn_->setText(tr("+ SHEET"));
        add_sheet_btn_->setToolTip(tr("Add new sheet"));
    }
    if (rename_btn_) {
        rename_btn_->setText(tr("RENAME"));
        rename_btn_->setToolTip(tr("Rename current sheet"));
    }
    if (delete_btn_) {
        delete_btn_->setText(tr("DELETE"));
        delete_btn_->setToolTip(tr("Delete current sheet"));
    }
    update_status(); // re-render the status bar summary in the new language
}

// ── IStatefulScreen ───────────────────────────────────────────────────────────

QVariantMap ExcelScreen::save_state() const {
    // Persist every sheet's cell contents/formulas (not just the active tab)
    // so the user's work survives closing the screen or the app.
    QVariantList sheets;
    if (sheet_tabs_) {
        for (int i = 0; i < sheet_tabs_->count(); ++i) {
            auto* sheet = qobject_cast<SpreadsheetWidget*>(sheet_tabs_->widget(i));
            if (sheet)
                sheets.append(sheet->serialize().toVariantMap());
        }
    }
    return {
        {"tab_index", sheet_tabs_ ? sheet_tabs_->currentIndex() : 0},
        {"sheets", sheets},
    };
}

void ExcelScreen::restore_state(const QVariantMap& state) {
    const QVariantList sheets = state.value("sheets").toList();

    if (sheet_tabs_ && !sheets.isEmpty()) {
        // Drop the default "Sheet1" created in build_ui() before recreating the
        // saved tabs (mirrors the tab-clearing loop in on_import()).
        while (sheet_tabs_->count() > 0) {
            auto* w = sheet_tabs_->widget(0);
            sheet_tabs_->removeTab(0);
            w->deleteLater();
        }

        for (const auto& v : sheets) {
            const QJsonObject obj = QJsonObject::fromVariantMap(v.toMap());
            auto* sheet = new SpreadsheetWidget("Sheet1", 100, 26, sheet_tabs_);
            sheet->deserialize(obj);
            sheet_tabs_->addTab(sheet, sheet->sheet_name());
        }

        sheet_counter_ = sheets.size() + 1; // keep generated sheet names unique
        update_status();
    }

    const int idx = state.value("tab_index", 0).toInt();
    if (sheet_tabs_ && idx >= 0 && idx < sheet_tabs_->count())
        sheet_tabs_->setCurrentIndex(idx);
}

} // namespace openmarketterminal::screens
