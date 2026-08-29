# The core resource: one uploaded reskin/theme submission. Called
# "Skin" throughout (model, table, routes -- /skins, not /reskins) --
# "reskin" stays only as the general descriptive word for what the app
# is about.
class Skin < ActiveRecord::Model
  attr_accessor user_id, title: String, description: String, platform: String, preview_image_path, file_path: String, original_filename: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @user_id = attributes["user_id"]
    @title = attributes["title"]
    @description = attributes["description"]
    @platform = attributes["platform"]
    @preview_image_path = attributes["preview_image_path"]
    @file_path = attributes["file_path"]
    @original_filename = attributes["original_filename"]
  end

  def to_attributes() = {"user_id": @user_id, "title": @title, "description": @description,
    "platform": @platform, "preview_image_path": @preview_image_path,
    "file_path": @file_path, "original_filename": @original_filename}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def user(db) = self.belongs_to(User.repository()).get(db, @user_id)

  # A real many-to-many via the `taggings` join table --
  # ActiveRecord::HasManyThrough, not a hand-rolled join query.
  def tags(db) = ActiveRecord::HasManyThrough.new(Tag.repository(), Arel.table("taggings"), "skin_id", "tag_id").all(db, self.id())
end

def build_skin(row) = Skin.new(row)

def skin_platforms() = ["windows", "macos", "linux", "android", "ios", "other"]

def build_skin_validator()
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("title"),
    ActiveRecord::Validators.length("title", 1, 150),
    ActiveRecord::Validators.presence("description"),
    ActiveRecord::Validators.length("description", 1, 4000),
    ActiveRecord::Validators.presence("platform"),
    ActiveRecord::Validators.inclusion("platform", skin_platforms()),
    ActiveRecord::Validators.presence("file_path"),
    ActiveRecord::Validators.presence("original_filename"),
  ])
end
