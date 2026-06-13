#pragma once
#include <QSettings>
#include <QString>
#include <QVariant>

namespace openmarketterminal {

/// Application configuration backed by QSettings (persistent across sessions).
class AppConfig {
  public:
    static AppConfig& instance();

    QVariant get(const QString& key, const QVariant& default_val = {}) const;
    void set(const QString& key, const QVariant& value);
    void remove(const QString& key);

    // Typed accessors for common settings
    QString api_base_url() const;
    QString cloud_base_url() const; // (legacy cloud-sync base URL — cloud sync was removed; kept inert)
    bool dark_mode() const;
    int refresh_interval_ms() const;

  private:
    AppConfig();
    QSettings settings_;
};

} // namespace openmarketterminal
