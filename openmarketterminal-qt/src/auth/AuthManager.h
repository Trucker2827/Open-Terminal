#pragma once
#include "auth/AuthTypes.h"

#include <QObject>

#include <functional>

namespace openmarketterminal::auth {

/// Central auth state machine.
/// Manages login/signup/MFA flows, session persistence, and validation.
class AuthManager : public QObject {
    Q_OBJECT
  public:
    static AuthManager& instance();

    const SessionData& session() const { return session_; }
    bool is_authenticated() const { return session_.authenticated; }
    bool is_loading() const { return is_loading_; }

    /// Resolve the OpenMarketTerminal api_key for LLM/service callers WITHOUT touching the
    /// plaintext SQLite settings table. Prefers the live in-memory session;
    /// falls back to the encrypted SecureStorage copy (which load_session also
    /// restores into the session at startup). Returns empty if no key is known.
    /// This is the single supported resolver — callers must NOT read the legacy
    /// plaintext "openmarketterminal_api_key" settings row.
    QString openmarketterminal_api_key() const;

    // Local logout: clears the in-memory/persisted session and emits
    // logged_out + auth_state_changed. No remote call (the remote-account
    // backend was removed). Retained for the lock-screen reauth path and the
    // toolbar / profile "log out" actions.
    void logout();

    // Session management
    void initialize();

    /// True if user needs to set up a PIN (authenticated but no PIN configured).
    bool needs_pin_setup() const;

  signals:
    void auth_state_changed();
    void logged_out();
    void loading_changed(bool loading);

    /// Emitted after successful login when PIN setup is needed.
    void pin_setup_required();

    /// Emitted after login + PIN verify (or setup) — terminal is fully unlocked.
    void terminal_unlocked();

  private:
    AuthManager();

    void set_loading(bool v);
    void save_session();
    void load_session();
    void clear_session();
    // One-shot: move secrets out of the legacy plaintext settings rows into
    // SecureStorage, then purge the cleartext copies (CR-08). Idempotent.
    void migrate_legacy_plaintext_credentials();
    void auto_configure_openmarketterminal_llm();
    QString generate_device_id() const;

    SessionData session_;
    bool is_loading_ = true;
    bool is_logging_out_ = false;
};

} // namespace openmarketterminal::auth
