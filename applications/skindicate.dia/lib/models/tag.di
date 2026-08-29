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

  # The reverse direction of Skin#tags -- every skin tagged with this
  # tag, via the same `taggings` join, just with the two join keys
  # swapped.
  def skins(db) = ActiveRecord::HasManyThrough.new(Skin.repository(), Arel.table("taggings"), "tag_id", "skin_id").all(db, self.id())
end

def build_tag(row) = Tag.new(row)

def build_tag_validator()
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("name"),
    ActiveRecord::Validators.length("name", 1, 40),
    ActiveRecord::Validators.format("name", Regexp.new("^[a-z0-9-]+$")),
  ])
end

# Normalizes free-typed tag text (a comma-separated field on the skin
# submission form) into the canonical lowercase/hyphenated form
# `build_tag_validator` above requires -- "Dark Mode" -> "dark-mode".
def normalize_tag_name(raw)
  raw.strip().downcase().gsub(Regexp.new("\\s+"), "-")
end
