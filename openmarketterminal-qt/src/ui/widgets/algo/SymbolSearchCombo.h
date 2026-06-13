// src/ui/widgets/algo/SymbolSearchCombo.h
#pragma once
#include <QComboBox>
#include <QCompleter>
#include <QStringListModel>

namespace openmarketterminal::ui::algo {

class SymbolSearchCombo : public QComboBox {
    Q_OBJECT
public:
    explicit SymbolSearchCombo(QWidget* parent = nullptr);
    void set_symbols(const QStringList& symbols);

private:
    QCompleter* completer_ = nullptr;
    QStringListModel* model_ = nullptr;
};

} // namespace openmarketterminal::ui::algo
