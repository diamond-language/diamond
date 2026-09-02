module ActiveDiscussion

  # Ported from ActiveDiscussion's own Configuration. `.configure`-style,
  # matching this codebase's own established pattern (see
  # packages/active_auth/lib/active_auth/configuration.di) rather than
  # Ruby's `ActiveDiscussion.configure do |c| ... end` yield-a-mutable-
  # object block DSL. `karma_checker`/`item_karma_initial_for` are plain
  # Callable values here instead of Ruby procs -- pass a `def self.x`
  # singleton method reference (`SomeClass.some_method`, no call) or a
  # top-level function, same as any other Callable value in this
  # codebase.
  class Configuration
    def self.configure(options: Hash = {})
      @@flame_threshold = if options["flame_threshold"] == nil then 5 else options["flame_threshold"] end
      @@flame_window = if options["flame_window"] == nil then 300 else options["flame_window"] end
      @@cooldown_duration = if options["cooldown_duration"] == nil then 3600 else options["cooldown_duration"] end
      @@karma_checker = options["karma_checker"]
      @@item_karma_initial_for = options["item_karma_initial_for"]
      @@hide_karma_threshold = if options["hide_karma_threshold"] == nil then 0 else options["hide_karma_threshold"] end
    end

    def self.ensure_configured()
      if @@flame_threshold == nil
        Configuration.configure()
      end
    end

    def self.flame_threshold()
      Configuration.ensure_configured()
      @@flame_threshold
    end
    def self.flame_window()
      Configuration.ensure_configured()
      @@flame_window
    end
    def self.cooldown_duration()
      Configuration.ensure_configured()
      @@cooldown_duration
    end
    def self.karma_checker()
      Configuration.ensure_configured()
      @@karma_checker
    end
    def self.item_karma_initial_for()
      Configuration.ensure_configured()
      @@item_karma_initial_for
    end
    def self.hide_karma_threshold()
      Configuration.ensure_configured()
      @@hide_karma_threshold
    end
  end

end
