class ProjectsController
  def self.index(request, context, params)
    projects = Project.all().order("name").to_a(Database.get(context))
    log_debug(request, context, "projects listed count=#{projects.length()}")
    Div.html_response(200, layout_html("Projects", projects_table_html(projects), context["current_user"], context["csrf_token"]))
  end

  def self.show(request, context, params)
    db = Database.get(context)
    project = Project.find(db, params["id"].to_i())
    if project == nil then return Dials::Response.not_found(request["path"]) end
    log_debug(request, context, "project shown project_id=#{project.id()}")
    content = project_show_html(project.id(), project.description(), project.tasks(db), context["current_user"] != nil, context["csrf_token"])
    Div.html_response(200, layout_html(project.name(), content, context["current_user"], context["csrf_token"]))
  end

  def self.render_form(request, context, project, id, errors, status)
    action = if id == nil then "/projects" else "/projects/#{id}" end
    content = project_form_html(action, project.name(), project.description(), if id == nil then "Create project" else "Save project" end, context["csrf_token"], errors)
    Div.html_response(status, layout_html(if id == nil then "New project" else "Edit project" end, content, context["current_user"], context["csrf_token"]))
  end

  def self.form(request, context, id)
    project = if id == nil then Project.new({"name": "", "description": ""}) else Project.find(Database.get(context), id) end
    if project == nil then return Dials::Response.not_found(request["path"]) end
    log_debug(request, context, "project form rendered project_id=#{id}")
    ProjectsController.render_form(request, context, project, id, [], 200)
  end

  def self.new_form(request, context, params) = ProjectsController.form(request, context, nil)
  def self.edit(request, context, params) = ProjectsController.form(request, context, params["id"].to_i())
  def self.create(request, context, params)
    project = Project.new({"name": params["name"], "description": params["description"]})
    begin
      project.save(Database.get(context))
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "project create rejected validation_errors=#{error.errors().join("; ")}")
      return ProjectsController.render_form(request, context, project, nil, error.errors(), 422)
    end
    log_info(request, context, "project created project_id=#{project.id()} user_id=#{context["current_user"].id()}")
    Dials::Response.redirect("/projects/#{project.id()}", "created")
  end
  def self.update(request, context, params)
    project = Project.find(Database.get(context), params["id"].to_i())
    if project == nil then return Dials::Response.not_found(request["path"]) end
    project.name = params["name"]
    project.description = params["description"]
    begin
      project.save(Database.get(context))
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "project update rejected project_id=#{project.id()} validation_errors=#{error.errors().join("; ")}")
      return ProjectsController.render_form(request, context, project, project.id(), error.errors(), 422)
    end
    log_info(request, context, "project updated project_id=#{project.id()} user_id=#{context["current_user"].id()}")
    Dials::Response.redirect("/projects/#{project.id()}", "updated")
  end
  def self.destroy(request, context, params)
    project = Project.find(Database.get(context), params["id"].to_i())
    if project != nil
      project.destroy(Database.get(context))
      log_info(request, context, "project deleted project_id=#{project.id()} user_id=#{context["current_user"].id()}")
    else
      log_warn(request, context, "project delete skipped reason=not_found project_id=#{params["id"]}")
    end
    Dials::Response.redirect("/projects", "deleted")
  end
end
