# A comment is a pure `entryable` -- just its own body. Threading,
# authorship, and timestamps all live on its own `Entry` (see
# entry.di); a "discussion" for a skin is nothing more than the skin's
# own root Entry plus every descendant Entry whose entryable_type is
# "Comment".
class Comment < ActiveRecord::Model
  attr_accessor body: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @body = attributes["body"]
  end

  def to_attributes() = {"body": @body}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def entry(db) = Entry.where({"entryable_type": "Comment", "entryable_id": self.id()}).first(db)
end

def build_comment(row) = Comment.new(row)

def build_comment_validator()
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("body"),
    ActiveRecord::Validators.length("body", 1, 1000),
  ])
end
