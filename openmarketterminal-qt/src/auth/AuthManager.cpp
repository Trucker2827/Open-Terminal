#include "auth/AuthManager.h"

#include "auth/PinManager.h"
#include "core/logging/Logger.h"
#include "network/http/HttpClient.h"
#include "storage/repositories/LlmConfigRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/secure/SecureStorage.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSysInfo>

namespace openmarketterminal::auth {

AuthManager& AuthManager::instance() {
    static AuthManager s;
    return s;
}

AuthManager::AuthManager() {}

void AuthManager::set_loading(bool v) {
    if (is_loading_ != v) {
        is_loading_ = v;
        emit loading_changed(v);
    }
}

QString AuthManager::generate_device_id() const {
    QString info =
        QSysInfo::machineHostName() + "|" + QSysInfo::productType() + "|" + QSysInfo::currentCpuArchitecture();
    QByteArray hash = QCryptographicHash::hash(info.toUtf8(), QCryptographicHash::Sha256).toHex();
    QString timestamp = QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
    quint32 random = QRandomGenerator::global()->generate();
    return QString("openmarketterminal_desktop_%1_%2_%3")
        .arg(QString::fromUtf8(hash.left(16)), timestamp, QString::number(random, 36));
}

// ── Token helper — single place to set/clear tokens on HttpClient ────────────

static void apply_tokens(const QString& api_key, const QString& session_token) {
    auto& http = openmarketterminal::HttpClient::instance();
    http.set_auth_header(api_key);
    http.set_session_token(session_token);
}

static void clear_tokens() {
    apply_tokens({}, {});
}

// ── Session persistence (SQLite via SettingsRepository) ──────────────────────

void AuthManager::save_session() {
    // CR-08: persist ONLY non-secret session fields to the unencrypted settings
    // table. The api_key and session_token are stored exclusively in
    // SecureStorage (AES-256-GCM) so a stolen openmarketterminal.db can't yield credentials.
    QJsonDocument doc(session_.to_persisted_json());
    QString json = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    auto r = openmarketterminal::SettingsRepository::instance().set("openmarketterminal_session", json, "auth");
    if (r.is_err()) {
        LOG_ERROR("Auth", "Failed to save session: " + QString::fromStdString(r.error()));
    }

    // Secrets → OS-native encrypted storage (DPAPI / Keychain) only.
    if (!session_.api_key.isEmpty()) {
        auto sr = openmarketterminal::SecureStorage::instance().store("api_key", session_.api_key);
        if (sr.is_err())
            LOG_ERROR("Auth", "SecureStorage: failed to persist api_key — credential not saved");
    } else {
        openmarketterminal::SecureStorage::instance().remove("api_key");
    }

    if (!session_.session_token.isEmpty()) {
        auto st = openmarketterminal::SecureStorage::instance().store("session_token", session_.session_token);
        if (st.is_err())
            LOG_ERROR("Auth", "SecureStorage: failed to persist session_token");
    } else {
        openmarketterminal::SecureStorage::instance().remove("session_token");
    }
}

void AuthManager::load_session() {
    auto r = openmarketterminal::SettingsRepository::instance().get("openmarketterminal_session");
    if (r.is_ok() && !r.value().isEmpty()) {
        auto doc = QJsonDocument::fromJson(r.value().toUtf8());
        if (!doc.isNull())
            session_ = SessionData::from_json(doc.object());
    }

    // CR-08 one-shot migration: existing installs have api_key + session_token
    // written in clear inside the legacy "openmarketterminal_session" blob (and a separate
    // plaintext "openmarketterminal_api_key" row). If the loaded session still carries
    // secrets, move them into SecureStorage and purge the plaintext copies.
    migrate_legacy_plaintext_credentials();

    // Recover secrets from SecureStorage (DPAPI / Keychain) — the only durable,
    // encrypted source. Survives SQLite corruption and DB migrations.
    auto secure_key = openmarketterminal::SecureStorage::instance().retrieve("api_key");
    if (secure_key.is_ok() && !secure_key.value().isEmpty()) {
        if (session_.api_key.isEmpty() || session_.api_key != secure_key.value()) {
            LOG_INFO("Auth", "Restored api_key from SecureStorage");
            session_.api_key = secure_key.value();
        }
    }

    auto secure_token = openmarketterminal::SecureStorage::instance().retrieve("session_token");
    if (secure_token.is_ok() && !secure_token.value().isEmpty()) {
        session_.session_token = secure_token.value();
    }

    // Never trust saved authenticated flag — must be re-validated
    session_.authenticated = false;
}

void AuthManager::migrate_legacy_plaintext_credentials() {
    auto& settings = openmarketterminal::SettingsRepository::instance();
    auto& secure = openmarketterminal::SecureStorage::instance();
    bool migrated = false;

    // 1. Secrets that came in via the legacy plaintext "openmarketterminal_session" blob.
    if (!session_.api_key.isEmpty()) {
        secure.store("api_key", session_.api_key);
        migrated = true;
    }
    if (!session_.session_token.isEmpty()) {
        secure.store("session_token", session_.session_token);
        migrated = true;
    }

    // 2. The standalone plaintext "openmarketterminal_api_key" row written by older builds.
    auto legacy_key = settings.get("openmarketterminal_api_key");
    if (legacy_key.is_ok() && !legacy_key.value().isEmpty()) {
        if (session_.api_key.isEmpty())
            session_.api_key = legacy_key.value();
        secure.store("api_key", legacy_key.value());
        migrated = true;
    }

    if (migrated) {
        // Rewrite openmarketterminal_session without secrets and drop the plaintext key row
        // so the cleartext copies no longer exist on disk for this install.
        QJsonDocument doc(session_.to_persisted_json());
        settings.set("openmarketterminal_session",
                     QString::fromUtf8(doc.toJson(QJsonDocument::Compact)), "auth");
        settings.remove("openmarketterminal_api_key");
        LOG_INFO("Auth", "Migrated legacy plaintext credentials into SecureStorage and purged settings rows");
    }
}

QString AuthManager::openmarketterminal_api_key() const {
    // Single supported resolver. Prefer the live in-memory session; fall back to
    // the encrypted SecureStorage copy. Never reads the legacy plaintext row.
    if (!session_.api_key.isEmpty())
        return session_.api_key;
    auto secure_key = openmarketterminal::SecureStorage::instance().retrieve("api_key");
    if (secure_key.is_ok())
        return secure_key.value();
    return {};
}

void AuthManager::clear_session() {
    session_ = SessionData{};
    clear_tokens();
    openmarketterminal::SettingsRepository::instance().remove("openmarketterminal_session");
    openmarketterminal::SettingsRepository::instance().remove("openmarketterminal_api_key");
    openmarketterminal::SecureStorage::instance().remove("api_key");
    openmarketterminal::SecureStorage::instance().remove("session_token");

    // PIN intentionally NOT cleared here. The PIN is a local-device credential,
    // independent of the backend session. Wiping it on every logout means a
    // transient session expiry (SessionGuard 401 path, explicit Logout) trains
    // the user to keep choosing fresh PINs and destroys the audit trail across
    // sessions. The only path that should reset the PIN is the max-attempts
    // re-auth flow (LockScreen → reauth_requested), and that path should call
    // PinManager::clear_pin() explicitly before invoking logout().

    // Clear auto-configured openmarketterminal LLM provider and reset LlmService
    LlmConfigRepository::instance().delete_provider("openmarketterminal");
}

bool AuthManager::needs_pin_setup() const {
    return session_.authenticated && !PinManager::instance().has_pin();
}

// ── Initialize ───────────────────────────────────────────────────────────────

void AuthManager::initialize() {
    // LOCAL-FIRST FORK: no OpenMarketTerminal account. Never load a saved session or contact
    // the server — the terminal runs unauthenticated / local-only. This removes the
    // only boot-time auth network call and disables session-pulse and profile sync
    // (both require an authenticated session). Any previously-saved encrypted
    // session row is simply ignored, never loaded or validated.
    session_ = {};
    set_loading(false);
    emit auth_state_changed();
}

// ── Logout (local only) ──────────────────────────────────────────────────────

void AuthManager::logout() {
    // LOCAL-FIRST FORK: the remote-account backend was removed, so there is no
    // server session to invalidate. logout() now only clears the local /
    // persisted session and notifies listeners. Retained because the
    // lock-screen max-attempts reauth path and the toolbar / profile "log out"
    // actions still call it.
    if (is_logging_out_)
        return;
    is_logging_out_ = true;

    clear_session();
    is_logging_out_ = false;

    emit logged_out();
    emit auth_state_changed();
}

// ── Auto-configure OpenMarketTerminal LLM provider ──────────────────────────────────────

void AuthManager::auto_configure_openmarketterminal_llm() {
    // LOCAL-FIRST FORK: the hosted OpenMarketTerminal LLM proxy is removed. This no longer
    // creates or activates a "openmarketterminal" provider. It now only defensively purges
    // any legacy plaintext key row and any stale "openmarketterminal" provider config left by
    // a prior build, so the LLM layer can never select the OpenMarketTerminal proxy. The
    // assistant defaults to local Ollama (see LlmService::ensure_config).
    openmarketterminal::SettingsRepository::instance().remove("openmarketterminal_api_key");
    LlmConfigRepository::instance().delete_provider("openmarketterminal");
}

} // namespace openmarketterminal::auth
