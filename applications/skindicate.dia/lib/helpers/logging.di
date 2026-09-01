# Thin call-site-compatible wrapper around packages/logger's
# RequestLogging (see lib/middleware.di's own RequestLogging.configure
# call and RequestLogging.call, which replace what used to be
# AppLogger/logging_middleware here) -- kept so every existing
# log_debug/log_info/log_warn(request, context, event, fields) call
# site across the app needed no changes.
def log_debug(request, context, event, fields: Hash = {}) = RequestLogging.debug(request, context, event, fields)
def log_info(request, context, event, fields: Hash = {}) = RequestLogging.info(request, context, event, fields)
def log_warn(request, context, event, fields: Hash = {}) = RequestLogging.warn(request, context, event, fields)
