module Dials

# Request query-string/form-body parsing -- protocol-generic, no
# knowledge of routes or controllers. Dials::Router.dispatch calls
# Params.parse itself and merges the result with captured path segments;
# call it directly too if a handler wants query/body params without
# going through the router at all.
class Params
  def self.decode(value: String) -> String
    result = value
    plus = result.index_of("+")
    while plus != nil
      result = result.slice(0, plus) + " " + result.slice(plus + 1, result.length())
      plus = result.index_of("+")
    end
    result
  end

  def self.parse(request)
    values = {}
    source = request["body"]
    if request["method"] == "GET"
      source = request["path"]
      question = source.index_of("?")
      source = if question == nil then "" else source.slice(question + 1, source.length()) end
    end
    source.split("&").each() do |pair|
      equals = pair.index_of("=")
      if equals != nil
        values[Params.decode(pair.slice(0, equals))] = Params.decode(pair.slice(equals + 1, pair.length()))
      end
    end
    values
  end
end

end
