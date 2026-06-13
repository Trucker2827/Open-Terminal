#pragma once

#include "core/result/Result.h"

#include <QString>

namespace openmarketterminal {

/// Local-only backup / restore of all the user's data. OpenMarketTerminal stores
/// everything on the user's own machine (SQLite + files); this lets them copy
/// that data to another machine or an external drive with no cloud involved.
///
/// A backup is a single self-contained .zip file (openable by any tool):
///   manifest.json              — app + version + schema + timestamp + notes
///   data/openmarketterminal.db — consistent VACUUM INTO snapshot
///   data/cache.db              — consistent VACUUM INTO snapshot
///   workspace.db               — consistent VACUUM INTO snapshot
///   layouts/  workspaces/  files/   — copied trees
///
/// What is NOT backed up: secrets (broker / API keys) live in the OS keychain,
/// not in these files, and are never exported. After restoring on a new machine
/// the user re-enters them. The backup is UNENCRYPTED personal data (portfolios,
/// watchlists, notes) — store it somewhere safe.
///
/// Restore is staged-on-restart so live, open databases are never overwritten in
/// place (which could corrupt them): import_backup() validates + stages the files
/// and asks the app to quit; on the next launch, before any DB is opened,
/// apply_pending_restore() swaps them in (keeping a pre-restore copy for undo).
class BackupService {
  public:
    static BackupService& instance();

    /// Write a complete backup as a single timestamped .zip in `dest_parent_dir`.
    /// Returns the created .zip path on success. Safe to call while the app is
    /// running (uses VACUUM INTO for live DBs).
    Result<QString> export_backup(const QString& dest_parent_dir);

    /// Validate `backup_zip` (must contain a compatible manifest.json), then
    /// STAGE it for restore and write the pending marker. The caller must then
    /// quit the app; the restore is applied on the next launch. Refuses a backup
    /// whose schema is NEWER than this build (would corrupt an older binary).
    Result<void> stage_import(const QString& backup_zip);

    /// Called once at startup, BEFORE any database is opened. If a restore was
    /// staged, move the current data aside (pre-restore-<ts>/ for undo), swap the
    /// staged files into place, and clear the marker. No-op if nothing pending.
    static Result<void> apply_pending_restore();

  private:
    BackupService() = default;
};

}  // namespace openmarketterminal
