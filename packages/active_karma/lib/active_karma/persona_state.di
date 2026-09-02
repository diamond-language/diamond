module ActiveKarma

  # Immutable trust-state snapshot -- ported from ActiveKarma's own
  # PersonaState. Four independent dimensions (trust_level,
  # write_permission, visibility, locks), each a fixed-vocabulary
  # Symbol/Array[Symbol], plus query predicates matching the Ruby
  # original one-for-one. No setters -- an instance is a value, built
  # once by Projector.project (see projector.di) and never mutated.
  class PersonaState
    attr_reader trust_level
    attr_reader write_permission
    attr_reader visibility
    attr_reader locks: Array

    def initialize(trust_level, write_permission, visibility, locks: Array = [])
      @trust_level = trust_level
      @write_permission = write_permission
      @visibility = visibility
      @locks = locks
    end

    def normal?() -> Bool = @trust_level == :normal
    def limited?() -> Bool = @trust_level == :limited
    def blocked?() -> Bool = @trust_level == :blocked
    def shadowbanned?() -> Bool = @trust_level == :shadowbanned
    def review_required?() -> Bool = @trust_level == :review_required

    def write_allowed?() -> Bool = @write_permission == :allowed
    def write_throttled?() -> Bool = @write_permission == :throttled
    def write_denied?() -> Bool = @write_permission == :denied

    def visible?() -> Bool = @visibility == :normal
    def shadow?() -> Bool = @visibility == :shadow
    def restricted?() -> Bool = @visibility == :restricted

    def locked?() -> Bool = @locks.length() > 0
    def compromised?() -> Bool = @locks.include?(:compromised)
    def verification_required?() -> Bool = @locks.include?(:verification_required)

    def can_write?() -> Bool = !self.write_denied?()
    def cannot_write?() -> Bool = self.write_denied?()

    def to_h() = {"trust_level": @trust_level, "write_permission": @write_permission,
      "visibility": @visibility, "locks": @locks}

    # The Ruby original exposes this as a frozen PersonaState::DEFAULT
    # constant; a factory method here instead, since a class-body
    # constant can't safely self-reference the class it's still defining
    # (see docs/roadmap.md's forward-reference notes) -- functionally
    # identical, since this class has no setters to begin with.
    def self.default() = PersonaState.new(:normal, :allowed, :normal, [])
  end

end
