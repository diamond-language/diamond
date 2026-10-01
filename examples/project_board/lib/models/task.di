# A row of the `tasks` table (see setup_db.di for the schema). Columns are
# copied from the row-shaped Hash into instance variables on construction and
# back out by `to_attributes` on save.
class Task < ActiveRecord::Model
  attr_accessor project_id, title: String, done

  def initialize(attributes: Hash = {})
    super(attributes)
    @project_id = attributes["project_id"]
    @title = attributes["title"]
    @done = attributes["done"]
  end

  def to_attributes() = {"project_id": @project_id, "title": @title, "done": @done}
  # The Repository (table + row builder + validator) is a class variable set
  # per worker by ensure_models_configured (lib/middleware.di).
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  # The owning project (a task always has one: the schema cascades deletes).
  def project(db) = self.belongs_to(Project.repository()).get(db, @project_id)

  # `done` is stored as 0/1.
  def done?() = @done == 1
end

# Builds a Task from a database row; given to the Repository.
def build_task(row) = Task.new(row)

# Task rules. Unlike Project's, one rule needs the database: a task's
# project_id must name a real project. `db` is captured by the nested
# validator below, which is why this is a builder function taking the
# worker's connection.
def build_task_validator(db)
  # A validator takes (attributes, exclude_id) and returns a list of error
  # messages (empty when fine). `exclude_id` exists for uniqueness checks and
  # is unused here.
  def project_exists(attributes, exclude_id)
    project_id = attributes["project_id"]
    if project_id == nil || Project.find(db, project_id) == nil
      ["project must exist"]
    else
      []
    end
  end


  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("title"),
    ActiveRecord::Validators.length("title", 1, 200),
    ActiveRecord::Validators.inclusion("done", [0, 1]),
    project_exists,
  ])
end
