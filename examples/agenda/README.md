# examples/agenda

Recurring events expanded into a dated agenda or a month calendar -- a
tour of Diamond's `Time` API.

```text
$ cat events.txt
every mon,wed,fri 09:30 Standup
every 2 weeks from 2026-09-07 15:00 Sprint review
last fri 16:00 Demo day
weekdays 08:15 Commute
monthly 31 Pay credit card
yearly 10-03 Anniversary
2026-10-02 18:30 Concert

$ diamond agenda.di --today 2026-09-25 --days 7 events.txt
Fri 25 Sep 2026
  08:15   Commute
  09:30   Standup
  16:00   Demo day
...
Wed 30 Sep 2026
  all day Pay credit card
  08:15   Commute
  09:30   Standup
...

$ diamond agenda.di --today 2026-09-25 --cal events.txt
       September 2026
 Mo  Tu  We  Th  Fr  Sa  Su
      1*  2*  3*  4*  5   6
  7*  8*  9* 10* 11* 12  13
 14* 15* 16* 17* 18* 19  20
 21* 22* 23* 24*[25] 26  27
 28* 29* 30*
```

```text
agenda [--today YYYY-MM-DD] [--days N] [--offset +HH:MM] [--cal] FILE
```

Rules: a one-off date, `every DAYS`, `every N weeks from DATE`,
`weekdays`, `monthly DAY` (a 31st falls on the last day of shorter months),
`last WEEKDAY` (of each month), and `yearly MM-DD`, each with an optional
`HH:MM` and a title. Without a time an event is all day. Exit status: 0,
64 for a usage error or a bad `--today`/`--offset`, 65 for an unreadable
rule, 66 when the file can't be opened.

## What it shows

- **Calendar arithmetic.** `days_from_now`, `end_of_month`, `beginning_of_month`,
  `wday`, `on_weekday?`, and `same_day?` drive the expansion
  (`lib/expand.di`) and the grid (`lib/calendar.di`). "Last Friday" is a
  Friday whose date a week later is in another month.
- **Fixed-offset zones.** Every occurrence is a UTC `Time`; `--offset`
  shows it with `localtime("+05:30")`, and grouping by the shifted date
  means an 08:15 UTC event appears the previous evening at `-10:00`.
- **Parsing and validation.** `Time.parse` rejects impossible dates such as
  `2026-02-30` with an `ArgumentError` that names them, which the CLI turns
  into a line-numbered error.
- **A sealed rule hierarchy** matched with Array patterns in `parse_rule`
  (`when ["every", count, "weeks", "from", start, *rest]`) and exhaustively
  by class in `occurs_on?`.
- **Sorting by a composite key**: `[event.at().to_i(), event.title()]`.

`Occurrence` is a plain class rather than a struct because struct fields
need a type, and native values such as `Time` have no type name to
annotate with.

## Test

```sh
bash smoke_test.sh
```

This checks the agenda, the calendar, and an offset view against expected
output (interpreted and as a `diamond build` binary), month-end and
leap-year clamping, a month with five Fridays, a biweekly rule, and the
error exits.
