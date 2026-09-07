module Dials

  # A route pattern's ":name" segments capture the corresponding path
  # segment into `params["name"]`; every other segment must match
  # literally. Segment *count* must match exactly too -- no wildcard/
  # catch-all segment in this first version (see ROADMAP.md). First
  # registered route wins on a tie (same verb, both would match).
  class Router
    def initialize()
      @routes = []
    end

    def get(pattern: String, handler: Callable[3], filters: Array = [], name = nil)
      self.register("GET", pattern, handler, filters, name)
    end

    def post(pattern: String, handler: Callable[3], filters: Array = [], name = nil)
      self.register("POST", pattern, handler, filters, name)
    end

    private

    def register(verb: String, pattern: String, handler: Callable[3], filters: Array, name)
      @routes << {"verb": verb, "segments": pattern.split("/"), "handler": handler, "filters": filters, "name": name}
      self
    end

    public

    # Every registered route, in registration order -- lets an external
    # tool (Dials::PathHelpers' own generator script, in whichever app
    # embeds this package) introspect the fully-built route table
    # without this class needing to know anything about path-helper
    # generation itself.
    def routes() = @routes

    # nil if `path_segments` doesn't match this route's own pattern
    # segments (wrong length, or a literal segment differs); otherwise the
    # Hash of ":name" captures (possibly empty).
    def match_segments(pattern_segments: Array, path_segments: Array)
      if pattern_segments.length() != path_segments.length()
        return nil
      end
      captured = {}
      index = 0
      while index < pattern_segments.length()
        pattern_segment = pattern_segments[index]
        path_segment = path_segments[index]
        if pattern_segment.length() > 0 && pattern_segment[0] == ":"
          captured[pattern_segment.slice(1, pattern_segment.length() - 1)] = path_segment
        elsif pattern_segment != path_segment
          return nil
        end
        index += 1
      end
      captured
    end

    def dispatch(request, context)
      path = request["path"]
      question = path.index_of("?")
      path_only = if question == nil then path else path.slice(0, question) end
      path_segments = path_only.split("/")
      method = request["method"]

      matched_route = @routes.find() do |route|
        route["verb"] == method && self.match_segments(route["segments"], path_segments) != nil
      end

      if matched_route == nil
        return Response.not_found(path_only)
      end
      matched_handler = matched_route["handler"]
      matched_captures = self.match_segments(matched_route["segments"], path_segments)
      matched_filters = matched_route["filters"]

      # Path params win over a same-named query/body one -- applied after,
      # onto the same Hash Params.parse already built.
      params = Params.parse(request)
      capture_index = 0
      while capture_index < matched_captures.length()
        params[matched_captures.key_at(capture_index)] = matched_captures.value_at(capture_index)
        capture_index += 1
      end

      filter_index = 0
      while filter_index < matched_filters.length()
        response = matched_filters[filter_index](request, context, params)
        if response != nil
          return response
        end
        filter_index += 1
      end

      matched_handler(request, context, params)
    end
  end

  # Memoizes one Router per class per VM -- exactly RackChain's own
  # pattern (packages/rack/lib/rack/rack_chain.di), reused here rather
  # than sharing that class directly since a Router isn't a rack chain
  # (its own doc comment already invites this: "a program that ... needs
  # ... should write its own small memoizing class following this same
  # two-line pattern"). Each Thread-spawned gremlin worker gets its own
  # independent VM, so @@instance is independently nil the first time
  # each worker's own dispatch shim runs, and independently set from then
  # on -- see build_router()'s own call site (README's "Rack integration")
  # for why this, not a capturing closure, is what a bare top-level
  # dispatch shim needs to reach a stateful Router instance safely.
  class RouterHolder
    def self.get(builder: Callable[0])
      if @@instance == nil
        @@instance = builder()
      end
      @@instance
    end
  end

end
