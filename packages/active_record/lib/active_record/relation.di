module ActiveRecord

# A thin, lazy wrapper over an Arel::Query -- built by Repository#relation
# (packages/active_record/lib/active_record/repository.di). Not constructed
# by hand against an arbitrary query/mapper/visitor the way HasMany/HasOne
# are meant to be, though nothing stops that. Every chain method here
# (#where/#order/#take/#skip) just forwards to the same method
# Arel::Query already has (packages/arel/README.md's own
# "immutable, chainable" query builder) and wraps the new Query it
# returns in a new Relation -- no query-building logic lives here at all.
# Nothing hits the database until #to_a/#first/#count, at which point
# rows are mapped through the same @mapper Repository#all/#where already
# use.
class Relation
  attr_reader query: Arel::Query
  attr_reader mapper: Callable[1]
  attr_reader visitor
  attr_reader repository

  def initialize(query, mapper: Callable[1], visitor = nil, repository = nil)
    @query = query
    @mapper = mapper
    @visitor = visitor
    @repository = repository
  end

  def wrap(query) = Relation.new(query, @mapper, @visitor, @repository)

  # Query#where already accepts a plain Hash and ANDs its keys together
  # via `eq` (see packages/arel/README.md's "Compatibility where"), so
  # there is no predicate-building of this Relation's own -- unlike
  # Repository#where, which still hand-rolls that loop for its own
  # eager, non-Relation callers.
  def where(conditions) = self.wrap(@query.where(conditions))
  def order(column_or_columns) = self.wrap(@query.order(column_or_columns))
  def take(n: Int) = self.wrap(@query.take(n))
  def limit(n: Int) = self.take(n)
  def skip(n: Int) = self.wrap(@query.skip(n))
  def offset(n: Int) = self.skip(n)

  # Explicit projection, matching Arel::Query#select. Callers pass an Array
  # of column names/Arel expressions; the returned Relation is independent
  # and remains lazy. `reselect` is the Rails spelling for replacing an
  # existing projection and is intentionally identical here because Arel's
  # own #select already replaces rather than appends.
  def select(columns) = self.wrap(@query.select(columns))
  def reselect(columns) = self.select(columns)

  def to_a(db)
    rows = @query.to_a(db, @visitor)
    mapped = []
    index = 0
    while index < rows.length()
      mapped.push(@mapper(rows[index]))
      index += 1
    end
    mapped
  end

  # Deliberately does not add an implicit `ORDER BY` (e.g. by primary
  # key) the way real ActiveRecord's #first does -- this package has no
  # schema inspection or naming convention to build one from (see
  # README.md), so #first without a preceding #order returns whatever row
  # the database happens to return first. Chain #order(...) first for a
  # deterministic result.
  def first(db)
    rows = self.take(1).to_a(db)
    if rows.length() == 0 then nil else rows[0] end
  end

  def count(db) = @query.count(db, @visitor)
end

end
