// src/app/WindowFrame_Auth.cpp
//
// Auth-flow handlers: show_login/register/forgot_password/pricing/info_*,
// the lock screen lifecycle, and on_auth_state_changed which routes the
// shell into / out of the auth stack as AuthManager state changes.
//
// Part of the partial-class split of WindowFrame.cpp.

#include "app/WindowFrame.h"

#include "app/DockScreenRouter.h"
#include "auth/AuthManager.h"
#include "auth/InactivityGuard.h"
#include "auth/PinManager.h"
#include "core/layout/WorkspaceShell.h"
#include "core/logging/Logger.h"
#include "core/session/SessionManager.h"
#include "screens/auth/LockScreen.h"
#include "storage/repositories/SettingsRepository.h"
#include "trading/instruments/InstrumentService.h"
#include "trading/instruments/SymbolResolver.h"
#include "ui/navigation/DockStatusBar.h"
#include "ui/navigation/DockToolBar.h"
#include "ui/theme/Theme.h"

#include <DockManager.h>

#include <QApplication>
#include <QMessageBox>
#include <QStackedWidget>

namespace openmarketterminal {

void WindowFrame::on_auth_state_changed() {
    auto& auth = auth::AuthManager::instance();

    if (guest_mode_) {
        LOG_DEBUG("WindowFrame", "on_auth_state_changed: guest mode active — keeping shell open");
        return;
    }

    // If logged out while the lock screen is active (e.g. max PIN attempts → reauth),
    // clear locked_ so the login screen can show. Otherwise stay on the lock screen.
    if (locked_) {
        if (!auth.is_authenticated()) {
            locked_ = false; // fall through to show login screen
        } else {
            return; // still authenticated — stay on lock screen
        }
    }

    // Still validating saved session — don't flash the login screen.
    // Stay on whatever is currently shown until validation completes.
    if (auth.is_loading())
        return;

    if (auth.is_authenticated()) {
        // Don't redirect if user is already on the app stack (dashboard/workspace
        // at index 1, or chat mode at index 2) — UNLESS the PIN gate hasn't been
        // cleared yet (auth completed while loading state showed dashboard early).
        //
        // pin_gate_cleared_ is critical: once the user has entered their PIN this
        // session, subsequent auth_state_changed events (periodic refresh_user_data,
        // focus-driven applicationStateChanged refresh, subscription_fetched, etc.)
        // must NOT re-lock the terminal. Without this guard, any auth refresh
        // while the user is working on the dashboard will re-show the PIN prompt.
        if (stack_->currentIndex() == 1 || stack_->currentIndex() == 2) {
            if (!pin_gate_cleared_) {
                if (auth.needs_pin_setup()) {
                    LOG_INFO("WindowFrame", "Authenticated (on app stack) but no PIN — showing PIN setup");
                    lock_screen_->show_setup();
                    locked_ = true;
                    set_shell_visible(false);
                    stack_->setCurrentIndex(3);
                    // Raise the process-wide locked flag so SessionGuard's
                    // pulse path and DockScreenRouter both know the user
                    // hasn't passed the PIN gate yet — without this, a
                    // backend 401 during the PIN-setup window can force a
                    // logout behind the lock screen.
                    auth::InactivityGuard::instance().set_terminal_locked(true);
                    return;
                }
                if (auth::PinManager::instance().has_pin()) {
                    LOG_INFO("WindowFrame", "Authenticated (on app stack) with PIN — showing PIN unlock");
                    lock_screen_->show_unlock();
                    locked_ = true;
                    set_shell_visible(false);
                    stack_->setCurrentIndex(3);
                    auth::InactivityGuard::instance().set_terminal_locked(true);
                    return;
                }
            }
            LOG_DEBUG("WindowFrame", QString("on_auth_state_changed: skipping redirect, "
                                            "stack index=%1, chat_mode=%2, gate_cleared=%3")
                                        .arg(stack_->currentIndex())
                                        .arg(chat_mode_)
                                        .arg(pin_gate_cleared_));
            return;
        }
        // User is viewing an Info screen (Help/Terms/Privacy/Contact/Trademarks)
        // inside the auth area — auth_stack_ index 1 is now the info stack
        // (the deleted Register/Forgot screens used to occupy indices 1/2,
        // shifting it down from 3). Don't
        // re-route or re-lock them on an auth-state refresh; they leave via the
        // screen's own Back action. (This slot used to special-case the removed
        // PricingScreen at this index — the PIN gate below does NOT check
        // pin_gate_cleared_, so dropping this early return would re-lock a user
        // who already passed the PIN and just opened an info page.)
        if (stack_->currentIndex() == 0 && auth_stack_->currentIndex() == 1)
            return;

        // ── PIN gate: require PIN setup or PIN unlock before proceeding ──
        // On first login (no PIN configured): show mandatory PIN setup.
        // On subsequent launches (PIN exists): show PIN unlock.
        // Skip if user is already on the lock screen (index 3).
        if (stack_->currentIndex() != 3) {
            if (auth.needs_pin_setup()) {
                LOG_INFO("WindowFrame", "Authenticated but no PIN — showing PIN setup");
                lock_screen_->show_setup();
                locked_ = true;
                set_shell_visible(false);
                stack_->setCurrentIndex(3);
                auth::InactivityGuard::instance().set_terminal_locked(true);
                return;
            }
            if (auth::PinManager::instance().has_pin()) {
                LOG_INFO("WindowFrame", "Authenticated with PIN — showing PIN unlock");
                lock_screen_->show_unlock();
                locked_ = true;
                set_shell_visible(false);
                stack_->setCurrentIndex(3);
                auth::InactivityGuard::instance().set_terminal_locked(true);
                return;
            }
        }

        // LOCAL-FIRST FORK: no plans, no paywall — every account gets full
        // access. (Was gated on session().has_paid_plan().)
        //
        // Defensive: at this point the PIN gate above must have either routed
        // us to the lock screen (and returned) or confirmed pin_gate_cleared_.
        // If we are about to show the shell while still locked or ungated, log
        // a warning so the regression is visible rather than leaking the
        // dashboard for one frame.
        if (locked_ || !pin_gate_cleared_) {
            LOG_WARN("WindowFrame",
                     QString("on_auth_state_changed: shell would become visible while "
                             "locked=%1 gate_cleared=%2 — forcing lock screen")
                         .arg(locked_).arg(pin_gate_cleared_));
            if (auth::PinManager::instance().has_pin())
                lock_screen_->show_unlock();
            else
                lock_screen_->show_setup();
            locked_ = true;
            set_shell_visible(false);
            stack_->setCurrentIndex(3);
            return;
        }

        set_shell_visible(true);
        stack_->setCurrentIndex(1);
        // Cold-boot restore: try last_loaded layout, then most recent
        // auto snapshot, then no-op (first run).
        layout::WorkspaceShell::load_last_or_default();
        // Warm instrument cache in background — only loaded if not already cached.
        auto& isvc = openmarketterminal::trading::InstrumentService::instance();
        for (const QString& bid : openmarketterminal::trading::SymbolResolver::instance().registered_brokers())
            isvc.load_from_db_async(bid);
    } else {
        // LOCAL-FIRST FORK: there is no OpenMarketTerminal account and no login screen to
        // show. Boot directly into the local shell (the PIN lock, if configured,
        // is the only gate). enter_local_shell() sets guest_mode_, so the early
        // return at the top of this function guards against re-entry once up.
        if (!guest_mode_)
            enter_local_shell();
    }
}

// Centralised transition into the auth stack. Any path that shows a login /
// register / forgot / pricing / info screen must go through here so the
// privileged shell (menus, command bar, CHAT, LOGOUT, status ticker) cannot
// be seen by an unauthenticated user. Title is also reset by set_shell_visible
// so the last-visited screen name does not leak.
void WindowFrame::enter_auth_stack(int auth_index) {
    set_shell_visible(false);
    stack_->setCurrentIndex(0);
    auth_stack_->setCurrentIndex(auth_index);
}

// LOCAL-FIRST FORK: the OpenMarketTerminal login / register / forgot-password / pricing
// screens are removed. show_login() (and the deleted register/forgot/pricing
// navigators it replaced) routes any caller into the local shell instead, so
// the dead auth stack can never be surfaced. The info stack is now at
// auth_stack_ index 1 (Register/Forgot used to occupy indices 1/2).
void WindowFrame::show_login() {
    enter_local_shell();
}
void WindowFrame::show_info_contact() {
    info_stack_->setCurrentIndex(0);
    enter_auth_stack(1);
}
void WindowFrame::show_info_terms() {
    info_stack_->setCurrentIndex(1);
    enter_auth_stack(1);
}
void WindowFrame::show_info_privacy() {
    info_stack_->setCurrentIndex(2);
    enter_auth_stack(1);
}
void WindowFrame::show_info_trademarks() {
    info_stack_->setCurrentIndex(3);
    enter_auth_stack(1);
}
void WindowFrame::show_info_help() {
    info_stack_->setCurrentIndex(4);
    enter_auth_stack(1);
}

void WindowFrame::continue_as_guest() {
    LOG_INFO("WindowFrame", QString("Continuing as local guest (window %1)").arg(window_id_));
    guest_mode_ = true;
    locked_ = false;
    pin_gate_cleared_ = true;

    auto& guard = auth::InactivityGuard::instance();
    guard.set_enabled(false);
    guard.set_terminal_locked(false);

    if (dock_manager_ && dock_manager_->parentWidget())
        dock_manager_->parentWidget()->setEnabled(true);
    if (dock_toolbar_)    dock_toolbar_->setEnabled(true);
    if (dock_status_bar_) dock_status_bar_->setEnabled(true);

    set_shell_visible(true);
    stack_->setCurrentIndex(1);
    if (dock_router_)
        dock_router_->navigate(QStringLiteral("dashboard"));
}

void WindowFrame::enter_local_shell() {
    guest_mode_ = true;

    // PIN gate (the only gate in local-first mode): if a local PIN is configured
    // and not yet cleared this session, require unlock before revealing the shell.
    if (!pin_gate_cleared_ && auth::PinManager::instance().has_pin()) {
        LOG_INFO("WindowFrame", QString("Local boot — PIN unlock required (window %1)").arg(window_id_));
        locked_ = true;
        lock_screen_->show_unlock();
        set_shell_visible(false);
        stack_->setCurrentIndex(3);
        auth::InactivityGuard::instance().set_terminal_locked(true);
        return;
    }

    // No PIN configured (or already unlocked this session) → straight to dashboard.
    LOG_INFO("WindowFrame", QString("Local boot — entering dashboard (window %1)").arg(window_id_));
    locked_ = false;
    pin_gate_cleared_ = true;

    auto& guard = auth::InactivityGuard::instance();
    if (dock_manager_ && dock_manager_->parentWidget())
        dock_manager_->parentWidget()->setEnabled(true);
    if (dock_toolbar_)    dock_toolbar_->setEnabled(true);
    if (dock_status_bar_) dock_status_bar_->setEnabled(true);

    set_shell_visible(true);
    stack_->setCurrentIndex(1);
    if (dock_router_)
        dock_router_->navigate(QStringLiteral("dashboard"));
    layout::WorkspaceShell::load_last_or_default();

    // Enable inactivity auto-lock only when a PIN exists to unlock with; otherwise
    // a timeout would lock the user out of their own local terminal permanently.
    if (auth::PinManager::instance().has_pin()) {
        auto r = SettingsRepository::instance().get("security.lock_timeout_minutes");
        if (r.is_ok() && !r.value().isEmpty()) {
            const int minutes = r.value().toInt();
            if (minutes > 0)
                guard.set_timeout_minutes(minutes);
        }
        if (!guard.is_enabled()) {
            qApp->installEventFilter(&guard);
            guard.set_enabled(true);
        }
        guard.reset_timer();
    } else {
        guard.set_enabled(false);
    }
    guard.set_terminal_locked(false);
}

void WindowFrame::show_lock_screen() {
    // LOCAL-FIRST FORK: locking is gated on a configured local PIN, not on auth.
    if (!auth::PinManager::instance().has_pin()) {
        LOG_DEBUG("WindowFrame", "show_lock_screen: ignored — no local PIN set");
        return;
    }

    // Raise the process-wide locked flag. Every WindowFrame listens for
    // terminal_locked_changed and applies the lock UI itself in
    // apply_lock_state() — including this one, idempotently. Stop the
    // inactivity guard so the activity stream on the lock screen does
    // not generate no-op ticks until the PIN is accepted.
    auth::InactivityGuard::instance().set_enabled(false);
    auth::InactivityGuard::instance().set_terminal_locked(true);
}

void WindowFrame::apply_lock_state(bool locked) {
    if (locked) {
        // Idempotent — if already on the lock screen, nothing to do.
        if (stack_->currentIndex() == 3)
            return;
        // Skip if this window is currently on the auth stack — login screen
        // is its own gate. Spurious lock during auth transition would push
        // a lock screen over the login screen which is meaningless.
        if (stack_->currentIndex() == 0) {
            LOG_DEBUG("WindowFrame", "apply_lock_state(true): on auth stack — skipping");
            return;
        }
        LOG_INFO("WindowFrame", QString("Locking window %1").arg(window_id_));
        lock_screen_->activate();
        locked_ = true;
        pin_gate_cleared_ = false;
        set_shell_visible(false);
        stack_->setCurrentIndex(3);
        if (chat_bubble_)
            chat_bubble_->setVisible(false);

        // Disable widgets behind the lock screen so keyboard shortcuts,
        // focus traversal, and dock-manager hit-testing cannot mutate state.
        if (dock_manager_ && dock_manager_->parentWidget())
            dock_manager_->parentWidget()->setEnabled(false);
        if (dock_toolbar_)    dock_toolbar_->setEnabled(false);
        if (dock_status_bar_) dock_status_bar_->setEnabled(false);
        return;
    }

    // Unlock — only the window the user typed the PIN into emits the
    // unlocked signal that lands in on_terminal_unlocked(). Secondary
    // windows take this path via terminal_locked_changed(false). For
    // them we just restore the dashboard chrome; PIN gate state is
    // per-window and stays cleared after the originator set it.
    if (stack_->currentIndex() != 3)
        return; // already unlocked
    LOG_INFO("WindowFrame", QString("Unlocking window %1 (sibling)").arg(window_id_));
    locked_ = false;
    pin_gate_cleared_ = true;
    if (dock_manager_ && dock_manager_->parentWidget())
        dock_manager_->parentWidget()->setEnabled(true);
    if (dock_toolbar_)    dock_toolbar_->setEnabled(true);
    if (dock_status_bar_) dock_status_bar_->setEnabled(true);
    set_shell_visible(true);
    stack_->setCurrentIndex(1);
}

void WindowFrame::on_terminal_unlocked() {
    auto& auth = auth::AuthManager::instance();
    LOG_INFO("WindowFrame", QString("Terminal unlocked via PIN (window %1)").arg(window_id_));
    locked_ = false;
    pin_gate_cleared_ = true;

    // Re-enable this window's widgets. Sibling MainWindows are handled by
    // apply_lock_state(false) once we flip the flag at the bottom of this
    // function — keep the flag-flip last so siblings don't race ahead and
    // restore their own UI before this originator is fully ready.
    if (dock_manager_ && dock_manager_->parentWidget())
        dock_manager_->parentWidget()->setEnabled(true);
    if (dock_toolbar_)    dock_toolbar_->setEnabled(true);
    if (dock_status_bar_) dock_status_bar_->setEnabled(true);

    // Enable/restart inactivity guard. It is disabled in show_lock_screen()
    // and on logout, so it may currently be off even though the filter is
    // already installed on qApp from a previous unlock. Re-read the saved
    // timeout from settings so a value changed in Settings → Security takes
    // effect on the very next lock cycle even if the guard had been stopped
    // (e.g. across a lock/unlock or logout/login round-trip).
    auto& guard = auth::InactivityGuard::instance();
    {
        auto r = SettingsRepository::instance().get("security.lock_timeout_minutes");
        if (r.is_ok() && !r.value().isEmpty()) {
            int minutes = r.value().toInt();
            if (minutes > 0)
                guard.set_timeout_minutes(minutes);
        }
    }
    if (!guard.is_enabled()) {
        qApp->installEventFilter(&guard);
        guard.set_enabled(true);
    }
    guard.reset_timer();

    // Reset PIN lockout on successful unlock
    auth::PinManager::instance().reset_lockout();

    // LOCAL-FIRST FORK: no plans / no pricing gate — after a successful PIN unlock
    // always restore the local dashboard.
    guest_mode_ = true;
    set_shell_visible(true);
    stack_->setCurrentIndex(1);
    // Restore chat bubble based on setting
    if (chat_bubble_) {
        auto r = SettingsRepository::instance().get("appearance.show_chat_bubble");
        bool show = !r.is_ok() || r.value() != "false";
        chat_bubble_->setVisible(show);
        if (show) {
            chat_bubble_->reposition();
            chat_bubble_->raise();
        }
    }
    // Cold-boot restore via the new system (frame layouts, panels, dock
    // state, monitor variants).
    layout::WorkspaceShell::load_last_or_default();

    // Flip the process-wide locked flag LAST. Sibling MainWindows fan out
    // via terminal_locked_changed → apply_lock_state(false) and restore
    // their own dashboard chrome from this single source of truth.
    auth::InactivityGuard::instance().set_terminal_locked(false);

    emit auth.terminal_unlocked();
}

} // namespace openmarketterminal
