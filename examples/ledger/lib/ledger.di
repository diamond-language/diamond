# A double-entry ledger. Every entry's postings must sum to zero: money
# moves from one account to another, never appears or vanishes.
require "./money"
require "./accounts"

class UnbalancedEntry < StandardError
end

class UnknownAccount < StandardError
end

# One line of an entry. A positive amount is a debit, a negative one a
# credit.
# Posting amounts are signed so that an entry's postings can simply be
# summed: debits positive, credits negative, a balanced entry totals zero.
struct Posting(account: Account, amount: Money)
end

# One dated, described journal entry and its postings.
struct Entry(date: String, memo: String, postings: Array[Posting])
  # Time is a native value with no type name to annotate a field with, so
  # the entry keeps its ISO date and parses it on demand.
  def time() = Time.parse("#{@date}T00:00:00Z")
  # Sum of the postings; zero for a balanced entry.
  def total() -> Money
    @postings.reduce(Money.zero()) do |sum, posting| sum + posting.amount() end
  end
end

# Handed to the block given to Ledger#post; collects postings by name.
class EntryBuilder
  def initialize(ledger: Ledger)
    @ledger = ledger
    @postings = []
  end

  # Account names are looked up immediately (an unknown one raises right here,
  # inside the block). A credit is stored as a NEGATIVE amount (`-amount` is
  # Money's unary minus).
  def debit(name: String, amount: Money)
    @postings.push(Posting.new(@ledger[name], amount))
  end

  def credit(name: String, amount: Money)
    @postings.push(Posting.new(@ledger[name], -amount))
  end
  def postings() -> Array = @postings
end

class Ledger
  include Enumerable

  # `delegate` generates `length()` as a call to the same method on @entries.
  delegate length(), to: @entries

  def initialize()
    @accounts = {}
    @entries = []
  end

  # Registers an account under its name (replacing any account of that name).
  def open(account: Account) -> Account
    @accounts[account.name()] = account
  end

  def accounts() -> Array = @accounts.values()

  # `ledger["checking"]`: the account, or UnknownAccount.
  def [](name: String) -> Account
    account = @accounts[name]
    raise UnknownAccount.new("no account named #{name}") if account == nil
    account
  end

  # ledger.post("2026-09-01", "Rent") do |entry|
  #   entry.debit("rent", usd("1200"))
  #   entry.credit("checking", usd("1200"))
  # end
  def post(date: String, memo: String, &build) -> Entry
    # Run the caller's block, which fills the builder with postings.
    builder = EntryBuilder.new(self)
    yield(builder)

    # Validate BEFORE storing: a bad date or an unbalanced entry raises, and
    # nothing is added to the ledger.
    entry = Entry.new(date, memo, builder.postings())
    entry.time()   # rejects an impossible date such as 2026-02-30
    unless entry.total().zero?()
      raise UnbalancedEntry.new("'#{memo}' is off by #{entry.total()}")
    end
    @entries.push(entry)
    entry
  end
  # `record` is another name for `post`.
  alias_method record, post

  # Enumerable's select/group_by/sort_by/... all come from this.
  def each(callback)
    @entries.each(callback)
    self
  end

  # The balance on the account's normal side, so a healthy asset or
  # income account reads as positive.
  def balance(name: String) -> Money
    account = self[name]
    net = Money.zero()

    # Add up every posting to this account (debits positive, credits
    # negative), then flip the sign for accounts that grow on the credit side.
    @entries.each() do |entry|
      entry.postings().each() do |posting|
        net = net + posting.amount() if posting.account() == account
      end
    end
    if normal_side(account) == :debit then net else -net end
  end

  # ledger.checking_balance() is ledger.balance("checking"), and
  # ledger.credit_card_balance() is ledger.balance("credit card"). Any
  # other unknown method is still an error.
  def method_missing(name, args)
    # `slice(0, length - 8)` drops the "_balance" suffix (8 characters), and
    # `tr` turns the remaining underscores back into spaces.
    text = "#{name}"
    if text.end_with?("_balance") && args.empty?()
      return self.balance(text.slice(0, text.length() - 8).tr("_", " "))
    end
    raise NoMethodError.new("undefined method '#{name}' for Ledger")
  end

  # Closing the books freezes the entries and the ledger itself; any later
  # post raises FrozenError.
  def close() -> Ledger
    @entries.freeze()
    self.freeze()
  end
end
