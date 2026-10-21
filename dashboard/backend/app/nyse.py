"""NYSE trading calendar: sessions, holidays and early closes, in New York time.

Source: NYSE Group's holiday and early-closings calendar (published 2025-12-23, covers
2026-2028). Update KNOWN_YEARS and the tables once a year. Outside the known years every
weekday counts as a full session, and `known` says so.

Days are the bot's day numbers (days since 1970-01-01 = the session's calendar date)."""
from dataclasses import dataclass
from datetime import date, datetime, time, timedelta
from zoneinfo import ZoneInfo

NY = ZoneInfo("America/New_York")
EPOCH = date(1970, 1, 1)

KNOWN_YEARS = range(2026, 2028)
HOLIDAYS = {
    date(2026, 1, 1): "New Year's Day",
    date(2026, 1, 19): "Martin Luther King Jr. Day",
    date(2026, 2, 16): "Washington's Birthday",
    date(2026, 4, 3): "Good Friday",
    date(2026, 5, 25): "Memorial Day",
    date(2026, 6, 19): "Juneteenth",
    date(2026, 7, 3): "Independence Day (observed)",
    date(2026, 9, 7): "Labor Day",
    date(2026, 11, 26): "Thanksgiving Day",
    date(2026, 12, 25): "Christmas Day",
    date(2027, 1, 1): "New Year's Day",
    date(2027, 1, 18): "Martin Luther King Jr. Day",
    date(2027, 2, 15): "Washington's Birthday",
    date(2027, 3, 26): "Good Friday",
    date(2027, 5, 31): "Memorial Day",
    date(2027, 6, 18): "Juneteenth (observed)",
    date(2027, 7, 5): "Independence Day (observed)",
    date(2027, 9, 6): "Labor Day",
    date(2027, 11, 25): "Thanksgiving Day",
    date(2027, 12, 24): "Christmas Day (observed)",
}
EARLY_CLOSES = {date(2026, 11, 27), date(2026, 12, 24), date(2027, 11, 26)}  # 13:00 ET

OPEN = time(9, 30)
CLOSE = time(16, 0)
EARLY_CLOSE = time(13, 0)
MOC_CUTOFF = timedelta(minutes=10)   # MOC orders must reach the exchange by close − 10 min
EOD_FETCH = time(18, 0)              # the bot decides after this (settled daily bars)


def day_of(d: date) -> int:
    return (d - EPOCH).days


def date_of(day: int) -> date:
    return EPOCH + timedelta(days=day)


def is_session(d: date) -> bool:
    return d.weekday() < 5 and d not in HOLIDAYS


def close_time(d: date) -> time:
    return EARLY_CLOSE if d in EARLY_CLOSES else CLOSE


def next_session(d: date) -> date:
    """The first session on or after d."""
    while not is_session(d):
        d += timedelta(days=1)
    return d


def sessions_between(after_day: int, through_day: int) -> int:
    """Sessions s with after_day < s <= through_day."""
    n, d, end = 0, date_of(after_day) + timedelta(days=1), date_of(through_day)
    while d <= end:
        n += is_session(d)
        d += timedelta(days=1)
    return n


def last_closed_session(now: datetime) -> int:
    """Day number of the latest session whose close has passed."""
    ny = now.astimezone(NY)
    d = ny.date()
    if not (is_session(d) and ny.time() >= close_time(d)):
        d -= timedelta(days=1)
        while not is_session(d):
            d -= timedelta(days=1)
    return day_of(d)


def _ms(d: date, t: time) -> int:
    return int(datetime.combine(d, t, NY).timestamp() * 1000)


@dataclass
class Session:
    open: bool                 # regular session in progress now
    today_is_session: bool
    holiday: str | None        # today's holiday, if any
    early_close: bool          # today (or the next session, when closed) closes at 13:00
    day: str                   # the session this describes: today's, or the next one
    open_ms: int
    close_ms: int
    cutoff_ms: int
    eod_ms: int
    known: bool                # the date is inside the published calendar


def session(now: datetime | None = None) -> Session:
    """Today's session if it hasn't closed yet, otherwise the next one."""
    ny = (now or datetime.now(NY)).astimezone(NY)
    today = ny.date()
    d = today
    if not is_session(d) or ny.time() >= close_time(d):
        d = next_session(today + timedelta(days=1))
    close = close_time(d)
    close_ms = _ms(d, close)
    return Session(
        open=d == today and ny.time() >= OPEN,
        today_is_session=is_session(today),
        holiday=HOLIDAYS.get(today),
        early_close=d in EARLY_CLOSES,
        day=d.isoformat(),
        open_ms=_ms(d, OPEN),
        close_ms=close_ms,
        cutoff_ms=close_ms - int(MOC_CUTOFF.total_seconds() * 1000),
        eod_ms=_ms(d, EOD_FETCH),
        known=d.year in KNOWN_YEARS,
    )
