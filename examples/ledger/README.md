# examples/ledger

Three months of personal finances in a double-entry ledger: every entry
moves money between accounts and must balance to the cent. The bookkeeping
is small; the point is a tour of Diamond's object model.

```text
$ diamond ledger.di
opened 10 accounts
power bill $187.00 split: $62.34 + $62.33 + $62.33 = $187.00
16 entries posted

Trial balance
-------------
asset: checking            $7,287.66
asset: savings             $8,028.00
liability: credit card       $246.09
...
unbalanced entry: UnbalancedEntry: 'Typo' is off by $90.00
unknown account: UnknownAccount: no account named yacht
mixed currencies: CurrencyMismatch: cannot combine USD with EUR
unknown method: NoMethodError: undefined method 'checking_total' for Ledger
ledger frozen? true, money frozen? true
posting after close: FrozenError: frozen object cannot be modified
```

`expected.txt` has the complete output.

## What it shows

`lib/money.di`, a value type:

- **Operator overloading.** `Money` defines `+`, `-`, `*`, and `negate`
  (unary minus), so amounts read as arithmetic: `dinner + dinner * 0.2`.
- **`<=>` and `Comparable`.** One `<=>` gives `<`, `>`, `between?`, and
  `clamp`, and lets the reports order amounts with `sort_by` and `max_by`.
  `==` is overridden so comparing with a non-`Money` answers `false`
  instead of raising.
- **Visibility.** `cents` is `protected`: another `Money` can read it (which
  `+` and `<=>` need) but outside code can't. `check_currency` and `group`
  are `private`.
- **Immutability.** `initialize` ends with `self.freeze()`, so every `Money`
  is frozen from birth.
- **A singleton constructor.** `Money.parse` destructures
  `digits.split(".")` with `[whole, *fraction] = ...`.

`lib/accounts.di` and `lib/ledger.di`, the model:

- **A sealed hierarchy.** `Account` is `sealed` with five kinds.
  `normal_side` matches all five; drop one and the program doesn't compile.
  A class variable, `@@opened`, counts accounts across every subclass.
- **Structs.** `Posting` and `Entry` are `struct`s with typed fields
  (`postings: Array[Posting]`) and a few added methods.
- **Blocks as builders.** `ledger.post(date, memo) do |entry| ... end` takes
  `&build` and calls `yield(builder)`. `record` is an `alias_method` of
  `post`.
- **Enumerable and operators on a collection.** `Ledger` defines `each` and
  includes `Enumerable`, which the reports use (`group_by`, `flat_map`).
  `ledger["checking"]` is a user-defined `[]`. `length` comes from
  `delegate length(), to: @entries`.
- **`method_missing`.** `ledger.credit_card_balance()` is answered by
  `method_missing` as `ledger.balance("credit card")`; any other unknown
  method still raises `NoMethodError`. The demo reaches it dynamically with
  `ledger.public_send("#{name}_balance")`.
- **`freeze`.** `close` freezes the entries Array and the ledger. A later
  `post` raises `FrozenError` from the frozen Array's `push`.

`lib/reports.di`, the output:

- **A structural interface.** `interface Report` asks for `title` and
  `rows`. The three report classes share no base class and declare nothing;
  having both methods, with `-> String` and `-> Array` annotations, is what
  makes them `Report`s. Passing an object without them to `print_report`
  raises `expected Report` at the call.
- **Time.** Entries keep an ISO date; `Entry#time` parses it with
  `Time.parse`, and the reports format it with `strftime`.

And in `ledger.di`, a `rescue error: A | B | C | ...` union catches each
failure the demo provokes.

## Test

```sh
bash smoke_test.sh
```

This runs the demo under the interpreter and as a `diamond build` binary and
compares both with `expected.txt`.
