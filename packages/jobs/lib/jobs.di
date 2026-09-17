# Durable, DB-backed background/scheduled work -- see README.md for
# the full rationale and the schema a consuming app owns (this package
# creates no tables of its own, matching packages/active_record's own
# Migrator: an app's migrations own its schema, this package only ever
# reads/writes rows in tables the app already created).
#
# One file per concern, matching packages/cookies's own split:
# ./jobs/queue (enqueue/schedule -- no polling, no execution) first,
# then ./jobs/worker (claim/run/retry -- the only piece that actually
# calls a handler), since the queue half has no dependency on the
# worker half but not the other way around.
require "./jobs/queue"
require "./jobs/worker"
