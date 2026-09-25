# Inline formatting: `code`, **strong**, *emphasis*, [links](url), and HTML
# escaping. One Inline is built per document, so each Regexp is compiled
# once rather than once per line.

class Inline
  def initialize()
    # The text inside the markers must start and end with a non-space, so
    # arithmetic like 5 * 3 * 2 is left alone.
    @strong = Regexp.new("\\*\\*(\\S(?:.*?\\S)?)\\*\\*")
    @emphasis = Regexp.new("\\*(\\S(?:.*?\\S)?)\\*")
    @link = Regexp.new("\\[([^\\]]+)\\]\\(([^)\\s]+)\\)")
    @unsafe_url = Regexp.new("^\\s*javascript:", 1)   # 1 = ignore case
    @special = Regexp.new("[&<>\"]")
    # & first, so the entities added by later rules aren't escaped again.
    @entities = [["&", "&amp;"], ["<", "&lt;"], [">", "&gt;"], ["\"", "&quot;"]].map() do |pair|
      [Regexp.new(pair[0]), pair[1]]
    end
  end

  def escape(text: String) -> String
    return text unless @special.match?(text)
    @entities.reduce(text) do |escaped, rule| escaped.gsub(rule[0], rule[1]) end
  end

  # Splitting on backticks alternates plain text and code: segments at odd
  # positions were between a pair of backticks. Code is escaped but never
  # formatted, so `**not bold**` stays literal.
  def render(text: String) -> String
    segments = text.split("`")
    if segments.length() % 2 == 0
      # An unpaired backtick: keep it as a literal character.
      last = segments.pop()
      segments[segments.length() - 1] = "#{segments.last()}`#{last}"
    end
    html = StringBuilder.new()
    segments.each_with_index() do |segment, index|
      if index % 2 == 1
        html.append("<code>#{self.escape(segment)}</code>")
      else
        html.append(self.format(self.escape(segment)))
      end
    end
    html.to_s()
  end

  private

  def format(text: String) -> String
    text = text.gsub(@strong, "<strong>\\1</strong>")
    text = text.gsub(@emphasis, "<em>\\1</em>")
    self.links(text)
  end

  # Links need a decision per match (javascript: URLs become "#"), so walk
  # the matches one at a time instead of using a gsub template.
  def links(text: String) -> String
    html = StringBuilder.new()
    rest = text
    loop do
      found = @link.match(rest)
      break if found == nil
      [whole, label, url] = found
      at = rest.index_of(whole)
      target = if @unsafe_url.match?(url) then "#" else url end
      html.append(rest.slice(0, at)).append("<a href=\"#{target}\">#{label}</a>")
      rest = rest.slice(at + whole.length(), rest.length())
    end
    html.append(rest).to_s()
  end
end
