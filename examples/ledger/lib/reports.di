# Reports share no base class. Anything with `title` and `rows` satisfies
# the Report interface structurally, and print_report accepts it.
require "./ledger"

interface Report
  def title() -> String
  def rows() -> Array
end

# Prints any Report: its title underlined with dashes, then each [label,
# amount] row with the label left-aligned and the amount right-aligned.
def print_report(report: Report)
  puts(report.title())
  puts("-".repeat(report.title().length()))
  report.rows().each() do |row|
    # A row is a two-element array; destructure it.
    [label, amount] = row
    puts("#{label.ljust(24, " ")}#{amount.to_s().rjust(12, " ")}")
  end
  puts("")
end

# Every account's balance on its normal side, grouped by kind.
class TrialBalance
  def initialize(ledger: Ledger)
    @ledger = ledger
  end
  def title() -> String = "Trial balance"
  def rows() -> Array
    # By kind in balance-sheet order, then by name. (Sorting on an array of
    # [position-of-kind, name] compares position first, then name.)
    kinds = ["asset", "liability", "equity", "income", "expense"]
    accounts = @ledger.accounts().sort_by() do |account|
      [kinds.index_of(account.kind()), account.name()]
    end
    accounts.map() do |account|
      ["#{account.kind()}: #{account.name()}", @ledger.balance(account.name())]
    end
  end
end

# Income minus expenses, month by month.
class MonthlyNet
  def initialize(ledger: Ledger)
    @ledger = ledger
  end
  def title() -> String = "Net income by month"
  def rows() -> Array
    # Group entries by "YYYY-MM"; Ledger gets group_by from Enumerable.
    by_month = @ledger.group_by() do |entry| entry.time().strftime("%Y-%m") end
    by_month.keys().sort().map() do |month|
      net = Money.zero()
      by_month[month].each() do |entry|
        entry.postings().each() do |posting|
          account = posting.account()

          # Postings are debit-positive, but income is a credit (negative)
          # and expenses a debit (positive), so subtracting turns income into
          # a gain and expenses into a loss.
          if account is Income || account is Expense
            net = net - posting.amount()
          end
        end
      end
      [month, net]
    end
  end
end

# The five largest expense postings, largest first.
class TopExpenses
  def initialize(ledger: Ledger)
    @ledger = ledger
  end
  def title() -> String = "Largest expenses"
  def rows() -> Array
    # One [label, amount] pair per expense posting across all entries:
    # flat_map joins each entry's list of pairs into one list.
    expenses = @ledger.flat_map() do |entry|
      spent = entry.postings().select() do |posting| posting.account() is Expense end
      spent.map() do |posting|
        ["#{entry.time().strftime("%b %d")} #{entry.memo()}", posting.amount()]
      end
    end

    # Sort by amount (Money is Comparable), biggest first, keep five.
    expenses.sort_by() do |row| row[1] end.reverse().take(5)
  end
end
