# Route table. The optional last argument of each route is a list of guards
# run in order before the action; a guard that returns a response stops the
# request. Reading pages are open to everyone; every write requires a login
# (`require_authentication`) and, for POSTs, a valid CSRF token
# (`require_csrf`). Login itself needs neither, since there is no session
# yet. The action methods are passed as bare references and, as in
# examples/library, literal paths ("/projects/new") are registered before
# their ":id" siblings so ":id" cannot capture "new".
def home_action(request, context, params) = Div.html_response(200, layout_html("Project board", home_html(), context["current_user"], context["csrf_token"]))

def build_router()
  router = Dials::Router.new()
  router.get("/", home_action)

  # Sessions.
  router.get("/login", SessionsController.new_form)
  router.post("/login", SessionsController.create)
  router.post("/logout", SessionsController.destroy, [require_authentication, require_csrf])

  # Projects.
  router.get("/projects/new", ProjectsController.new_form, [require_authentication])
  router.get("/projects", ProjectsController.index)
  router.post("/projects", ProjectsController.create, [require_authentication, require_csrf])
  router.get("/projects/:id/edit", ProjectsController.edit, [require_authentication])
  router.post("/projects/:id/delete", ProjectsController.destroy, [require_authentication, require_csrf])
  router.get("/projects/:id", ProjectsController.show)
  router.post("/projects/:id", ProjectsController.update, [require_authentication, require_csrf])

  # Tasks (listed on their project's page, so there is no index/show).
  router.get("/tasks/new", TasksController.new_form, [require_authentication])
  router.post("/tasks", TasksController.create, [require_authentication, require_csrf])
  router.get("/tasks/:id/edit", TasksController.edit, [require_authentication])
  router.post("/tasks/:id/delete", TasksController.destroy, [require_authentication, require_csrf])
  router.post("/tasks/:id", TasksController.update, [require_authentication, require_csrf])

  router
end
