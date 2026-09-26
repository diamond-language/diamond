# examples/grades

A grade report from CSV lines of `student,course,score`, written to show
what Diamond's gradual type system checks and when. Besides the program
itself, `rejected/` holds four small programs that the compiler refuses to
run, one per kind of mistake it catches.

```text
$ diamond grades.di testdata/scores.csv
Students
  name         mean median grade
  ada          92.3   91.0 A     3 course(s): math, physics, poetry
  edsger       58.5   58.5 F     2 course(s): math, physics
  ...
Courses
  name         mean median grade
  math         82.2   85.0 B     top: ada (98)
  ...
Honor roll: ada, grace
13 scores from 5 students in 3 courses

Rejected
  line 13: score '' is not a whole number
  line 15: score 'one hundred' is not a whole number
  line 16: expected 3 fields, got 4
  line 17: score 103 is over 100
```

`grades FILE...` exits 1 when any line was rejected, 64 with no files, and
66 when a file can't be opened.

## What it shows

- **Generic functions.** `group_by_key[T, K]` and `best_by[T]`
  (`lib/stats.di`) work for any element type; the compiler infers `T` and
  `K` from each call's arguments.
- **A sealed result type.** `parse_score` returns `ParseResult`, which is
  `Parsed` or `Rejected`. `grades.di` matches it with binding patterns
  (`when Parsed{score: score}`), and because the class is sealed, leaving a
  case out doesn't compile.
- **`Nil` in the type.** `best_by` returns `T | Nil`, so
  `CourseReport#detail` must handle `when nil` before using the result.
- **A structural interface.** `StudentReport` and `CourseReport` share no
  base class; both satisfy `Summary` by having `label`, `points`, and
  `detail` with matching return types, and `print_row(row: Summary)` takes
  either.
- **Typed structs and collections.** `Score` is a struct with typed fields;
  parameters such as `values: Array[Int]` are checked element by element
  when the call runs.

### What gets checked when

| Mistake | Caught |
|---|---|
| A call whose argument can't have the right type (`letter("A")`) | compile time |
| A `case` over a sealed class that misses a subclass | compile time |
| Returning `T \| Nil` where the result promises `T` | compile time |
| A method returning the wrong type for its `-> Type` | compile time |
| An argument whose type is only known at run time (e.g. from a file) | when the call runs, as a rescuable `TypeError` |

`rejected/` has one program for each compile-time row;
`smoke_test.sh` checks each fails with its message.

One Diamond-specific detail: `return` inside a `do` block ends that call of
the block, not the enclosing method, so `grades_main` loops over files with
`while` where it may need to `return` an exit code early.

## Test

```sh
bash smoke_test.sh
```

This checks the report under the interpreter and as a `diamond build`
binary, the exit codes, and every program in `rejected/`.
