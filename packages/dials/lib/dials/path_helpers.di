module Dials

  # Turns a route table (Router#routes()) into Diamond source defining
  # one Rails-style path helper function per named route -- the offline
  # codegen counterpart to Router's own runtime dispatch, needed because
  # Diamond's compile_method/define_method only attach instance methods
  # (docs/design.md's own scoping note on runtime method synthesis) and
  # there is no self.method_missing, so a genuine top-level `skin_path
  # (id)` call site can't be synthesized at runtime the way Rails does
  # it. A consuming app runs this offline (see skindicate.dia's own
  # generate_route_helpers.di for the driver script shape) and commits
  # the result -- the same "generate, review the diff, commit" workflow
  # as any other checked-in generated code, not a gitignored build
  # artifact, since producing it needs the app's own boot chain fully
  # loaded (to call its real build_router()), which would be a circular
  # dependency if the generated file itself were required
  # unconditionally by that same boot chain before it exists.
  class PathHelpers

    # One named route -> one line of Diamond source defining
    # "#{name}_path(...)". Every ":name" segment becomes a positional
    # parameter, spliced back into the path template in order (built via
    # the "\#{...}" escape -- a literal "#{" in *this* source, not an
    # interpolation of it, so the generated line contains real Diamond
    # interpolation syntax for the *next* compile pass to act on); every
    # generated function also takes an optional trailing `query: Hash`
    # for a "?key=value&..." suffix (#query_suffix). The root route
    # ("/" -> a single "" segment) is special-cased -- joining a lone ""
    # segment with "/" produces "", not "/".
    def self.function_source(route: Hash)
      name = route["name"]
      segments = route["segments"]
      params = []
      template_parts = []
      index = 0
      while index < segments.length()
        segment = segments[index]
        if segment.length() > 0 && segment[0] == ":"
          param_name = segment.slice(1, segment.length() - 1)
          params.push(param_name)
          template_parts.push("\#{" + param_name + "}")
        else
          template_parts.push(segment)
        end
        index += 1
      end
      path_template = template_parts.join("/")
      if path_template == ""
        path_template = "/"
      end
      arg_list = params.concat(["query: Hash = {}"]).join(", ")
      "def #{name}_path(#{arg_list}) = " + "\"" + path_template + "\"" + " + Dials::PathHelpers.query_suffix(query)"
    end

    # Groups `routes` (Router#routes()) by name, skipping unnamed ones,
    # and emits exactly one function per unique name -- first
    # registration wins, so two verbs sharing one path and name (a GET
    # show + POST update on the same "/skins/:id", say) correctly
    # collapse onto a single generated helper instead of a duplicate-def
    # compile error.
    def self.generate_source(routes: Array)
      seen = {}
      lines = ["# GENERATED FILE -- do not hand-edit; regenerate via", "# generate_route_helpers.di (see compile_routes.sh).", ""]
      index = 0
      while index < routes.length()
        route = routes[index]
        name = route["name"]
        if name != nil && seen[name] == nil
          seen[name] = true
          lines.push(PathHelpers.function_source(route))
        end
        index += 1
      end
      lines.join("\n") + "\n"
    end

    # "" for an empty Hash, otherwise "?key=value&..." with every key
    # and value percent-encoded via url_encode -- shared by every
    # generated "#{name}_path" function's own optional `query:` kwarg.
    def self.query_suffix(query: Hash = {})
      if query.length() == 0
        return ""
      end
      parts = []
      index = 0
      while index < query.length()
        key = query.key_at(index)
        value = query.value_at(index)
        parts.push("#{url_encode("#{key}")}=#{url_encode("#{value}")}")
        index += 1
      end
      "?" + parts.join("&")
    end
  end

end
