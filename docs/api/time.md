# Time

[API reference](index.md) · Previous: [Dir](dir.md)

A `Time` is an instant, stored as fractional seconds since the Unix epoch,
plus a display zone: UTC, the process's local zone, or a fixed offset. The
zone changes calendar fields and formatting, never the instant, so equality,
ordering and arithmetic ignore it. There are no named IANA zones. The
[time guide](../time.md) explains daylight saving, calendar movement and the
other rules in more depth.

```ruby
t = Time.utc(2026, 8, 30, 12, 30, 15)
t.iso8601()                       # => "2026-08-30T12:30:15Z"
(t + 60).iso8601()                # => "2026-08-30T12:31:15Z"
t.localtime("+05:30").hour()      # => 18
```

## Operators

| Operator | Result |
|---|---|
| `t + seconds`, `t - seconds` | a `Time` moved by a number of seconds, in the same zone |
| `t - other` | the `Float` seconds between two Times |
| `==`, `!=`, `<`, `<=`, `>`, `>=` | compare instants, whatever their zones |

```ruby
Time.utc(2026, 8, 30, 12, 30, 15) - Time.utc(2026, 8, 30, 12, 0, 0)   # => 1815.0
Time.parse("2026-08-30T07:00:00Z") == Time.parse("2026-08-30T00:00:00-07:00")   # => true
```

## Creating times

### `Time.now() -> Time`

The current time, in the local zone.

### `Time.utc_now() -> Time`

The current time, in UTC.

### `Time.at(seconds) -> Time`

The instant `seconds` (an `Int` or `Float`) after the epoch, in the local zone.

```ruby
Time.at(0).utc().iso8601()   # => "1970-01-01T00:00:00Z"
```

### `Time.utc(year, month, day, hour, minute, second) -> Time`

### `Time.local(year, month, day, hour, minute, second) -> Time`

The time with these calendar fields, in UTC or the local zone. All six are
required `Int`s; a date that does not exist raises `ArgumentError`.

### `Time.fixed(offset, year, month, day, hour, minute, second) -> Time`

The same in a fixed zone. `offset` is `"Z"`, a signed `"HH:MM[:SS]"` String,
or seconds east of UTC.

```ruby
Time.fixed("+05:30", 2026, 8, 30, 12, 30, 0).iso8601()   # => "2026-08-30T12:30:00+05:30"
```

### `Time.parse(text) -> Time`

Reads a strict ISO-8601 timestamp with a zone,
`YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM[:SS]|-HH:MM[:SS])`. Anything else,
or a date that does not exist, raises `ArgumentError`.

```ruby
Time.parse("2026-08-30T12:30:00-07:00").utc().iso8601()   # => "2026-08-30T19:30:00Z"
```

### `Time.monotonic() -> Float`

Seconds from an unspecified starting point that never goes backwards. It is
not a `Time`; subtract two readings to measure elapsed time.

## Zones and output

### `iso8601(digits = 0) -> String`

The ISO-8601 text, with `digits` (0 to 9) of fractional seconds. `Time.parse`
reads it back exactly.

```ruby
Time.at(0.125).utc().iso8601(3)   # => "1970-01-01T00:00:00.125Z"
```

### `localtime(offset = nil) -> Time`

The same instant in the local zone, or in a fixed zone given like
`Time.fixed`'s `offset`.

```ruby
Time.utc(2026, 8, 30, 12, 30, 0).localtime("+05:30").iso8601()   # => "2026-08-30T18:00:00+05:30"
```

### `strftime(format) -> String`

Formats with the system's `strftime(3)`; `%z` is filled in by d6 for a fixed
zone.

```ruby
Time.utc(2026, 8, 30, 12, 30, 0).strftime("%Y-%m-%d %H:%M")   # => "2026-08-30 12:30"
```

### `to_f() -> Float`

Seconds since the epoch.

### `to_i() -> Int`

Whole seconds since the epoch.

```ruby
Time.utc(2026, 8, 30, 12, 30, 15).to_i()   # => 1788093015
```

### `to_s() -> String`

A readable form, also used by interpolation.

```ruby
Time.utc(2026, 8, 30, 12, 30, 15).to_s()   # => "2026-08-30 12:30:15 UTC"
```

### `utc() -> Time`

The same instant in UTC.

### `utc?() -> Bool`

Whether the zone is UTC itself; a fixed `"Z"` offset is not.

### `utc_offset() -> Int`

The zone's offset from UTC, in seconds.

```ruby
Time.utc(2026, 8, 30, 0, 0, 0).localtime("+05:30").utc_offset()   # => 19800
```

## Calendar fields

### `year() -> Int`

### `month() -> Int`

### `day() -> Int`

The date, with `month` from 1 to 12.

### `hour() -> Int`

### `min() -> Int`

### `sec() -> Int`

The time of day.

### `wday() -> Int`

The day of the week, `0` for Sunday to `6` for Saturday.

### `yday() -> Int`

The day of the year, from 1.

```ruby
t = Time.utc(2026, 8, 30, 12, 30, 15)
[t.year(), t.month(), t.day(), t.wday(), t.yday()]   # => [2026, 8, 30, 0, 242]
```

## Calendar movement

These keep the time of day across daylight-saving changes, unlike adding
`1.day()`, which is always 86,400 seconds. Each takes an `Int`; a negative one
moves the other way. Moving by months or years lands on the last day of a
shorter month.

### `days_ago(n) -> Time`

### `days_from_now(n) -> Time`

### `weeks_ago(n) -> Time`

### `weeks_from_now(n) -> Time`

### `months_ago(n) -> Time`

### `months_from_now(n) -> Time`

### `years_ago(n) -> Time`

### `years_from_now(n) -> Time`

```ruby
Time.utc(2026, 3, 31, 0, 0, 0).months_ago(1).iso8601()   # => "2026-02-28T00:00:00Z"
```

### `next_weekday(n = 1) -> Time`

### `previous_weekday(n = 1) -> Time`

`n` Monday-to-Friday days later or earlier, skipping weekends.

```ruby
Time.utc(2026, 8, 30, 9, 0, 0).next_weekday().iso8601()   # => "2026-08-31T09:00:00Z"
```

## Calendar boundaries

Each returns the first instant of the period, or the last microsecond of it,
in the receiver's zone. Weeks start on Monday; quarters start in January,
April, July and October.

### `beginning_of_day() -> Time`

### `end_of_day() -> Time`

### `beginning_of_week() -> Time`

### `end_of_week() -> Time`

### `beginning_of_month() -> Time`

### `end_of_month() -> Time`

### `beginning_of_quarter() -> Time`

### `end_of_quarter() -> Time`

### `beginning_of_year() -> Time`

### `end_of_year() -> Time`

```ruby
t = Time.utc(2026, 8, 30, 12, 30, 15)
t.beginning_of_week().iso8601()    # => "2026-08-24T00:00:00Z"
t.end_of_month().iso8601()         # => "2026-08-31T23:59:59Z"
```

## Predicates

### `past?() -> Bool`

### `future?() -> Bool`

Whether the instant is before or after now.

### `today?() -> Bool`

### `yesterday?() -> Bool`

### `tomorrow?() -> Bool`

Whether the date, in the receiver's zone, is today, yesterday or tomorrow.

### `on_weekday?() -> Bool`

### `on_weekend?() -> Bool`

Whether the date is Monday to Friday, or Saturday or Sunday.

### `same_day?(other) -> Bool`

Whether `other` falls on the same date, seen in the receiver's zone.
