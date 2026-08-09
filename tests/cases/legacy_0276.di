module State
 attr_predicate ready
end
class Job
 include State
end
Job.new().ready?()
