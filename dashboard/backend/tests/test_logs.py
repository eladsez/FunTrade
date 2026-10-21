from datetime import datetime

from app import logs

NY = logs.NY


def t(stamp: str) -> int:
    return int(datetime.strptime(stamp, "%Y-%m-%d %H:%M:%S").replace(tzinfo=NY).timestamp() * 1000)


def write(path, *lines):
    path.write_text("".join(f"[{s}.123] [FunTrade] [{lvl}] {msg}\n" for s, lvl, msg in lines))


def test_groups_by_run_and_folds_repeats(tmp_path):
    f = tmp_path / "funtrade.log"
    write(f,
          ("2026-10-20 09:00:00", "warning", "before the journal"),
          ("2026-10-20 16:55:08", "error", "MISMATCH IP: bot books -53, broker holds 0"),
          ("2026-10-20 16:55:09", "warning", "untracked RapidOrder GWW"),
          ("2026-10-20 16:56:00", "info", "Running 61 baskets."),
          ("2026-10-20 17:34:18", "warning", "untracked RapidOrder GWW"),
          ("2026-10-20 17:40:00", "critical", "something bad"),
          ("2026-10-20 18:00:00", "warning", "untracked RapidOrder GWW"))
    runs = logs.problems(f, [t("2026-10-20 17:34:00"), t("2026-10-20 16:55:00")])
    assert [r["started_at"] for r in runs] == [t("2026-10-20 17:34:00"), t("2026-10-20 16:55:00"), None]
    assert [r["current"] for r in runs] == [True, False, False]

    cur = runs[0]["lines"]
    assert [l["message"] for l in cur] == ["untracked RapidOrder GWW", "something bad"], "newest first, info skipped"
    assert cur[0]["count"] == 2 and cur[0]["time"] == "2026-10-20 17:34:18" and cur[0]["last"] == "2026-10-20 18:00:00"
    assert cur[1]["level"] == "error", "critical shows as error"
    assert all("MISMATCH" not in l["message"] for l in cur), "the earlier run's problem is not current"
    assert [l["message"] for l in runs[1]["lines"]] == ["untracked RapidOrder GWW", "MISMATCH IP: bot books -53, broker holds 0"]
    assert runs[2]["lines"][0]["message"] == "before the journal"


def test_without_journaled_starts_everything_is_one_group(tmp_path):
    f = tmp_path / "funtrade.log"
    write(f, ("2026-10-20 09:00:00", "warning", "a"), ("2026-10-20 09:00:01", "warning", "a"))
    runs = logs.problems(f, [])
    assert len(runs) == 1 and runs[0]["started_at"] is None
    assert runs[0]["current"] is True, "nothing says it's an older run"
    assert runs[0]["lines"][0]["count"] == 2


def test_same_text_at_different_levels_stays_apart(tmp_path):
    f = tmp_path / "funtrade.log"
    write(f, ("2026-10-20 09:00:00", "warning", "x"), ("2026-10-20 09:00:01", "error", "x"))
    assert len(logs.problems(f, [])[0]["lines"]) == 2


def test_limit_keeps_the_newest(tmp_path):
    f = tmp_path / "funtrade.log"
    write(f, *[(f"2026-10-20 09:00:{i:02d}", "warning", f"m{i}") for i in range(10)])
    assert [l["message"] for l in logs.problems(f, [], limit=3)[0]["lines"]] == ["m9", "m8", "m7"]


def test_missing_log(tmp_path):
    assert logs.problems(tmp_path / "nope.log", []) == []
