# ledger: a double-entry bookkeeping demo, and a tour of Diamond's object
# model -- operator overloading, Comparable, protected and private methods,
# sealed classes, structs, interfaces, method_missing, freeze, and more.
#
#   diamond ledger.di
require "./lib/reports"

ledger = Ledger.new()
[
  Asset.new("checking"),
  Asset.new("savings"),
  Liability.new("credit card"),
  Equity.new("opening balance"),
  Income.new("salary"),
  Income.new("interest"),
  Expense.new("rent"),
  Expense.new("groceries"),
  Expense.new("utilities"),
  Expense.new("dining"),
].each() do |account| ledger.open(account) end
puts("opened #{Account.opened()} accounts")

ledger.post("2026-07-01", "Opening balance") do |entry|
  entry.debit("checking", usd("1500.00"))
  entry.debit("savings", usd("8000.00"))
  entry.credit("opening balance", usd("9500.00"))
end

# Three months of ordinary activity. `record` is an alias of `post`.
["07", "08", "09"].each() do |month|
  ledger.record("2026-#{month}-01", "Paycheck") do |entry|
    entry.debit("checking", usd("4200.00"))
    entry.credit("salary", usd("4200.00"))
  end
  ledger.post("2026-#{month}-02", "Rent") do |entry|
    entry.debit("rent", usd("1850.00"))
    entry.credit("checking", usd("1850.00"))
  end
  ledger.post("2026-#{month}-15", "Groceries") do |entry|
    amount = usd("412.37") + usd("35.10") * (month.to_i() - 7)
    entry.debit("groceries", amount)
    entry.credit("credit card", amount)
  end
  ledger.post("2026-#{month}-28", "Card payment") do |entry|
    entry.debit("credit card", usd("400.00"))
    entry.credit("checking", usd("400.00"))
  end
end

# A shared bill split three ways without losing a cent.
power_bill = usd("187.00")
shares = power_bill.allocate([1, 1, 1])
puts("power bill #{power_bill} split: #{shares.join(" + ")} = #{shares.reduce(Money.zero()) do |a, b| a + b end}")
ledger.post("2026-08-20", "Power bill (my share)") do |entry|
  entry.debit("utilities", shares[0])
  entry.credit("checking", shares[0])
end

# Dinner and a 20% tip, rounded to the cent by Money#*.
dinner = usd("86.40")
ledger.post("2026-09-12", "Dinner out") do |entry|
  entry.debit("dining", dinner + dinner * 0.2)
  entry.credit("credit card", dinner + dinner * 0.2)
end

# Savings interest at 0.35% for the quarter.
ledger.post("2026-09-30", "Interest") do |entry|
  interest = ledger.savings_balance() * 0.0035
  entry.debit("savings", interest)
  entry.credit("interest", interest)
end
puts("#{ledger.length()} entries posted")
puts("")

# Reports satisfy the Report interface structurally; public_send picks a
# balance by a name computed at runtime.
[TrialBalance.new(ledger), MonthlyNet.new(ledger), TopExpenses.new(ledger)].each() do |report|
  print_report(report)
end

["checking", "credit_card"].each() do |name|
  method = "#{name}_balance"
  puts("#{method}: #{ledger.public_send(method)}")
end
puts("net worth: #{ledger.balance("checking") + ledger.balance("savings") - ledger.balance("credit card")}")
richest = ledger.accounts().select() do |account| account is Asset end.max_by() do |account|
  ledger.balance(account.name())
end
puts("largest asset: #{richest}")
puts("")

# Everything below is rejected, each with its own exception.
def attempt(label: String, &action)
  begin
    yield()
    puts("#{label}: ok")
  rescue error: UnbalancedEntry | UnknownAccount | CurrencyMismatch | FrozenError | NoMethodError
    # Errors raised by the runtime append their source location on a
    # second line; the first line is enough here.
    puts("#{label}: #{error.class()}: #{error.message().split("\n")[0]}")
  end
end

attempt("unbalanced entry") do
  ledger.post("2026-09-30", "Typo") do |entry|
    entry.debit("rent", usd("100.00"))
    entry.credit("checking", usd("10.00"))
  end
end
attempt("unknown account") do
  ledger.post("2026-09-30", "Mystery") do |entry|
    entry.debit("yacht", usd("1.00"))
  end
end
attempt("mixed currencies") do
  usd("5.00") + Money.new(500, "EUR")
end
attempt("unknown method") do
  ledger.checking_total()
end

ledger.close()
puts("ledger frozen? #{ledger.frozen?()}, money frozen? #{usd("1.00").frozen?()}")
attempt("posting after close") do
  ledger.post("2026-10-01", "Paycheck") do |entry|
    entry.debit("checking", usd("4200.00"))
    entry.credit("salary", usd("4200.00"))
  end
end
puts("still #{ledger.length()} entries")

exit(0)
