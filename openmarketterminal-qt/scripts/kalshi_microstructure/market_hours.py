from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, time, timezone
from zoneinfo import ZoneInfo


KALSHI_TZ = ZoneInfo("America/New_York")
THURSDAY = 3
MAINTENANCE_START = time(3, 0)
MAINTENANCE_END = time(5, 0)


@dataclass(frozen=True)
class MaintenanceWindow:
    active: bool
    label: str


def kalshi_maintenance_window(now: datetime | None = None) -> MaintenanceWindow:
    current = (now or datetime.now(timezone.utc)).astimezone(KALSHI_TZ)
    active = (
        current.weekday() == THURSDAY
        and MAINTENANCE_START <= current.time().replace(tzinfo=None) < MAINTENANCE_END
    )
    return MaintenanceWindow(
        active=active,
        label="Thursday 3:00-5:00 AM ET",
    )


def daily_crypto_restart_window(now: datetime | None = None) -> MaintenanceWindow:
    current = (now or datetime.now(timezone.utc)).astimezone(KALSHI_TZ)
    active = MAINTENANCE_START <= current.time().replace(tzinfo=None) < MAINTENANCE_END
    return MaintenanceWindow(
        active=active,
        label="Daily 3:00-5:00 AM ET crypto restart guard",
    )
