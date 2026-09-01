# One file per class, matching packages/cookies/active_record's own
# split (their own lib/*.di has the full rationale) -- Logger itself
# first, then RequestLogging, which depends on it.
require "./logger/logger"
require "./logger/request_logging"
