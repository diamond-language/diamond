# Routing, request params, and generic responses -- the controller layer
# sitting on top of packages/rack (request/response convention) and
# alongside packages/div (view rendering) and packages/active_record
# (models). One file per logical grouping, both reopening the same
# `module Dials` (see docs/roadmap.md's "Module/class reopening" -- the
# same mechanism packages/rack and packages/arel already use for their
# own file splits).
require "./dials/response"
require "./dials/params"
require "./dials/router"
