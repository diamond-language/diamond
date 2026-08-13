class Feature
 attr_writer enabled
 attr_predicate enabled: Bool
end
feature = Feature.new()
feature.enabled=(true)
puts(feature.enabled?())

module State
 attr_predicate ready
end
class Job
 include State
end
puts(Job.new().ready?())
nil
