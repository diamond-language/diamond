# Replaces what the rules find. In tag mode the same value always gets the
# same tag, so a redacted log still shows that two lines came from one
# user: the first email seen becomes [email-1], the next new one
# [email-2], and a repeat of the first is [email-1] again. Mask mode
# keeps only the shape: every letter and digit becomes *, except a card's
# last four digits.
class Tagger
  attr_reader counts: Hash[String, Int]

  def initialize(mask: Bool)
    @mask = mask
    @tags = {}
    @counts = {}
  end

  # The replacement text for one match. Every match counts toward the
  # summary, even repeats.
  def replace(rule: Rule, found: String) -> String
    @counts[rule.name()] = @counts.fetch(rule.name(), 0) + 1
    return masked(rule, found) if @mask

    # Tag mode: a value seen before reuses its tag; a new value gets the next
    # number for its kind. The key combines rule and value, so the same text
    # matched by two different rules is tagged separately.
    key = "#{rule.name()}:#{found}"
    unless @tags.include_key?(key)
      @tags[key] = "[#{rule.name()}-#{self.seen(rule.name()) + 1}]"
    end
    @tags[key]
  end

  # How many different values of this kind have been tagged so far.
  def seen(name: String) -> Int
    @tags.keys().count() do |key| key.start_with?("#{name}:") end
  end

  # Mask mode: letters and digits become `*`; punctuation stays, so the
  # shape of the value ("a**@***.com") is still visible. For a card, the
  # last four digits are kept (the usual convention for confirming which
  # card without revealing it).
  def masked(rule: Rule, found: String) -> String
    stars = found.gsub(Regexp.new("[A-Za-z0-9]"), "*")
    return stars unless rule.name() == "card"
    stars.slice(0, stars.length() - 4) + found.slice(found.length() - 4, 4)
  end

  # Applies every rule to a line, in order. `gsub` with a block calls the
  # block for each match and substitutes what it returns: the replacement
  # if the rule's check (when it has one) passes, or the original text
  # unchanged if the check fails (a 16-digit number that is not a valid
  # card is left alone).
  def redact(line: String, rules: Array[Rule]) -> String
    rules.reduce(line) do |text, rule|
      text.gsub(rule.pattern()) do |found|
        check = rule.check()
        if check == nil || check(found)
          self.replace(rule, found)
        else
          found
        end
      end
    end
  end
end
