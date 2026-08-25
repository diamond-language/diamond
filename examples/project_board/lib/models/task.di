class Task < ActiveRecord::Model
  attr_accessor project_id, title: String, done

  def initialize(attributes: Hash = {})
    super(attributes)
    @project_id = attributes["project_id"]
    @title = attributes["title"]
    @done = attributes["done"]
  end

  def to_attributes() = {"project_id": @project_id, "title": @title, "done": @done}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  def project(db) = self.belongs_to(Project.repository()).get(db, @project_id)
  def done?() = @done == 1
end

def build_task(row) = Task.new(row)

def build_task_validator(db)
  def project_exists(attributes)
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
