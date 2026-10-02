# A row of the `projects` table (see setup_db.di for the schema). Columns are
# copied from the row-shaped Hash into instance variables on construction and
# back out by `to_attributes` on save.
class Project < ActiveRecord::Model
  attr_accessor name: String, description: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
    @description = attributes["description"]
  end

  def to_attributes() = {"name": @name, "description": @description}
  # The Repository (table + row builder + validator) is a class variable set
  # per worker by ensure_models_configured (lib/middleware.di).
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  # The project's tasks, via tasks.project_id.
  def tasks(db) = self.has_many(Task.repository(), "project_id").all(db, self.id())
end

# Builds a Project from a database row; given to the Repository.
def build_project(row) = Project.new(row)

# The rules a Project must satisfy to be saved; a failure raises
# ActiveRecord::ValidationError carrying the messages, which the controller
# shows in the form. `combine` runs all the checks and gathers every message.
def build_project_validator()
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("name"),
    ActiveRecord::Validators.length("name", 1, 100),
    ActiveRecord::Validators.presence("description"),
    ActiveRecord::Validators.length("description", 1, 1000),
  ])
end
