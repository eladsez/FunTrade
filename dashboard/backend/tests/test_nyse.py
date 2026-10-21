from datetime import date, datetime

import pytest

from app import nyse

NY = nyse.NY


def at(y, m, d, hh=12, mm=0):
    return datetime(y, m, d, hh, mm, tzinfo=NY)


def ms(y, m, d, hh, mm):
    return int(at(y, m, d, hh, mm).timestamp() * 1000)


@pytest.mark.parametrize("d, expected", [
    (date(2026, 10, 21), True),    # a Wednesday
    (date(2026, 10, 24), False),   # Saturday
    (date(2026, 11, 26), False),   # Thanksgiving
    (date(2026, 11, 27), True),    # the day after: a session, closing early
    (date(2026, 7, 3), False),     # Independence Day observed (the 4th is a Saturday)
    (date(2027, 3, 26), False),    # Good Friday 2027
    (date(2027, 12, 24), False),   # Christmas observed (the 25th is a Saturday)
    (date(2027, 12, 31), True),    # New Year 2028 falls on a Saturday: not observed in 2027
])
def test_sessions_follow_the_nyse_calendar(d, expected):
    assert nyse.is_session(d) is expected


def test_close_times():
    assert nyse.close_time(date(2026, 10, 21)) == nyse.CLOSE
    assert nyse.close_time(date(2026, 12, 24)) == nyse.EARLY_CLOSE
    assert nyse.close_time(date(2027, 11, 26)) == nyse.EARLY_CLOSE


def test_sessions_between_skips_weekends_and_holidays():
    wed, mon = nyse.day_of(date(2026, 11, 25)), nyse.day_of(date(2026, 11, 30))
    assert nyse.sessions_between(wed, mon) == 2, "Thanksgiving and the weekend don't count"
    assert nyse.sessions_between(wed, wed) == 0
    fri, mon2 = nyse.day_of(date(2026, 10, 16)), nyse.day_of(date(2026, 10, 19))
    assert nyse.sessions_between(fri, mon2) == 1


def test_last_closed_session():
    assert nyse.last_closed_session(at(2026, 10, 21, 15, 59)) == nyse.day_of(date(2026, 10, 20))
    assert nyse.last_closed_session(at(2026, 10, 21, 16, 0)) == nyse.day_of(date(2026, 10, 21))
    assert nyse.last_closed_session(at(2026, 10, 25, 12)) == nyse.day_of(date(2026, 10, 23)), "Sunday → Friday"
    assert nyse.last_closed_session(at(2026, 11, 27, 13, 0)) == nyse.day_of(date(2026, 11, 27)), "early close"
    assert nyse.last_closed_session(at(2026, 11, 27, 12, 59)) == nyse.day_of(date(2026, 11, 25))


def test_session_in_progress():
    s = nyse.session(at(2026, 10, 21, 11, 0))
    assert s.open and s.today_is_session and s.holiday is None and not s.early_close
    assert s.day == "2026-10-21"
    assert s.close_ms == ms(2026, 10, 21, 16, 0)
    assert s.cutoff_ms == ms(2026, 10, 21, 15, 50)
    assert s.eod_ms == ms(2026, 10, 21, 18, 0)
    assert s.known


def test_before_the_open_describes_today():
    s = nyse.session(at(2026, 10, 21, 8, 0))
    assert not s.open and s.day == "2026-10-21" and s.open_ms == ms(2026, 10, 21, 9, 30)


def test_after_the_close_describes_the_next_session():
    s = nyse.session(at(2026, 10, 23, 17, 0))   # Friday evening
    assert not s.open and s.day == "2026-10-26"
    assert s.open_ms == ms(2026, 10, 26, 9, 30)


def test_holiday_names_the_holiday_and_points_to_the_next_session():
    s = nyse.session(at(2026, 11, 26, 11, 0))
    assert not s.open and not s.today_is_session
    assert s.holiday == "Thanksgiving Day"
    assert s.day == "2026-11-27" and s.early_close
    assert s.close_ms == ms(2026, 11, 27, 13, 0)
    assert s.cutoff_ms == ms(2026, 11, 27, 12, 50), "MOC cutoff 10 min before the early close"


def test_early_close_day_is_closed_after_13():
    s = nyse.session(at(2026, 12, 24, 14, 0))
    assert not s.open and s.day == "2026-12-28"


def test_outside_the_published_years_weekdays_are_assumed():
    s = nyse.session(at(2028, 3, 1, 11, 0))
    assert s.open and not s.known
