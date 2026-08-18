# dup falls through to each native type's own accurate "undefined
# method" error rather than a generic one, for kinds that don't have a
# well-defined shallow copy (Regexp, Time, File, Socket, ...).
Regexp.new("abc").dup()
