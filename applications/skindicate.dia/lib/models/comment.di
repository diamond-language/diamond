class Comment < ActiveRecord::Model
  attr_accessor skin_id, user_id, body: String, created_at

  def initialize(attributes: Hash = {})
    super(attributes)
    @skin_id = attributes["skin_id"]
    @user_id = attributes["user_id"]
    @body = attributes["body"]
    @created_at = attributes["created_at"]
  end

  def to_attributes() = {"skin_id": @skin_id, "user_id": @user_id, "body": @body, "created_at": @created_at}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def user(db) = self.belongs_to(User.repository()).get(db, @user_id)
end

def build_comment(row) = Comment.new(row)

def build_comment_validator()
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("body"),
    ActiveRecord::Validators.length("body", 1, 1000),
  ])
end

# Oldest first -- a normal comment-thread reading order, unlike
# SkinsController.newest_first's own newest-first browse ordering.
def comments_for_skin(db, skin_id) = Comment.where({"skin_id": skin_id}).order(Arel.table("comments").column("created_at").asc()).to_a(db)
