module ActiveDiscussion

  # The fixed vocabulary of flame-signal report reasons -- ported from
  # ActiveDiscussion's own Signal module. This is the one moderation
  # primitive kept from the Ruby original (see README.md): a lightweight
  # per-discussion report/flag, not the separate case/vote tribunal or
  # forum-level permission scheme the architecture review found
  # duplicating this in the wider MaquinasStack, neither of which are
  # ported.
  module Signal
    SPAM = "spam"
    ABUSE = "abuse"
    FLAME = "flame"
    REPORT = "report"

    def self.all() = [SPAM, ABUSE, FLAME, REPORT]
    def self.valid?(type: String) -> Bool = Signal.all().include?(type)
  end

end
