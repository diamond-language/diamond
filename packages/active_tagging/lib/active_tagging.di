require "../../active_record/lib/active_record"

# Generalized from applications/skindicate.dia's own hand-rolled
# Tag/Tagging models and SkinsController.parse_tags -- a from-scratch
# acts_as_taggable: free-typed comma-separated text -> normalized tag
# names -> find-or-create Tag rows -> diff-and-replace join rows on
# save. Only skindicate's own `skin_id` column name was actually
# tag-specific; everything else was already domain-agnostic.
#
# `Tagging#taggable_id` is a single opaque id, exactly like
# packages/active_social's Follow#follower_id/#followed_id -- this
# package doesn't assume or enforce which table it references. Unlike
# Follow, though, there's no `taggable_type` discriminator: one
# `ActiveTagging::Tagging` class means one shared `@@repository` class
# variable (see packages/active_record's own Model class comment on
# why -- every subclass gets its own slot, but two *different owner
# types* both configuring this same class would collide, last
# `.configure` call wins). Fine for skindicate (only Skin is taggable),
# not something this package tries to solve speculatively for a second
# taggable type that doesn't exist yet -- see README's "out of scope"
# for the real workaround if that day comes.
module ActiveTagging

  class Tag < ActiveRecord::Model
    attr_accessor name: String

    def initialize(attributes: Hash = {})
      super(attributes)
      @name = attributes["name"]
    end

    def to_attributes() = {"name": @name}
    def repository() = @@repository
    def self.repository() = @@repository
    def self.configure(repository: ActiveRecord::Repository)
      @@repository = repository
    end

    # "Dark Mode" -> "dark-mode" -- the canonical lowercase/hyphenated
    # form the validator built by build_active_tagging_tag_validator
    # (below) requires.
    def self.normalize_name(raw: String) -> String = raw.strip().downcase().gsub(Regexp.new("\\s+"), "-")

    # Comma-separated free-typed text (a tag-list form field, typically)
    # -> normalized, deduplicated names in first-occurrence order. Blank
    # input, or a field that normalizes to nothing but blanks/commas,
    # returns [] rather than [""].
    def self.parse_names(text)
      if text == nil || text.strip() == ""
        return []
      end
      names = []
      closure collect_name(raw)
        normalized = Tag.normalize_name(raw)
        if normalized != "" && !names.include?(normalized)
          names.push(normalized)
        end
      end
      text.split(",").each(collect_name)
      names
    end

    # The existing Tag named `name`, or a freshly created one.
    def self.find_or_create(db, name)
      existing = Tag.where({"name": name}).first(db)
      if existing != nil
        existing
      else
        # Model.self.create returns Repository#create's own raw result
        # (an insert count), not a mapped instance -- unlike Model#save,
        # which explicitly re-reads last_insert_row_id() -- so the
        # created row has to be reloaded here to get a real Tag back.
        Tag.create(db, {"name": name})
        Tag.find(db, db.last_insert_row_id())
      end
    end
  end

  # The `taggings` join row itself -- an ordinary ActiveRecord::Model
  # (not just a bare Arel table) so it can be created/queried/destroyed
  # the same way every other model here is, even though nothing reads it
  # back as a domain object in its own right; a caller reads through it
  # via .tags_for/.taggable_ids_for_tag below instead.
  class Tagging < ActiveRecord::Model
    attr_accessor tag_id, taggable_id

    def initialize(attributes: Hash = {})
      super(attributes)
      @tag_id = attributes["tag_id"]
      @taggable_id = attributes["taggable_id"]
    end

    def to_attributes() = {"tag_id": @tag_id, "taggable_id": @taggable_id}
    def repository() = @@repository
    def self.repository() = @@repository
    def self.configure(repository: ActiveRecord::Repository)
      @@repository = repository
    end

    # Every Tag currently attached to `taggable_id`.
    def self.tags_for(db, taggable_id)
      taggings = Tagging.where({"taggable_id": taggable_id}).to_a(db)
      tags = []
      index = 0
      while index < taggings.length()
        tags.push(Tag.find(db, taggings[index].tag_id()))
        index += 1
      end
      tags
    end

    # The raw taggable_ids currently tagged with `tag_id` -- map these
    # through your own owner model's .find yourself (e.g. Skin.find);
    # this package has no way to know which class taggable_id refers to.
    def self.taggable_ids_for_tag(db, tag_id)
      taggings = Tagging.where({"tag_id": tag_id}).to_a(db)
      ids = []
      index = 0
      while index < taggings.length()
        ids.push(taggings[index].taggable_id())
        index += 1
      end
      ids
    end

    # Finds/creates each name in `tag_names` (already-normalized or not
    # -- run it through Tag.parse_names first if it's raw user text),
    # then replaces `taggable_id`'s own taggings with exactly this set --
    # call this on both creation and every subsequent edit, so an edit
    # that drops a tag actually removes the old association instead of
    # only ever adding new ones.
    def self.set_tags(db, taggable_id, tag_names)
      existing = Tagging.where({"taggable_id": taggable_id}).to_a(db)
      index = 0
      while index < existing.length()
        existing[index].destroy(db)
        index += 1
      end
      index = 0
      while index < tag_names.length()
        tag = Tag.find_or_create(db, tag_names[index])
        Tagging.create(db, {"taggable_id": taggable_id, "tag_id": tag.id()})
        index += 1
      end
    end
  end

  end

  # Genuine top-level functions, not nested inside `module ActiveTagging`
  # above -- a top-level function referencing a namespaced class needs
  # that class already declared earlier in the same file (see
  # packages/active_auth/README.md's own note on this), so these are
  # placed after the module.
  def build_active_tagging_tag(row) = ActiveTagging::Tag.new(row)
  def build_active_tagging_tagging(row) = ActiveTagging::Tagging.new(row)

  # Matches skindicate's own original build_tag_validator exactly --
  # name required, 1-40 characters, lowercase-alphanumeric-and-hyphen
  # only (the same charset Tag.normalize_name above produces).
  def build_active_tagging_tag_validator()
    ActiveRecord::Validators.combine([
      ActiveRecord::Validators.presence("name"),
      ActiveRecord::Validators.length("name", 1, 40),
      ActiveRecord::Validators.format("name", Regexp.new("^[a-z0-9-]+$")),
    ])
end
