class C
  def self.get() = @@never_set
end
C.get() == nil
