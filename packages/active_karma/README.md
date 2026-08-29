# active_karma

System-authored persona trust state -- ported from MaquinasStack's Ruby
`ActiveKarma` gem (`active_karma/`, private monorepo). It watches for
signals (spam flags, rate-limit hits, abuse reports, manual review
actions) and folds them into a persona's standing: a trust level, a
write permission, a visibility modifier, and a set of locks.
`ActiveKarma` does not enforce anything itself -- it only *describes*
state for something else (a permission gate, a controller filter) to
read.

## What's ported, what isn't (yet)

This package is the pure value/state-machine core only:

- `ActiveKarma::Signal` -- the fixed signal vocabulary (`SPAM_DETECTED`,
  `ABUSE_DETECTED`, `MANUALLY_CLEARED`, ...), each namespaced
  `"karma.xxx"`, plus `.valid?`/`.namespaced`.
- `ActiveKarma::PersonaState` -- an immutable snapshot (`trust_level`,
  `write_permission`, `visibility`, `locks`) with the same query
  predicates as the Ruby original (`normal?`, `write_allowed?`,
  `compromised?`, `can_write?`, ...).
- `ActiveKarma::Projector.project(events)` -- folds an oldest-first
  Array of `{"event_type": String, "created_at": Int}` records into a
  `PersonaState`, via the identical state-transition table the Ruby
  `Projector` uses (same precedence rules -- e.g. a `RATE_LIMITED`
  signal never *loosens* an already-`denied` write permission back to
  `throttled`).

**Not ported**: the Ruby gem's `Repository`/`Configuration`/`Karma`
modules -- a persistence layer built on `ActiveStenographer` (an
append-only event store) plus Ruby-specific global-singleton wiring
(`Karma.repo ||= ...`). `ActiveStenographer` itself isn't ported yet,
and porting just enough of it to back this one gem would mean carrying
over the exact foundation-depending-on-its-consumers problem the
Ruby version's own architecture review flagged. A consuming app
supplies its own event storage instead (e.g. a plain
`packages/active_record` `Repository` over a `karma_events` table) and
passes the resulting rows straight into `Projector.project` as plain
Hashes -- there's no assumed schema beyond the two keys above.

## Usage

```ruby
require "../../active_karma/lib/active_karma"

events = [
  {"event_type": ActiveKarma::Signal::SPAM_DETECTED, "created_at": 1000},
  {"event_type": ActiveKarma::Signal::MANUALLY_CLEARED, "created_at": 2000},
]
state = ActiveKarma::Projector.project(events)
state.normal?()       # => true (MANUALLY_CLEARED resets every dimension)

ActiveKarma::PersonaState.default()  # the zero-signal baseline
```

## Diamond-specific notes

- The Ruby original's `PersonaState::DEFAULT` frozen constant is a
  `PersonaState.default()` factory method here instead -- a class-body
  constant can't safely reference the class it's still defining, and
  this class has no setters to begin with, so the practical effect is
  identical.
- `Projector`'s private `apply` class method (Ruby:
  `private_class_method :apply`) is a genuine top-level function
  (`apply_karma_signal`, `projector.di`) here, not nested inside
  `module ActiveKarma` -- confirmed directly that a plain `def` (no
  `self.`) declared inside a bare `module ... end` block isn't
  callable at all (not by its bare name, not as
  `ActiveKarma.apply_karma_signal(...)`) -- only `def self.x` methods
  and nested classes work there. It has to be a true top-level
  function, defined before `Projector.project` (which calls it) since
  top-level function calls resolve source order only, not forward.

## Test

```sh
make test-active-karma-package
# or: DIAMOND_BIN=../../build/diamond bash test.sh
```
