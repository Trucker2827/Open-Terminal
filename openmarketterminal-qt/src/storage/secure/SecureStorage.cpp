#include "storage/secure/SecureStorage.h"

#include "core/logging/Logger.h"
#include "storage/secure/KeychainKey.h"
#include "storage/sqlite/Database.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QSysInfo>

#include <openssl/err.h>
#include <openssl/evp.h>

// ── SQLite-backed credential store ───────────────────────────────────────────
//
// Values live in `secure_credentials` (created by migration v025) encrypted
// with AES-256-GCM. The 256-bit key is derived once per process from a
// machine-local seed and cached in static storage; rotating the seed (e.g.
// after copying the profile to another machine) makes the existing rows
// undecryptable, which is the desired behaviour.
//
// Crypto is OpenSSL EVP — already linked transitively for Google Sheets JWT
// signing, so no new dependency. All EVP allocations are stack-bounded and
// freed on every exit path; no heap thrash on the hot path.

static constexpr auto TAG = "SecureStorage";

namespace openmarketterminal {
namespace {

constexpr int kKeyLen = 32;     // AES-256
constexpr int kIvLen = 12;      // GCM-recommended nonce size (RFC 5116 §5)
constexpr int kTagLen = 16;     // GCM authentication tag (128 bits)

// Stable per-app salt mixed into the machine-ID hash so a different app
// on the same machine derives a different key. Bumping this rotates
// every key — only do that with a fresh DB.
constexpr const char* kAppSalt = "openmarketterminal/secure-storage/v1";

// Legacy machine-derived key: SHA-256(machineUniqueId + productType + appSalt).
// Reconstructible from public inputs, so it is the WEAK tier — retained only as
// (a) the permanent decrypt fallback for rows written before the keychain key
// existed, and (b) the active key on platforms without a keystore backend yet.
const QByteArray& legacy_key() {
    static QMutex m;
    static QByteArray cached;
    QMutexLocker lock(&m);
    if (!cached.isEmpty())
        return cached;

    QByteArray seed;
    seed.append(QSysInfo::machineUniqueId());
    seed.append('\x1f');
    seed.append(QSysInfo::productType().toUtf8());
    seed.append('\x1f');
    seed.append(kAppSalt);

    cached = QCryptographicHash::hash(seed, QCryptographicHash::Sha256);
    Q_ASSERT(cached.size() == kKeyLen);
    return cached;
}

// Active master-key state, resolved once by SecureStorage::init().
QMutex g_key_mutex;
QByteArray g_active_key;           // key used to ENCRYPT new rows
bool g_active_is_keychain = false; // true → g_active_key came from the OS keychain
bool g_initialized = false;

// The key new ciphertext is encrypted under. Falls back to the legacy key when
// init() has not run yet (e.g. a credential touched before startup wiring), so
// a read/write never blocks; init() upgrades it to the keychain key.
QByteArray active_key() {
    QMutexLocker lock(&g_key_mutex);
    if (!g_active_key.isEmpty())
        return g_active_key;
    return legacy_key();
}

// True when the active key is the keychain key and differs from the legacy key,
// i.e. legacy-encrypted rows may exist and need lazy migration on read.
bool migration_enabled() {
    QMutexLocker lock(&g_key_mutex);
    return g_initialized && g_active_is_keychain;
}

QByteArray random_iv() {
    QByteArray iv(kIvLen, '\0');
    QRandomGenerator* rng = QRandomGenerator::system();
    for (int i = 0; i < kIvLen; ++i)
        iv[i] = static_cast<char>(rng->bounded(256));
    return iv;
}

// AES-256-GCM encrypt. Returns ciphertext + the GCM tag through `tag_out`.
// On any OpenSSL error returns an empty QByteArray and logs — callers must
// check both isEmpty() AND tag_out.size() == kTagLen before persisting.
QByteArray aes_gcm_encrypt(const QByteArray& key, const QByteArray& iv,
                           const QByteArray& plaintext, QByteArray& tag_out) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};

    QByteArray out;
    bool ok = false;
    do {
        if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr))
            break;
        if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr))
            break;
        if (1 != EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                                    reinterpret_cast<const unsigned char*>(key.constData()),
                                    reinterpret_cast<const unsigned char*>(iv.constData())))
            break;

        // Reserve plaintext.size() + block size; AES has 16-byte blocks but
        // GCM is a stream cipher (CTR-mode under the hood) so output length
        // equals input length. The +16 margin keeps us safe if EVP ever
        // reports cipher block size for compatibility reasons.
        out.resize(plaintext.size() + 16);
        int len = 0;
        if (1 != EVP_EncryptUpdate(ctx,
                                   reinterpret_cast<unsigned char*>(out.data()), &len,
                                   reinterpret_cast<const unsigned char*>(plaintext.constData()),
                                   plaintext.size()))
            break;
        int written = len;

        if (1 != EVP_EncryptFinal_ex(ctx,
                                     reinterpret_cast<unsigned char*>(out.data()) + written,
                                     &len))
            break;
        written += len;
        out.resize(written);

        tag_out.resize(kTagLen);
        if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag_out.data()))
            break;

        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        LOG_ERROR(TAG, QString("AES-GCM encrypt failed: OpenSSL err 0x%1")
                           .arg(ERR_get_error(), 0, 16));
        return {};
    }
    return out;
}

// AES-256-GCM decrypt with tag verification. Returns plaintext on success or
// an empty QByteArray if the tag doesn't verify (tampered ciphertext, wrong
// machine, corrupted DB row). Callers must NOT use the returned bytes if
// `ok_out` is false — an empty QByteArray is also a valid plaintext.
QByteArray aes_gcm_decrypt(const QByteArray& key, const QByteArray& iv,
                           const QByteArray& ciphertext, const QByteArray& tag,
                           bool& ok_out) {
    ok_out = false;
    if (iv.size() != kIvLen || tag.size() != kTagLen)
        return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};

    QByteArray out;
    do {
        if (1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr))
            break;
        if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr))
            break;
        if (1 != EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                    reinterpret_cast<const unsigned char*>(key.constData()),
                                    reinterpret_cast<const unsigned char*>(iv.constData())))
            break;

        out.resize(ciphertext.size() + 16);
        int len = 0;
        if (1 != EVP_DecryptUpdate(ctx,
                                   reinterpret_cast<unsigned char*>(out.data()), &len,
                                   reinterpret_cast<const unsigned char*>(ciphertext.constData()),
                                   ciphertext.size()))
            break;
        int written = len;

        // EVP_CTRL_GCM_SET_TAG must be called before EVP_DecryptFinal_ex —
        // Final is what actually verifies the tag and returns 0 on mismatch.
        if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen,
                                     const_cast<char*>(tag.constData())))
            break;

        if (1 != EVP_DecryptFinal_ex(ctx,
                                     reinterpret_cast<unsigned char*>(out.data()) + written,
                                     &len))
            break;  // tag mismatch — leave ok_out=false
        written += len;
        out.resize(written);
        ok_out = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return out;
}

} // anonymous namespace

SecureStorage& SecureStorage::instance() {
    static SecureStorage s;
    return s;
}

void SecureStorage::init() {
    QMutexLocker lock(&g_key_mutex);
    if (g_initialized)
        return;
    // Touch the OS keychain here (main thread, startup). keychain_master_key()
    // creates + round-trip-verifies a random key on first run, or returns empty
    // on failure / unsupported platform — in which case we keep the legacy
    // machine-derived key so behaviour is unchanged and no data is stranded.
    QByteArray kc = secure_detail::keychain_master_key(kKeyLen);
    if (kc.size() == kKeyLen) {
        g_active_key = kc;
        g_active_is_keychain = true;
        LOG_INFO(TAG, "SecureStorage master key sourced from the OS keychain");
    } else {
        g_active_key = legacy_key();
        g_active_is_keychain = false;
        LOG_INFO(TAG, "SecureStorage using machine-derived master key (OS keychain backend unavailable)");
    }
    g_initialized = true;
}

Result<void> SecureStorage::store(const QString& key, const QString& value) {
    auto& db = Database::instance();
    if (!db.is_open()) {
        LOG_ERROR(TAG, "SecureStorage::store called before Database::open()");
        return Result<void>::err("Database not open");
    }

    const QByteArray plaintext = value.toUtf8();
    const QByteArray iv = random_iv();
    QByteArray tag;
    const QByteArray ciphertext = aes_gcm_encrypt(active_key(), iv, plaintext, tag);
    if (tag.size() != kTagLen) {
        // aes_gcm_encrypt already logged the OpenSSL error.
        return Result<void>::err("Encryption failed");
    }

    QSqlQuery q(db.connection()); // per-thread connection (see retrieve) — thread-safe
    q.prepare(R"sql(
        INSERT INTO secure_credentials (key, ciphertext, iv, tag, updated_at)
        VALUES (?, ?, ?, ?, datetime('now'))
        ON CONFLICT(key) DO UPDATE SET
            ciphertext = excluded.ciphertext,
            iv         = excluded.iv,
            tag        = excluded.tag,
            updated_at = excluded.updated_at
    )sql");
    q.addBindValue(key);
    q.addBindValue(ciphertext);
    q.addBindValue(iv);
    q.addBindValue(tag);
    if (!q.exec()) {
        LOG_ERROR(TAG, QString("SQL upsert failed for key %1: %2")
                           .arg(key, q.lastError().text()));
        return Result<void>::err("Failed to write credential row");
    }
    return Result<void>::ok();
}

Result<QString> SecureStorage::retrieve(const QString& key) {
    auto& db = Database::instance();
    if (!db.is_open())
        return Result<QString>::err("Database not open");

    // Use the per-thread connection, NOT the shared raw_db(): credential reads run
    // on worker threads (algo quote polling, candle warm-up, order placement), and
    // sharing one SQLite handle across threads corrupts the parser → SIGSEGV.
    QSqlQuery q(db.connection());
    q.prepare("SELECT ciphertext, iv, tag FROM secure_credentials WHERE key = ?");
    q.addBindValue(key);
    if (!q.exec()) {
        LOG_ERROR(TAG, QString("SQL select failed for key %1: %2")
                           .arg(key, q.lastError().text()));
        return Result<QString>::err("Read failed");
    }
    if (!q.next())
        return Result<QString>::err("Not found");

    const QByteArray ciphertext = q.value(0).toByteArray();
    const QByteArray iv = q.value(1).toByteArray();
    const QByteArray tag = q.value(2).toByteArray();
    q.finish();  // release the SELECT before any same-connection write below

    bool ok = false;
    QByteArray plaintext = aes_gcm_decrypt(active_key(), iv, ciphertext, tag, ok);

    if (!ok && migration_enabled()) {
        // The row may have been written under the legacy machine-derived key
        // before the keychain key existed. Try it; on success, transparently
        // re-encrypt under the active (keychain) key. Best-effort: a re-encrypt
        // failure must NOT fail the read, and the row is overwritten in place
        // (upsert), never deleted — a transient write error just retries next read.
        bool legacy_ok = false;
        QByteArray legacy_pt = aes_gcm_decrypt(legacy_key(), iv, ciphertext, tag, legacy_ok);
        if (legacy_ok) {
            ok = true;
            plaintext = legacy_pt;
            if (store(key, QString::fromUtf8(plaintext)).is_ok())
                LOG_INFO(TAG, QString("Migrated credential '%1' to the keychain-backed key").arg(key));
            else
                LOG_WARN(TAG, QString("Lazy key migration for '%1' failed — will retry next read").arg(key));
        }
    }

    if (!ok) {
        // Tag mismatch under every key we tried — corrupted/tampered row, a
        // profile copied from another machine, or a keychain reset. Surface as a
        // plain "not found" so the caller's recovery path (re-prompt for PIN /
        // re-enter credentials) kicks in cleanly. The row is left INTACT — never
        // deleted on a decrypt failure. Logged WARN for forensic visibility.
        LOG_WARN(TAG, QString("AES-GCM tag mismatch for key %1 — row unreadable").arg(key));
        return Result<QString>::err("Not found");
    }
    return Result<QString>::ok(QString::fromUtf8(plaintext));
}

Result<void> SecureStorage::remove(const QString& key) {
    auto& db = Database::instance();
    if (!db.is_open())
        return Result<void>::err("Database not open");

    QSqlQuery q(db.connection()); // per-thread connection (see retrieve) — thread-safe
    q.prepare("DELETE FROM secure_credentials WHERE key = ?");
    q.addBindValue(key);
    if (!q.exec()) {
        LOG_ERROR(TAG, QString("SQL delete failed for key %1: %2")
                           .arg(key, q.lastError().text()));
        return Result<void>::err("Delete failed");
    }
    return Result<void>::ok();
}

} // namespace openmarketterminal
