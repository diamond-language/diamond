# Inline formatting: `code`, **strong**, *emphasis*, [links](url), and HTML
# escaping. One Inline is built per document, so each Regexp is compiled
# once rather than once per line.

# Turns one line of Markdown text into HTML.
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

  # Makes text safe to put inside HTML. The `special` check is a fast path:
  # most text has nothing to escape, and gsub four times is wasted work then.
  def escape(text: String) -> String
    return text unless @special.match?(text)
    @entities.reduce(text) do |escaped, rule| escaped.gsub(rule[0], rule[1]) end
  end

  # Splitting on backticks alternates plain text and code: segments at odd
  # positions were between a pair of backticks. Code is escaped but never
  # formatted, so `**not bold**` stays literal.
  def render(text: String) -> String
    segments = text.split("`")

    # An even number of segments means an odd number of backticks, so the last
    # one has no partner.
    if segments.length() % 2 == 0
      # An unpaired backtick: keep it as a literal character.
      last = segments.pop()
      segments[segments.length() - 1] = "#{segments.last()}`#{last}"
    end

    # Render each segment: code (odd) escaped only; text (even) escaped, THEN
    # formatted, so the < and > of any HTML typed by the author are neutralized
    # before the tags this renderer adds are put in.
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

  # Bold before italic: `**x**` must be taken as strong before the
  # single-star rule can see its inner `*`s.
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

    # Find the first link in what is left, emit the text before it and then
    # the link, and continue after it.
    loop do
      found = @link.match(rest)
      break if found == nil
      [whole, label, url] = found
      at = rest.index_of(whole)

      # `javascript:` URLs would run script when clicked, so they are
      # replaced by "#". (The text was already escaped, so a quote cannot
      # break out of the href attribute.)
      target = if @unsafe_url.match?(url) then "#" else url end
      html.append(rest.slice(0, at)).append("<a href=\"#{target}\">#{label}</a>")
      rest = rest.slice(at + whole.length(), rest.length())
    end
    html.append(rest).to_s()
  end
end
