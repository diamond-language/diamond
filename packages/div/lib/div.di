# ERB-style view templates: translates .html.drb source into ordinary .di
# source (packages/drb/lib/drb/compiler.di), plus small runtime helpers
# shared with generated output and with packages/rack (lib/drb/runtime.di).
# One file per logical grouping, both reopening the same `module Drb`
# (see docs/roadmap.md's "Module/class reopening" -- the same mechanism
# packages/rack and packages/arel already use for their own file splits).
require "./drb/compiler"
require "./drb/runtime"
