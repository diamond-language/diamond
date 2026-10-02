# Actions take (request, context, params): `params` has the URL `:id` and any
# form fields (strings). Writes are only reachable through the guards in
# routes.di, so `context["current_user"]` is always set in create/update/
# destroy. Failed validations re-render the form with status 422 instead of
# redirecting, so the user's input and the error messages are not lost.
class ProjectsController
  # GET /projects (public): all projects by name.
  def self.index(request, context, params)
    projects = Project.all().order("name").to_a(Database.get(context))
    log_debug(request, context, "projects.listed", {"count": projects.length()})
    Div.html_response(200, layout_html("Projects", projects_table_html(projects), context["current_user"], context["csrf_token"]))
  end

  # GET /projects/:id (public): the project and its tasks. The `!= nil` flag
  # tells the view whether to show the edit/add links.
  def self.show(request, context, params)
    db = Database.get(context)
    project = Project.find(db, params["id"].to_i())
    if project == nil then return Dials::Response.not_found(request["path"]) end
    log_debug(request, context, "project.shown", {"project_id": project.id()})
    content = project_show_html(project.id(), project.description(), project.tasks(db), context["current_user"] != nil, context["csrf_token"])
    Div.html_response(200, layout_html(project.name(), content, context["current_user"], context["csrf_token"]))
  end

  # Renders the new/edit form. `id` is nil for "new". `errors` (a list of
  # messages, empty on a first visit) and `status` (200, or 422 after a failed
  # save) are what let create/update redisplay a rejected form.
  def self.render_form(request, context, project, id, errors, status)
    action = if id == nil then "/projects" else "/projects/#{id}" end
    content = project_form_html(action, project.name(), project.description(), if id == nil then "Create project" else "Save project" end, context["csrf_token"], errors)
    Div.html_response(status, layout_html(if id == nil then "New project" else "Edit project" end, content, context["current_user"], context["csrf_token"]))
  end

  # GET handler shared by "new" (id nil) and "edit": load or blank the
  # project, then show it with no errors.
  def self.form(request, context, id)
    project = if id == nil then Project.new({"name": "", "description": ""}) else Project.find(Database.get(context), id) end
    if project == nil then return Dials::Response.not_found(request["path"]) end
    log_debug(request, context, "project.form_rendered", {"project_id": id})
    ProjectsController.render_form(request, context, project, id, [], 200)
  end

  # GET /projects/new and GET /projects/:id/edit.
  def self.new_form(request, context, params) = ProjectsController.form(request, context, nil)
  def self.edit(request, context, params) = ProjectsController.form(request, context, params["id"].to_i())

  # POST /projects. `save` validates first and raises ValidationError if the
  # project is invalid; on success it redirects to the new project.
  def self.create(request, context, params)
    project = Project.new({"name": params["name"], "description": params["description"]})

    begin
      project.save(Database.get(context))
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "project.create_rejected", {"validation_errors": error.errors()})
      return ProjectsController.render_form(request, context, project, nil, error.errors(), 422)
    end

    log_info(request, context, "project.created", {"project_id": project.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/projects/#{project.id()}", "created")
  end
  # POST /projects/:id: load, overwrite the editable fields, save (which
  # validates). A rejected edit re-renders with the user's changes still in
  # the form.
  def self.update(request, context, params)
    project = Project.find(Database.get(context), params["id"].to_i())
    if project == nil then return Dials::Response.not_found(request["path"]) end

    project.name = params["name"]
    project.description = params["description"]

    begin
      project.save(Database.get(context))
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "project.update_rejected", {"project_id": project.id(), "validation_errors": error.errors()})
      return ProjectsController.render_form(request, context, project, project.id(), error.errors(), 422)
    end

    log_info(request, context, "project.updated", {"project_id": project.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/projects/#{project.id()}", "updated")
  end
  # POST /projects/:id/delete. Deleting something already gone is logged but
  # not an error. The project's tasks are removed by the database's
  # ON DELETE CASCADE, not by this code.
  def self.destroy(request, context, params)
    project = Project.find(Database.get(context), params["id"].to_i())

    if project != nil
      project.destroy(Database.get(context))
      log_info(request, context, "project.deleted", {"project_id": project.id(), "user_id": context["current_user"].id()})
    else
      log_warn(request, context, "project.delete_skipped", {"reason": "not_found", "project_id": params["id"]})
    end
    Dials::Response.redirect("/projects", "deleted")
  end
end
