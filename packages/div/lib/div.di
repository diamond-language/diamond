# ERB-style view templates: translates .html.div source into ordinary .di
# source (packages/div/lib/div/compiler.di), plus small runtime helpers
# shared with generated output and with packages/rack (lib/div/runtime.di).
# One file per logical grouping, both reopening the same `module Div`
# (see docs/roadmap.md's "Module/class reopening" -- the same mechanism
# packages/rack and packages/arel already use for their own file splits).
require "./div/compiler"
require "./div/runtime"
