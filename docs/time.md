# Time and calendar guide

Diamond's `Time` represents an instant as fractional Unix-epoch seconds. Each
value also has an immutable display mode:

- UTC;
- the process-local timezone; or
- a fixed UTC offset.

The display mode controls calendar fields and formatting. It does not change
the represented instant, equality, ordering, hashing, or elapsed-time
arithmetic.

Diamond deliberately does not provide named IANA timezone selection such as
`"America/New_York"`. Local time uses the timezone configured for the process;
explicit zones use UTC offsets. Fixed offsets are stored per `Time` value and
never mutate the process-global `TZ` setting, so they are safe when independent
VMs run on concurrent threads.

## Creating times

Current wall-clock time:

```ruby
local_now = Time.now()
utc_now = Time.utc_now()
```

From Unix-epoch seconds:

```ruby
local_epoch = Time.at(0)
fractional = Time.at(0.125)
```

From complete calendar fields:

```ruby
utc = Time.utc(2026, 8, 30, 12, 30, 0)
local = Time.local(2026, 8, 30, 12, 30, 0)
india = Time.fixed("+05:30", 2026, 8, 30, 12, 30, 0)
offset_seconds = Time.fixed(19800, 2026, 8, 30, 12, 30, 0)
```

Calendar constructors require all six `Int` fields. Impossible dates such as
February 30 are rejected. `Time.local` asks libc to resolve daylight-saving
transitions: a nonexistent local time may normalize forward, and an ambiguous
time uses the platform's choice.

## Parsing and serializing ISO-8601

`Time.parse` accepts a strict timestamp with an explicit zone:

```ruby
Time.parse("2026-08-30T12:30:00Z")
Time.parse("2026-08-30T12:30:00.125+05:30")
Time.parse("2026-08-30T12:30:00-07:00")
Time.parse("2026-08-30T12:30:00+05:30:01")
```

The accepted grammar is:

```text
YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM[:SS]|-HH:MM[:SS])
```

Dates are validated rather than silently normalized. A `Z` input produces UTC
mode; a signed input preserves that fixed offset.

Use `iso8601` for canonical, round-trippable output:

```ruby
t = Time.parse("2026-08-30T12:30:00.125+05:30")
t.iso8601()   # => "2026-08-30T12:30:00+05:30"
t.iso8601(3)  # => "2026-08-30T12:30:00.125+05:30"
```

Precision must be an `Int` from 0 through 9. Non-minute offsets include their
seconds so parsing the result does not lose offset information.

## Changing the display zone

Zone conversion returns a new value at the same instant:

```ruby
t.utc()
t.localtime()
t.localtime("Z")
t.localtime("+05:30")
t.localtime(-28800)
```

String offsets accept `Z` or signed `HH:MM[:SS]`. Integer offsets are seconds
strictly between `-86400` and `86400`.

```ruby
t.utc?()       # true only for explicit UTC mode
t.utc_offset() # active offset in seconds
```

Fixed zero (`localtime("Z")` or `Time.fixed("Z", ...)`) has an offset of zero
but is not explicit UTC, so `utc?()` returns `false`.

## Calendar fields and formatting

```ruby
t.year()
t.month() # 1..12
t.day()
t.hour()
t.min()
t.sec()
t.wday()  # 0=Sunday .. 6=Saturday
t.yday()  # 1..366
```

`strftime` delegates to the system's `strftime(3)` implementation:

```ruby
t.strftime("%Y-%m-%d %H:%M:%S %z")
```

`%z` on a fixed-offset Time is the one exception: Diamond substitutes it
itself before the format string ever reaches the system implementation,
rather than relying on the platform to honor the offset, since not every
`strftime(3)` does (confirmed on Darwin). Every other directive, `%Z`
included, still delegates straight through.

`to_s()` and interpolation use Diamond's default human-readable format.
`to_i()` returns truncated epoch seconds; `to_f()` returns the fractional
epoch.

## Instants, arithmetic, and comparison

Numeric addition and subtraction use elapsed seconds:

```ruby
deadline = Time.now() + 30
earlier = deadline - 5.5
elapsed = deadline - earlier # => 5.5
```

Adding two `Time` values is an error. Subtracting two returns `Float` seconds.
The result of adding or subtracting a duration preserves the receiver's display
mode.

All comparisons use the represented instant, not displayed fields:

```ruby
utc = Time.parse("2026-08-30T07:00:00Z")
west = Time.parse("2026-08-30T00:00:00-07:00")

utc == west # => true
utc <= west # => true
```

`<`, `<=`, `>`, `>=`, `==`, and `!=` work between `Time` values. A `Time` can
also be used as a Hash key; equal epochs hash equally regardless of zone mode.

## Numeric durations and relative time

`Int` and `Float` provide fixed-length duration units:

```ruby
30.seconds()
5.minutes()
1.5.hours()
2.days()
3.weeks()
```

Singular spellings are also available. These values remain numbers measured in
seconds and compose with arithmetic:

```ruby
2.hours() + 30.minutes() # => 9000
```

Create a local `Time` relative to the current wall clock with:

```ruby
15.minutes().ago()
2.days().from_now()
```

`NaN` and positive or negative infinity are rejected as durations.

## Calendar-aware movement

Fixed numeric durations and calendar movement are intentionally different.
Across a daylight-saving transition, `time + 1.day()` always advances exactly
86,400 seconds, while `time.days_from_now(1)` preserves the local clock time
and may advance 23 or 25 elapsed hours.

```ruby
t.days_ago(1)
t.days_from_now(1)
t.weeks_ago(2)
t.weeks_from_now(2)
t.months_ago(1)
t.months_from_now(1)
t.years_ago(1)
t.years_from_now(1)
```

Calendar movement takes an `Int`, preserves fractional seconds and display
mode, and accepts negative values to reverse direction. Month and year movement
clamps to the target month's final day; for example, one month before March 31
is February 28 or 29.

## Calendar boundaries

Boundary helpers return new values in the receiver's display zone:

```ruby
t.beginning_of_day()
t.end_of_day()
t.beginning_of_week()
t.end_of_week()
t.beginning_of_month()
t.end_of_month()
t.beginning_of_quarter()
t.end_of_quarter()
t.beginning_of_year()
t.end_of_year()
```

Weeks begin Monday. Quarters are Jan–Mar, Apr–Jun, Jul–Sep, and Oct–Dec. End
helpers return the final microsecond before the following period. Local
boundaries follow DST, so a local day or week does not necessarily contain a
fixed number of elapsed seconds.

## Predicates and weekdays

```ruby
t.today?()
t.yesterday?()
t.tomorrow?()
t.past?()
t.future?()
t.on_weekday?()
t.on_weekend?()
t.same_day?(other)
```

Date predicates use the receiver's display zone. `same_day?` projects the
other instant into the receiver's zone before comparing dates. `past?` and
`future?` compare absolute instants.

Navigate Monday–Friday dates without counting weekends:

```ruby
t.next_weekday()
t.next_weekday(5)
t.previous_weekday()
t.previous_weekday(10)
```

Counts are non-negative; zero preserves the exact instant. Navigation keeps
the wall-clock fields, fractional seconds, and display mode across local DST.

## Monotonic time

`Time.monotonic()` is not a calendar `Time`. It returns a `Float` from an
unspecified monotonic origin and is only meaningful for elapsed durations:

```ruby
started = Time.monotonic()
do_work()
elapsed = Time.monotonic() - started
```

Do not serialize it, compare it across processes, or treat it as Unix time.

## Deliberate limits

- No named IANA timezone selection or bundled timezone database.
- No date-only value distinct from `Time`.
- No free-form or locale-dependent parsing.
- No numeric month/year duration because those units need a reference date;
  use calendar movement on a `Time` value.

For lower-level implementation-adjacent I/O reference material, see
[I/O and native services](io.md).
