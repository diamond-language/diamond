class Project < ActiveRecord::Model
  attr_accessor name: String, description: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
    @description = attributes["description"]
  end

  def to_attributes() = {"name": @name, "description": @description}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  def tasks(db) = self.has_many(Task.repository(), "project_id").all(db, self.id())
end

def build_project(row) = Project.new(row)
