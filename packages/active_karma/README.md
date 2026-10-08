# active_karma

Project trust signals into a persona state with write and visibility decisions.

## Installation

`active_karma` is not published to the registry yet. Until it is, copy `packages/active_karma` from a checkout of the [Diamond repository](https://github.com/diamond-language/diamond) into your project as `cuts/active_karma/`, then load it with `require_cut "active_karma"`. `facet update` leaves hand-copied cuts in place.

## Usage

Karma is a pure projection: you store trust events however you like (a table, a log), pass
them oldest first to `Projector.project`, and get back a `PersonaState` to decide with. Nothing
is persisted by this cut.

```ruby
require_cut "active_karma"

# Trust events recorded for one persona, oldest first. Each needs an
# event_type and a created_at.
events = [
  {"event_type": ActiveKarma::Signal::SPAM_DETECTED, "created_at": 1000},
]

state = ActiveKarma::Projector.project(events)
state.limited?()            # => true
state.write_throttled?()    # => true
state.can_write?()          # => true (throttled, not denied)

events.push({"event_type": ActiveKarma::Signal::MANUALLY_BLOCKED, "created_at": 2000})
state = ActiveKarma::Projector.project(events)
state.blocked?()            # => true
state.can_write?()          # => false

ActiveKarma::Projector.project([{"event_type": ActiveKarma::Signal::MANUALLY_CLEARED, "created_at": 3000}]).normal?()
# => true: MANUALLY_CLEARED resets every dimension

ActiveKarma::PersonaState.default()   # the zero-signal baseline: normal, allowed, visible
```

In an application, load a persona's events from your own table before each decision that
matters, such as accepting a comment:

```ruby
def may_post?(db, persona_id)
  rows = db.query("SELECT event_type, created_at FROM trust_events WHERE persona_id = ? ORDER BY created_at", [persona_id])
  ActiveKarma::Projector.project(rows).can_write?()
end
```

## Notes

`ActiveKarma::Signal` lists the event types (`SPAM_DETECTED`, `ABUSE_DETECTED`, `COMPROMISED`,
`RATE_LIMITED`, `MANUALLY_BLOCKED`, `MANUALLY_CLEARED`, and so on). A state answers
`trust_level` (`normal?`, `limited?`, `blocked?`, `shadowbanned?`, `review_required?`),
`write_permission` (`write_allowed?`, `write_throttled?`, `write_denied?`), `visibility`
(`visible?`, `shadow?`, `restricted?`) and `locked?`.
