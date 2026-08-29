# The `taggings` join row itself -- an ordinary ActiveRecord::Model
# (not just a bare Arel table) so it can be created/queried/destroyed
# the same way every other model here is, even though nothing ever
# reads it back as a domain object in its own right (Skin#tags/
# Tag#skins read through it via ActiveRecord::HasManyThrough instead,
# never via this class).
class Tagging < ActiveRecord::Model
  attr_accessor skin_id, tag_id

  def initialize(attributes: Hash = {})
    super(attributes)
    @skin_id = attributes["skin_id"]
    @tag_id = attributes["tag_id"]
  end

  def to_attributes() = {"skin_id": @skin_id, "tag_id": @tag_id}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
end

def build_tagging(row) = Tagging.new(row)

# Finds each already-normalized tag name, creating any that don't exist
# yet, then replaces `skin`'s own taggings with exactly this set --
# called on both skin creation and every subsequent edit, so an edit
# that drops a tag actually removes the old association instead of
# only ever adding new ones.
def find_or_create_tag(db, name)
  existing = Tag.where({"name": name}).first(db)
  if existing != nil
    existing
  else
    # Model.self.create returns Repository#create's own raw result (an
    # insert count), not a mapped instance -- unlike Model#save, which
    # explicitly re-reads last_insert_row_id() -- so the created row
    # has to be reloaded here to get a real Tag back.
    Tag.create(db, {"name": name})
    Tag.find(db, db.last_insert_row_id())
  end
end

def set_skin_tags(db, skin, tag_names)
  existing_taggings = Tagging.where({"skin_id": skin.id()}).to_a(db)
  index = 0
  while index < existing_taggings.length()
    existing_taggings[index].destroy(db)
    index += 1
  end
  index = 0
  while index < tag_names.length()
    tag = find_or_create_tag(db, tag_names[index])
    Tagging.create(db, {"skin_id": skin.id(), "tag_id": tag.id()})
    index += 1
  end
end
