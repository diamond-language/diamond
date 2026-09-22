# active_karma

Project trust signals into a persona state with write and visibility decisions.

## Installation

Install the cut at `cuts/active_karma/` and load it with `require_cut "active_karma"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```ruby
require_cut "active_karma"

events = [
  {"event_type": ActiveKarma::Signal::SPAM_DETECTED, "created_at": 1000},
  {"event_type": ActiveKarma::Signal::MANUALLY_CLEARED, "created_at": 2000},
]
state = ActiveKarma::Projector.project(events)
state.normal?()       # => true (MANUALLY_CLEARED resets every dimension)

ActiveKarma::PersonaState.default()  # the zero-signal baseline
```

## Notes

Pass events oldest first. Each event needs `event_type` and `created_at`; persistence is the application’s responsibility.
