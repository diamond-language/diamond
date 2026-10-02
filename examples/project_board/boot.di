# Loads every piece of the app in dependency order, so that both app.di and
# the smoke test can `require "./boot"` and get a working `app` function.
#
# Order matters: a compiled view (`*_html`) or any top-level helper called by
# bare name must already be defined where it is used, whereas
# `ClassName.method` calls resolve later and can reference classes in any
# order (see examples/library/app.di for the long form of this rule). So the
# views and helpers come first, then models, controllers, routes, middleware.
require_cut "active_record"
require_cut "rack"
require_cut "div"
require_cut "dials"
require_cut "logger"

# Configuration, then the pre-compiled HTML templates.
require "./lib/config/environment"
require "./lib/views/.cache/home.html"
require "./lib/views/.cache/login_form.html"
require "./lib/views/.cache/projects_table.html"
require "./lib/views/.cache/project_show.html"
require "./lib/views/.cache/project_form.html"
require "./lib/views/.cache/task_form.html"
require "./lib/views/.cache/layout.html"

# Logging and database access, then the models they serve.
require "./lib/helpers/logging"
require "./lib/database"
require "./lib/models/user"
require "./lib/models/session"
require "./lib/models/project"
require "./lib/models/task"

# Auth helpers (they use the models), then controllers, which use the
# helpers; routes name controller actions; middleware names the router.
require "./lib/helpers/auth"
require "./lib/controllers/sessions_controller"
require "./lib/controllers/projects_controller"
require "./lib/controllers/tasks_controller"
require "./lib/routes"
require "./lib/middleware"
