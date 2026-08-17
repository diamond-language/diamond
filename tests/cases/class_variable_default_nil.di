class C
  def self.get()
    @@never_set
  end
end
C.get() == nil
