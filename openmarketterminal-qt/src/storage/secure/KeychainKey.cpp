#include "storage/secure/KeychainKey.h"

#include "core/logging/Logger.h"

#include <QtGlobal>

// Isolated TU (excluded from unity build — see CMakeLists.txt) so the platform
// keystore headers below don't pollute batched unity sources.
#if defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace openmarketterminal::secure_detail {

namespace {
constexpr auto TAG = "Keychain";
// Service/account identifiers for the keychain item. Bumping the account
// suffix would orphan the existing key (forcing credential re-entry), so it is
// versioned and stable.
constexpr const char* kService = "org.openterminal.OpenTerminal.securestorage";
constexpr const char* kAccount = "aes-master-key-v1";
}  // namespace

#if defined(Q_OS_MACOS)

namespace {

// RAII for CoreFoundation objects so every early-return path releases cleanly.
template <typename T>
struct CFGuard {
    T ref = nullptr;
    CFGuard() = default;
    explicit CFGuard(T r) : ref(r) {}
    ~CFGuard() {
        if (ref)
            CFRelease(ref);
    }
    CFGuard(const CFGuard&) = delete;
    CFGuard& operator=(const CFGuard&) = delete;
    operator T() const { return ref; }
};

CFStringRef cf(const char* s) {
    return CFStringCreateWithCString(nullptr, s, kCFStringEncodingUTF8);
}

// Read the stored key bytes. Returns empty if absent or on error.
QByteArray read_key() {
    CFGuard<CFStringRef> svc(cf(kService));
    CFGuard<CFStringRef> acct(cf(kAccount));
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit};
    const void* vals[] = {kSecClassGenericPassword, svc.ref, acct.ref, kCFBooleanTrue, kSecMatchLimitOne};
    CFGuard<CFDictionaryRef> query(
        CFDictionaryCreate(nullptr, keys, vals, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    CFTypeRef result = nullptr;
    OSStatus st = SecItemCopyMatching(query, &result);
    CFGuard<CFTypeRef> result_guard(result);
    if (st != errSecSuccess || !result)
        return {};
    CFDataRef data = static_cast<CFDataRef>(result);
    return QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                      static_cast<int>(CFDataGetLength(data)));
}

// Add or update the stored key. Returns true on success.
bool write_key(const QByteArray& key) {
    CFGuard<CFStringRef> svc(cf(kService));
    CFGuard<CFStringRef> acct(cf(kAccount));
    CFGuard<CFDataRef> data(
        CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(key.constData()), key.size()));

    const void* akeys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecAttrAccessible, kSecValueData};
    const void* avals[] = {kSecClassGenericPassword, svc.ref, acct.ref,
                           kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly, data.ref};
    CFGuard<CFDictionaryRef> add(
        CFDictionaryCreate(nullptr, akeys, avals, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    OSStatus st = SecItemAdd(add, nullptr);

    if (st == errSecDuplicateItem) {
        const void* qkeys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
        const void* qvals[] = {kSecClassGenericPassword, svc.ref, acct.ref};
        CFGuard<CFDictionaryRef> q(
            CFDictionaryCreate(nullptr, qkeys, qvals, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        const void* ukeys[] = {kSecValueData};
        const void* uvals[] = {data.ref};
        CFGuard<CFDictionaryRef> upd(
            CFDictionaryCreate(nullptr, ukeys, uvals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        st = SecItemUpdate(q, upd);
    }
    return st == errSecSuccess;
}

}  // namespace

QByteArray keychain_master_key(int key_len) {
    // Existing key wins (must be the right length).
    QByteArray existing = read_key();
    if (existing.size() == key_len)
        return existing;
    if (!existing.isEmpty()) {
        LOG_WARN(TAG, "Keychain master key present but wrong size — ignoring (falling back to legacy key)");
        return {};
    }

    // Generate a fresh cryptographically-random key.
    QByteArray fresh(key_len, '\0');
    if (SecRandomCopyBytes(kSecRandomDefault, key_len, reinterpret_cast<uint8_t*>(fresh.data())) != errSecSuccess) {
        LOG_ERROR(TAG, "SecRandomCopyBytes failed — cannot create keychain master key");
        return {};
    }
    if (!write_key(fresh)) {
        LOG_ERROR(TAG, "Failed to store master key in keychain — falling back to legacy key");
        return {};
    }
    // SAFETY: prove it round-trips before the caller encrypts anything with it.
    // If the keystore silently dropped the write, returning empty keeps us on
    // the legacy key instead of orphaning future credentials.
    if (read_key() != fresh) {
        LOG_ERROR(TAG, "Keychain master key did not round-trip — falling back to legacy key");
        return {};
    }
    LOG_INFO(TAG, "Created new keychain-backed SecureStorage master key");
    return fresh;
}

#else  // !Q_OS_MACOS

QByteArray keychain_master_key(int /*key_len*/) {
    // TODO: Windows (DPAPI / Credential Manager) and Linux (libsecret) backends.
    // Until then, return empty so SecureStorage keeps its existing
    // machine-derived key — no behaviour change on these platforms.
    return {};
}

#endif

}  // namespace openmarketterminal::secure_detail
