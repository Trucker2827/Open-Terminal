// ProfileTools.cpp — Profile tab MCP tools (14 tools)
// Covers: profile, session, security, usage, billing, notifications.

#include "mcp/tools/ProfileTools.h"

#include "auth/AuthManager.h"
#include "auth/AuthTypes.h"
#include "services/notifications/NotificationService.h"

#include <QJsonArray>
#include <QJsonObject>

namespace openmarketterminal::mcp::tools {

using namespace openmarketterminal::auth;

// ── Tool registration ─────────────────────────────────────────────────────────
//
// LOCAL-FIRST FORK: the remote-account backend (UserApi) was removed, so the
// profile_get / profile_update / profile_regenerate_api_key /
// profile_get_login_history / profile_enable_mfa / profile_disable_mfa tools
// (all backed by UserApi network calls) are gone. The remaining tools read the
// local in-memory session and the local notification history only.

std::vector<ToolDef> get_profile_tools() {
    std::vector<ToolDef> tools;

    // ── profile_get_session ──────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "profile_get_session";
        t.description = "Get current session information: authentication state, username, "
                        "email, MFA status.";
        t.category = "profile";
        t.input_schema.properties = QJsonObject{};
        t.handler = [](const QJsonObject&) -> ToolResult {
            const auto& sess = AuthManager::instance().session();
            if (!sess.authenticated)
                return ToolResult::fail("Not authenticated");
            return ToolResult::ok_data(sess.to_json());
        };
        tools.push_back(std::move(t));
    }

    // ── profile_get_api_key ──────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "profile_get_api_key";
        t.description = "Get the current user's API key from the active session.";
        t.category = "profile";
        t.input_schema.properties = QJsonObject{};
        t.handler = [](const QJsonObject&) -> ToolResult {
            const auto& sess = AuthManager::instance().session();
            if (!sess.authenticated)
                return ToolResult::fail("Not authenticated");
            if (sess.api_key.isEmpty())
                return ToolResult::fail("No API key in session");
            return ToolResult::ok_data(QJsonObject{{"api_key", sess.api_key}});
        };
        tools.push_back(std::move(t));
    }

    // NOTE: profile_get_usage (API usage / "credits used" metering) was removed
    // — OpenMarketTerminal is free with no metering, so the AI must not surface
    // usage/credit accounting.

    // ── profile_get_notifications ────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "profile_get_notifications";
        t.description = "Get in-app notification history. Can filter to unread only.";
        t.category = "profile";
        t.input_schema.properties = QJsonObject{
            {"limit", QJsonObject{{"type", "integer"}, {"description", "Max notifications (default: 20)"}}},
            {"offset", QJsonObject{{"type", "integer"}, {"description", "Pagination offset (default: 0)"}}},
            {"unread_only",
             QJsonObject{{"type", "boolean"}, {"description", "Only return unread notifications (default: false)"}}},
        };
        t.handler = [](const QJsonObject& args) -> ToolResult {
            int limit = args["limit"].toInt(20);
            int offset = args["offset"].toInt(0);
            bool unread = args["unread_only"].toBool(false);

            using namespace openmarketterminal::notifications;
            const auto& history = NotificationService::instance().history();

            QJsonArray arr;
            int count = 0;
            int skipped = 0;
            for (const auto& rec : history) {
                if (unread && rec.read)
                    continue;
                if (skipped < offset) {
                    ++skipped;
                    continue;
                }
                if (count >= limit)
                    break;

                QJsonObject obj;
                obj["id"] = rec.id;
                obj["title"] = rec.request.title;
                obj["message"] = rec.request.message;
                obj["read"] = rec.read;
                obj["time"] = rec.received_at.toString(Qt::ISODate);
                arr.append(obj);
                ++count;
            }

            QJsonObject result;
            result["notifications"] = arr;
            result["total"] = arr.size();
            return ToolResult::ok("OK", QJsonValue(result));
        };
        tools.push_back(std::move(t));
    }

    // ── profile_mark_notification_read ───────────────────────────────────────
    {
        ToolDef t;
        t.name = "profile_mark_notification_read";
        t.description = "Mark a specific in-app notification as read by its ID.";
        t.category = "profile";
        t.input_schema.properties = QJsonObject{
            {"id", QJsonObject{{"type", "integer"}, {"description", "Notification ID to mark as read"}}},
        };
        t.input_schema.required = {"id"};
        t.handler = [](const QJsonObject& args) -> ToolResult {
            int id = args["id"].toInt(-1);
            if (id < 0)
                return ToolResult::fail("Missing or invalid 'id'");
            openmarketterminal::notifications::NotificationService::instance().mark_read(id);
            QJsonObject result;
            result["success"] = true;
            result["id"] = id;
            return ToolResult::ok("Marked as read", QJsonValue(result));
        };
        tools.push_back(std::move(t));
    }

    // ── profile_mark_all_notifications_read ──────────────────────────────────
    {
        ToolDef t;
        t.name = "profile_mark_all_notifications_read";
        t.description = "Mark all in-app notifications as read at once.";
        t.category = "profile";
        t.input_schema.properties = QJsonObject{};
        t.handler = [](const QJsonObject&) -> ToolResult {
            openmarketterminal::notifications::NotificationService::instance().mark_all_read();
            return ToolResult::ok("All notifications marked as read");
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace openmarketterminal::mcp::tools
