#pragma once
#include "core/result/Result.h"

#include <QString>

namespace openmarketterminal {

/// Local credential store backed by SQLite + AES-256-GCM.
///
/// Values are encrypted with a 256-bit key. Custody of that key is the security
/// boundary:
///   * macOS: a random 256-bit key stored in the OS Keychain
///     (kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly). It is NOT derivable
///     from anything on disk, so copying the SQLite file to another machine —
///     or reading it as the same user without the Keychain — cannot decrypt it.
///   * Windows/Linux (not yet implemented): falls back to the legacy key below.
///   * Legacy fallback (and permanent decrypt path for older rows): a key
///     derived from `QSysInfo::machineUniqueId()` + a fixed app salt via SHA-256.
///     This is reconstructible by any process running as the same user, so it is
///     a weaker tier kept only for backward compatibility / migration.
///
/// Each row carries its own random 96-bit IV and 128-bit GCM tag, so a tampered
/// ciphertext fails `retrieve()` rather than returning corrupted plaintext.
///
/// Migration is lazy and fail-safe: rows written under the legacy key are still
/// decryptable (retrieve tries the active key, then the legacy key) and are
/// transparently re-encrypted under the keychain key on first successful read.
/// A decrypt failure NEVER deletes the row — it surfaces as "not found" so the
/// caller re-prompts. A keychain key that fails to round-trip is rejected, so
/// credentials are never encrypted under an unverified/unrecoverable key.
///
/// Behaviour change vs. the old machine-derived scheme: a Keychain reset (or a
/// fresh user account) means stored credentials must be re-entered; profile
/// copies to another machine intentionally no longer decrypt.
///
/// What this does NOT protect against: forensic dumps of live process memory,
/// or malware running as the user that can drive the Keychain API.
///
/// Latency: AES-NI / ARM-Crypto-Extensions accelerate AES-GCM in hardware.
/// Per-call cost is dominated by the SQLite write — well under 1ms.
class SecureStorage {
  public:
    static SecureStorage& instance();

    /// Resolve and cache the active master key. Call ONCE at startup on the main
    /// thread (after Database::open), before any worker thread reads credentials:
    /// on macOS it touches the Keychain, which must happen on the main thread to
    /// avoid a blocking prompt / thread-safety issues on a worker. Idempotent.
    /// If the keychain is unavailable it logs and uses the legacy key.
    void init();

    Result<void> store(const QString& key, const QString& value);
    Result<QString> retrieve(const QString& key);
    Result<void> remove(const QString& key);

  private:
    SecureStorage() = default;
};

} // namespace openmarketterminal
