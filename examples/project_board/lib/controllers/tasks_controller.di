# Same shape as ProjectsController (see its comments); tasks have no index or
# show page of their own, they are listed on their project's page, so every
# write redirects back to that project.
class TasksController
  # Renders the new/edit form. The form needs every project for its dropdown.
  # `errors`/`status` let create/update redisplay a rejected form with 422.
  def self.render_form(request, context, task, id, errors, status)
    db = Database.get(context)
    projects = Project.all().order("name").to_a(db)
    action = if id == nil then "/tasks" else "/tasks/#{id}" end
    content = task_form_html(action, projects, task.project_id(), task.title(), task.done(), if id == nil then "Create task" else "Save task" end, context["csrf_token"], errors)
    Div.html_response(status, layout_html(if id == nil then "New task" else "Edit task" end, content, context["current_user"], context["csrf_token"]))
  end

  # Shared GET handler for "new" (id nil) and "edit".
  def self.form(request, context, id)
    task = if id == nil then Task.new({"project_id": "", "title": "", "done": 0}) else Task.find(Database.get(context), id) end
    if task == nil then return Dials::Response.not_found(request["path"]) end
    log_debug(request, context, "task.form_rendered", {"task_id": id})
    TasksController.render_form(request, context, task, id, [], 200)
  end

  # GET /tasks/new and GET /tasks/:id/edit.
  def self.new_form(request, context, params) = TasksController.form(request, context, nil)
  def self.edit(request, context, params) = TasksController.form(request, context, params["id"].to_i())

  # Form fields arrive as strings; the numeric columns are converted.
  def self.attributes(params) = {"project_id": params["project_id"].to_i(), "title": params["title"], "done": params["done"].to_i()}

  # POST /tasks: validate-and-save; on failure show the form again (422).
  def self.create(request, context, params)
    task = Task.new(TasksController.attributes(params))

    begin
      task.save(Database.get(context))
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "task.create_rejected", {"validation_errors": error.errors()})
      return TasksController.render_form(request, context, task, nil, error.errors(), 422)
    end

    log_info(request, context, "task.created", {"task_id": task.id(), "project_id": task.project_id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/projects/#{task.project_id()}", "created")
  end
  # POST /tasks/:id. A task can be moved to another project here, so the
  # redirect uses the (possibly new) project_id.
  def self.update(request, context, params)
    task = Task.find(Database.get(context), params["id"].to_i())
    if task == nil then return Dials::Response.not_found(request["path"]) end

    values = TasksController.attributes(params)
    task.project_id = values["project_id"]
    task.title = values["title"]
    task.done = values["done"]

    begin
      task.save(Database.get(context))
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "task.update_rejected", {"task_id": task.id(), "validation_errors": error.errors()})
      return TasksController.render_form(request, context, task, task.id(), error.errors(), 422)
    end

    log_info(request, context, "task.updated", {"task_id": task.id(), "project_id": task.project_id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/projects/#{task.project_id()}", "updated")
  end
  # POST /tasks/:id/delete. Remember the project id BEFORE deleting, since it
  # is needed afterwards to redirect back to the right project page (or to
  # the project list if the task was already gone).
  def self.destroy(request, context, params)
    task = Task.find(Database.get(context), params["id"].to_i())
    project_id = if task == nil then nil else task.project_id() end

    if task != nil
      task.destroy(Database.get(context))
      log_info(request, context, "task.deleted", {"task_id": task.id(), "project_id": project_id, "user_id": context["current_user"].id()})
    else
      log_warn(request, context, "task.delete_skipped", {"reason": "not_found", "task_id": params["id"]})
    end
    Dials::Response.redirect(if project_id == nil then "/projects" else "/projects/#{project_id}" end, "deleted")
  end
end
