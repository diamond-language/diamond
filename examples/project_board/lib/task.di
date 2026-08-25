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
