def build_router()
  router = Dials::Router.new()
  router.get("/", SkinsController.index)
  router.get("/signup", SessionsController.new_signup_form)
  router.post("/signup", SessionsController.create_signup)
  router.get("/login", SessionsController.new_form)
  router.post("/login", SessionsController.create)
  router.post("/logout", SessionsController.destroy, [require_authentication, require_csrf])
  # Literal routes before the same-segment-count "/skins/:id" pattern --
  # Dials::Router matches the first registered route on a tie, so "new"
  # would otherwise be swallowed as if it were an id.
  router.get("/skins/new", SkinsController.new_form, [require_authentication])
  router.get("/skins/:id/edit", SkinsController.edit, [require_authentication, require_ownership])
  router.post("/skins/:id/delete", SkinsController.destroy, [require_authentication, require_ownership, require_csrf])
  router.post("/skins/:id", SkinsController.update, [require_authentication, require_ownership, require_csrf])
  router.get("/skins/:id", SkinsController.show)
  router.post("/skins", SkinsController.create, [require_authentication, require_csrf])
  router.get("/tags/:name", SkinsController.by_tag)
  router
end
