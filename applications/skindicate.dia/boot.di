# Split from app.di the way examples/project_board splits its own
# boot.di from app.di: everything reachable without actually starting a
# server lives here, so smoke_test.di can `require "./boot"` and drive
# `app` directly without a real socket.

require "../../packages/active_record/lib/active_record"
require "../../packages/rack/lib/rack"
require "../../packages/cookies/lib/cookies"
require "../../packages/multipart/lib/multipart"
require "../../packages/div/lib/div/runtime"
require "../../packages/dials/lib/dials"
require "../../packages/logger/lib/logger"

require "./lib/config/environment"
require "./lib/views/.cache/layout.html"
require "./lib/views/.cache/skins_index.html"
require "./lib/views/.cache/skin_show.html"
require "./lib/views/.cache/skin_form.html"
require "./lib/views/.cache/login_form.html"
require "./lib/views/.cache/signup_form.html"

require "./lib/helpers/logging"
require "./lib/database"
require "./lib/models/user"
require "./lib/models/session"
require "./lib/models/skin"
require "./lib/models/tag"
require "./lib/models/tagging"
require "./lib/helpers/auth"
require "./lib/helpers/uploads"
require "./lib/controllers/sessions_controller"
require "./lib/controllers/skins_controller"
require "./lib/routes"
require "./lib/middleware"
