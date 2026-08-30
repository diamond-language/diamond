# The core content tree: every skin and every comment is an `Entry`
# pointing (polymorphically, via entryable_type/entryable_id) at its
# own small type-specific row (Skin/Comment), the same shape Rails'
# own `delegated_type` gives you -- Diamond has no macro for this, so
# the dispatch in #entryable below is written out explicitly, the same
# "no DSL, an ordinary runtime helper" convention has_many/belongs_to
# already use in packages/active_record.
#
# Tree structure is a materialized path (Ruby's `ancestry` gem shape),
# not a `parent_id`-per-row adjacency list: `ancestry` holds every
# ancestor's id, "/"-joined, root-to-immediate-parent, and is NULL for
# a root entry. `ancestry_depth` is `ancestry` gem's own `:cache_depth`
# column -- the ancestor count, cached so nothing needs to re-split the
# string just to know how deep an entry is.
class Entry < ActiveRecord::Model
  attr_accessor ancestry: String, ancestry_depth, user_id, entryable_type: String, entryable_id, created_at, updated_at

  def initialize(attributes: Hash = {})
    super(attributes)
    @ancestry = attributes["ancestry"]
    @ancestry_depth = if attributes["ancestry_depth"] == nil then 0 else attributes["ancestry_depth"] end
    @user_id = attributes["user_id"]
    @entryable_type = attributes["entryable_type"]
    @entryable_id = attributes["entryable_id"]
    @created_at = attributes["created_at"]
    @updated_at = attributes["updated_at"]
  end

  def to_attributes() = {"ancestry": @ancestry, "ancestry_depth": @ancestry_depth, "user_id": @user_id,
    "entryable_type": @entryable_type, "entryable_id": @entryable_id, "created_at": @created_at, "updated_at": @updated_at}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def root?() -> Bool = @ancestry == nil

  def parent_id()
    if @ancestry == nil then nil else @ancestry.split("/").last().to_i() end
  end

  # The ancestry value *this entry's own children* will carry when
  # created (see create_entry! below). Not currently also used as a
  # LIKE prefix for a deep descendant scan -- with comments capped at
  # one level of replies, #children already covers this app's whole
  # subtree; see the package README-style note in comments_controller.di
  # for why a real multi-level descendant query isn't built yet.
  def entry_path() -> String
    if @ancestry == nil then "#{self.id()}" else "#{@ancestry}/#{self.id()}" end
  end

  def children(db) -> Array
    Entry.where({"ancestry": self.entry_path()}).to_a(db).sort_by() do |e| e.created_at() end
  end

  def entryable(db)
    if @entryable_type == "Skin" then Skin.find(db, @entryable_id)
    elsif @entryable_type == "Comment" then Comment.find(db, @entryable_id)
    else nil end
  end

  # Destroys this entry, its own entryable row, and every descendant
  # entry/entryable beneath it (bottom-up) -- deleting a Skin's own
  # root entry this way takes its whole comment tree with it, the same
  # cascading-subtree-delete behavior active_discussion's own
  # Comment#delete! had.
  def destroy_subtree!(db)
    self.children(db).each() do |child|
      child.destroy_subtree!(db)
    end
    entryable = self.entryable(db)
    if entryable != nil
      entryable.destroy(db)
    end
    self.destroy(db)
  end
end

def build_entry(row) = Entry.new(row)

# The two-step "create the entryable row, then the Entry row pointing
# at it" factory -- mirrors what a real `delegated_type` create does.
# `entryable_type` is a plain String literal the caller passes ("Skin",
# "Comment") -- Diamond has no reflection to derive a class's own name
# from a value (a bare class name isn't a valid expression outside
# `.new`/`is`/a type annotation), so this doesn't try to be cleverer
# than that. `entryable` is the already-constructed (not yet saved)
# model instance; `parent` is the parent Entry, or nil for a root entry.
def create_entry!(db, entryable_type, entryable, user_id, parent = nil)
  entryable.save(db)
  ancestry = if parent == nil then nil else parent.entry_path() end
  ancestry_depth = if parent == nil then 0 else parent.ancestry_depth() + 1 end
  entry = Entry.new({"ancestry": ancestry, "ancestry_depth": ancestry_depth, "user_id": user_id,
    "entryable_type": entryable_type, "entryable_id": entryable.id(), "created_at": Time.now().to_i()})
  entry.save(db)
  entry
end
