# template: render a Mustache-ish template against a JSON context.
#
# {{name}}             HTML-escaped variable, dotted path into the context
# {{{name}}}           the same, unescaped
# {{#name}}...{{/name}}   section: repeats once per element if the value
#                         is an Array, once (with the value as the new
#                         context, if it's a Hash) if any other truthy
#                         value, or not at all if false/nil/an empty Array
# {{^name}}...{{/name}}   inverted section: renders only when {{#name}}
#                         wouldn't
# {{!comment}}         dropped
#
# What this shows: a one-pass parser (lib/parser.di) that builds the
# final Node tree directly, without a separate flat-token pass first --
# each open section is a frame pushed onto a stack, holding its own
# body-so-far, until its matching close tag pops it back into its
# parent's body; a mismatched or missing close is a TemplateError naming
# the position. Rendering (lib/render.di) is then a small, sealed-class
# case split, recursing through render_section back into render_nodes
# for however deep the sections nest.

require "./lib/nodes"
require "./lib/parser"
require "./lib/render"

def usage() -> Int
  warn("usage: template.di TEMPLATE_FILE CONTEXT_FILE.json")
  64
end

def main(argv) -> Int
  return usage() unless argv.length() == 2

  # Read both files, parse the template, render it against the JSON context.
  # `print`, not `puts`: the output is the template's own text, newlines
  # included. Bad JSON or a bad template is exit 65; an unreadable file, 66.
  begin
    text = File.open(argv[0], "r").read()
    context = JSON.parse(File.open(argv[1], "r").read())
    print(render_nodes(template_parse(text), context))
    0
  rescue error: IOError
    warn(error.message())
    66
  rescue error: JSONError
    warn("#{argv[1]}: #{error.message()}")
    65
  rescue error: TemplateError
    warn("#{argv[0]}:#{error.position()}: #{error.message()}")
    65
  end
end

exit(main(ARGV))
