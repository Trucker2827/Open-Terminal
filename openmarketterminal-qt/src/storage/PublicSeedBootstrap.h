#pragma once

#include <QString>

namespace openmarketterminal::storage {

struct PublicSeedInstallResult {
    bool ok = true;
    bool installed = false;
    QString message;
    QString database_path;
};

/// Installs an already-sanitized public seed beside, never into, the live DB.
/// Missing bundled data is a normal no-op. Invalid data fails closed.
class PublicSeedBootstrap final {
public:
    static PublicSeedInstallResult install_from_directory(const QString& source_directory,
                                                          const QString& data_directory);
    static PublicSeedInstallResult install_bundled_seed(const QString& data_directory);
};

} // namespace openmarketterminal::storage
