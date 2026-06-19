#pragma once
// TouchIdAuth — Touch ID biometric authentication bridge.
// This header is pure C++ (no ObjC types) so it can be included by
// Qt/C++ translation units on all platforms.
//
// On non-Apple platforms the .mm is not compiled; callers must guard
// invocations with #ifdef Q_OS_MACOS.

#include <QString>
#include <functional>

namespace openmarketterminal::auth {

/// Touch ID / biometric authentication via LocalAuthentication.
///
/// is_available() is synchronous and safe to call on the UI thread.
/// authenticate() is asynchronous; the done callback is always invoked
/// on the Qt main thread exactly once.
///
/// DeviceOwnerAuthenticationWithBiometrics = Touch ID only (no password
/// system fallback). Our own 4-digit PIN serves as the in-app fallback.
class TouchIdAuth {
  public:
    /// True if Touch ID is present and enrolled (biometrics configured on this
    /// Mac). Safe to call from the Qt main thread; synchronous.
    static bool is_available();

    /// Prompt the user with a Touch ID fingerprint sheet.
    /// reason — short localised string shown in the LA prompt.
    /// done   — called once on the Qt main thread:
    ///            done(true,  "")             on success
    ///            done(false, errorString)    on cancel / failure
    static void authenticate(const QString& reason,
                             std::function<void(bool ok, QString error)> done);
};

} // namespace openmarketterminal::auth
