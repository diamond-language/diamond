# Lets a resolver check what its own result's sub-selections actually
# are, *before* doing expensive work to produce that result -- the
# classic use case is skipping an eager-load a query didn't ask for
# (an `author` resolver checking whether `books` was selected before
# joining the books table at all). execution/executor.di sets
# `context["lookahead"]` to one of these immediately before every field
# resolution (see its own header comment on why a shared, mutated-in-
# place context entry is safe here, not a fresh Hash per field).
#
# Deliberately simple relative to graphql-ruby's own Lookahead: doesn't
# filter by a selection's fragment type condition (an inline fragment
# or named fragment spread's own sub-selections are always considered,
# regardless of `... on SomeType`) -- the common case this exists for
# (deciding whether to eager-load an association) almost always
# involves a single concrete object type, where there's no type
# condition to filter by in the first place. A selection nested only
# under `... on OtherType` on a genuinely polymorphic field will show
# up as "selected" here even if the runtime type turns out not to
# match -- a real, documented over-approximation, not a silent gap
# (see ROADMAP.md).
module GraphQL
module Execution

class Lookahead
  def initialize(selection_set, fragments, coerced_variables)
    @selection_set = selection_set
    @fragments = fragments
    @coerced_variables = coerced_variables
  end

  # Every Field AST node reachable from this level, after expanding
  # fragment spreads/inline fragments and honoring @include/@skip --
  # the shared basis every other method here filters/maps over.
  def all_fields()
    result = []
    self.collect_fields(@selection_set, [], result)
    result
  end

  def collect_fields(selection_set, visited_fragments, result)
    index = 0
    while index < selection_set.length()
      selection = selection_set[index]
      case selection
      when GraphQL::Language::Field
        if GraphQL::Execution::Directives.included?(selection.directives(), @coerced_variables)
          result.push(selection)
        end
      when GraphQL::Language::FragmentSpread
        if GraphQL::Execution::Directives.included?(selection.directives(), @coerced_variables) &&
           !visited_fragments.include?(selection.name())
          visited_fragments.push(selection.name())
          fragment = @fragments[selection.name()]
          unless fragment == nil
            self.collect_fields(fragment.selection_set(), visited_fragments, result)
          end
        end
      when GraphQL::Language::InlineFragment
        if GraphQL::Execution::Directives.included?(selection.directives(), @coerced_variables)
          self.collect_fields(selection.selection_set(), visited_fragments, result)
        end
      end
      index += 1
    end
  end

  private collect_fields

  # Every Field AST node at this level named `field_name` (almost
  # always 0 or 1, more than 1 only when fragments merge into the same
  # field with different sub-selections -- #selection below merges
  # those the same way execution/executor.di's own
  # #merged_selection_set does).
  def matching_fields(field_name)
    fields = self.all_fields()
    result = []
    index = 0
    while index < fields.length()
      if fields[index].name() == field_name
        result.push(fields[index])
      end
      index += 1
    end
    result
  end

  # True if `field_name` is selected anywhere at this level (by its
  # real field name, not a response alias -- "was `books` requested",
  # not "was it requested as `myBooks`").
  def selects?(field_name) = self.matching_fields(field_name).length() > 0

  # A Lookahead scoped to `field_name`'s own sub-selections -- empty
  # (so every #selects? underneath it is false) when `field_name`
  # wasn't selected at all, matching graphql-ruby's own NullLookahead
  # behavior without a separate class for it.
  def selection(field_name)
    matches = self.matching_fields(field_name)
    merged = []
    index = 0
    while index < matches.length()
      selection_set = matches[index].selection_set()
      unless selection_set == nil
        merged = merged.concat(selection_set)
      end
      index += 1
    end
    GraphQL::Execution::Lookahead.new(merged, @fragments, @coerced_variables)
  end

  # Every distinct field name selected at this level, first-seen order
  # -- for a resolver that wants to see everything at once rather than
  # asking #selects? field by field.
  def selections()
    fields = self.all_fields()
    result = []
    index = 0
    while index < fields.length()
      name = fields[index].name()
      unless result.include?(name)
        result.push(name)
      end
      index += 1
    end
    result
  end
end

end
end
