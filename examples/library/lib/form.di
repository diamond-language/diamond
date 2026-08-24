# URL-decoding/parsing only -- rendering moved to views/author_form.html.div
# and views/book_form.html.div (see README's "Views" section for why the
# old generic Form.render went away rather than moving as-is).
class Form
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
        values[Form.decode(pair.slice(0, equals))] = Form.decode(pair.slice(equals + 1, pair.length()))
      end
    end
    values
  end
end
