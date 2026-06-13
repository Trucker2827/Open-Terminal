#pragma once
#include <QByteArray>

// OS-keystore custody for SecureStorage's AES master key.
//
// This is deliberately a SEPARATE translation unit (excluded from the unity
// build) so the platform keystore headers — Apple's <Security/Security.h> /
// <CoreFoundation/CoreFoundation.h>, which drag in MacTypes (Boolean, Point,
// Rect, Fixed, …) — never leak into the batched unity sources and clash there.

namespace openmarketterminal::secure_detail {

/// Fetch the SecureStorage master key from the OS keychain, creating a fresh
/// random key on first use. Returns a `key_len`-byte key on success.
///
/// Contract / safety:
///   * The newly created key is read back and byte-compared before being
///     returned, so a keystore that silently fails to persist never becomes the
///     active encryption key (which would orphan every credential written under
///     it). On any failure — unsupported platform, keystore error, round-trip
///     mismatch — this returns an EMPTY QByteArray and the caller MUST fall back
///     to the legacy machine-derived key (never encrypt under an unverified key).
///   * Pure read/create of the key material; performs no SQLite access.
///   * Call once at startup on the main thread and cache the result; the macOS
///     Keychain item uses kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly so a
///     cached copy is usable from worker threads without a blocking prompt.
///
/// Currently implemented for macOS only. Windows (DPAPI/Credential Manager) and
/// Linux (libsecret) return empty → callers keep the existing machine-derived
/// key, i.e. no behaviour change on those platforms (see KeychainKey.cpp TODO).
QByteArray keychain_master_key(int key_len);

}  // namespace openmarketterminal::secure_detail
