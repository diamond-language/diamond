# Classes and structs

Everything in D6 is an object, and you define your own kinds of object with
`class`. For plain bundles of data, `struct` writes the class for you.

## A class

```ruby
class Account
  attr_reader owner: String
  attr_reader balance: Int

  def initialize(owner: String, balance: Int = 0)
    @owner = owner
    @balance = balance
  end

  def deposit(amount: Int) -> Account
    @balance += amount
    self
  end

  def to_s() = "#{@owner}: #{@balance}"
end

account = Account.new("Ada")
account.deposit(50).deposit(25)
puts(account.balance())   # => 75
puts(account)             # => Ada: 75
```

The pieces:

- `ClassName.new(...)` creates an object and passes its arguments to
  `initialize`.
- `@name` is an instance variable, a field of the object. Fields are private:
  code outside the class reads them through methods.
- `attr_reader name: Type` writes a reader method for you, so
  `account.balance()` returns `@balance`. `attr_writer` writes a setter and
  `attr_accessor` both. Attributes always state their type.
- `self` is the object a method was called on. Returning it, as `deposit`
  does, lets calls chain.
- `to_s` is what `puts` and string interpolation use to show the object.

With an `attr_accessor` (or any method named `name=`), assignment syntax
calls the setter:

```ruby
class Label
  attr_accessor text: String
  def initialize(text: String)
    @text = text
  end
end

label = Label.new("draft")
label.text = "final"
puts(label.text())   # => final
```

Inside a method, you can call the object's other methods without `self.`.

## Class methods

`def self.name` defines a method you call on the class itself, typically a
constructor with a meaningful name:

```ruby
class Temperature
  attr_reader degrees: Float
  def initialize(degrees: Float)
    @degrees = degrees
  end
  def self.freezing() -> Temperature = Temperature.new(0.0)
end

puts(Temperature.freezing().degrees())   # => 0.0
```

## Private methods

Methods after `private` can only be called from inside the class, without a
receiver:

```ruby
class Report
  def initialize(rows: Array)
    @rows = rows
  end

  def summary() = "#{@rows.length()} rows, #{format_total()}"

  private

  def format_total() = "total #{@rows.sum()}"
end

puts(Report.new([3, 4]).summary())   # => 2 rows, total 7
```

Calling `report.format_total()` from outside raises an error saying the
method is private.

## Inheritance

A class can extend one other class with `<`. `super(...)` calls the parent's
version of the current method:

```ruby
class Animal
  attr_reader name: String
  def initialize(name: String)
    @name = name
  end
  def describe() = "#{@name} the animal"
end

class Dog < Animal
  def describe() = super() + ", who barks"
end

puts(Dog.new("Rex").describe())   # => Rex the animal, who barks
```

`value is ClassName` tests whether a value is an instance of a class or one
of its subclasses. It also works with the built-in types:

```ruby
class Animal
end
class Dog < Animal
end

puts(Dog.new() is Animal)   # => true
puts(5 is Int)              # => true
puts("5" is Int)            # => false
```

## Modules

A module groups methods that several classes can share. `include` mixes them
into a class. Inside a module method, call the including object's methods
through `self.`:

```ruby
module Greets
  def greet() = "Hello from #{self.name()}"
end

class Robot
  include Greets
  attr_reader name: String
  def initialize(name: String)
    @name = name
  end
end

puts(Robot.new("R2").greet())   # => Hello from R2
```

## Operators and comparison

Operators are methods, so a class can define `+`, `==`, `<=>` and the rest.
Defining `<=>` and including `Comparable` gives you `<`, `>`, `between?`,
and lets `sort` and `max` work on your objects:

```ruby
class Version
  include Comparable
  attr_reader major: Int
  attr_reader minor: Int

  def initialize(major: Int, minor: Int)
    @major = major
    @minor = minor
  end

  def <=>(other: Version) -> Int
    @major == other.major() ? @minor <=> other.minor() : @major <=> other.major()
  end
  def to_s() = "#{@major}.#{@minor}"
end

versions = [Version.new(1, 10), Version.new(1, 2), Version.new(0, 9)]
puts(versions.max())                          # => 1.10
puts(Version.new(1, 2) < Version.new(1, 10))  # => true
```

## Structs

When a class is mostly data, `struct` declares the fields and generates the
constructor, readers, `==` and `to_s`:

```ruby
struct Point(x: Int, y: Int)
end

a = Point.new(1, 2)
b = Point.new(x: 1, y: 2)
puts(a)        # => Point(x: 1, y: 2)
puts(a.x())    # => 1
puts(a == b)   # => true
```

Two structs are equal when they are the same struct and their fields are
equal, which makes them good keys and good test expectations. A struct body
can add methods like any class:

```ruby
struct Point(x: Int, y: Int)
  def distance_from_origin() -> Float = Math.sqrt((@x * @x + @y * @y).to_f())
end

puts(Point.new(3, 4).distance_from_origin())   # => 5.0
```

A plain class that extends a struct and adds fields of its own gets an `==`
and a `to_s` covering those fields too, unless it writes its own.

## Sealed classes

A `sealed` class lists its variants: it cannot be created directly, only
through its subclasses. In return, the compiler knows the complete set of
subclasses and checks that a `case` over one covers them all:

```ruby
sealed class Shape
end

class Circle < Shape
  attr_reader radius: Float
  def initialize(radius: Float)
    @radius = radius
  end
end

class Square < Shape
  attr_reader side: Float
  def initialize(side: Float)
    @side = side
  end
end

def area(shape: Shape) -> Float
  case shape
  when Circle then 3.14159 * shape.radius() * shape.radius()
  when Square then shape.side() * shape.side()
  end
end

puts(area(Square.new(3.0)))   # => 9.0
```

Delete the `when Square` line and the program no longer compiles:

```text
error: case is not exhaustive over its subject's known closed type; missing: Square; add the missing branch or an 'else'
```

Adding a new subclass later turns every `case` that forgot it into a compile
error, instead of a `nil` at run time.

## Coming from another language

- **Ruby**: attributes are declared with a name and a type
  (`attr_reader name: String`), not with symbols. `obj.is_a?(C)` is written
  `obj is C`. Classes are not objects you can pass around at run time.
- **Python**: there is no `self` parameter; `@name` is `self.name`, and
  `initialize` is `__init__`. Fields are private unless you add readers.
- **JavaScript**: `constructor` is `initialize`, `this.x` is `@x`, and
  `extends` is `<`. A `struct` is a class with value equality, which plain
  JavaScript objects lack.

The [classes and modules reference](../classes-and-modules.md) covers
visibility, class variables, reopening classes, generics and more.

Next: [Types](types.md).
