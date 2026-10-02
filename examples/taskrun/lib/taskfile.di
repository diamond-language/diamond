# Parsing a Taskfile and ordering its tasks.
#
#   # a comment
#   build: compile assets      <- a task and the tasks it depends on
#     echo building            <- its commands, indented, run in order
#
# A task may have no commands (it just groups its dependencies).

class TaskfileError < StandardError
end

struct Task(name: String, deps: Array[String], commands: Array[String], line: Int)
end

# Returns a Hash of task name -> Task, in file order.
def parse_taskfile(text: String) -> Hash
  tasks = {}
  current = nil
  number = 0

  text.split("\n").each() do |raw|
    number += 1
    line = raw.strip()
    next if line.empty?() || line.start_with?("#")

    # An indented line is a command of the most recent task. (Checked on the
    # RAW line, since `line` has had its indentation stripped.)
    if raw.start_with?(" ") || raw.start_with?("\t")
      if current == nil
        raise TaskfileError.new("line #{number}: command outside a task")
      end
      current.commands().push(line)
      next
    end

    # Otherwise it must be a "name: deps" header. Duplicate names are an
    # error that points at the first definition.
    found = Regexp.new("^([A-Za-z0-9_.-]+):(.*)$").match(line)
    raise TaskfileError.new("line #{number}: expected 'name: deps'") if found == nil
    [_, name, rest] = found

    if tasks.include_key?(name)
      raise TaskfileError.new("line #{number}: task '#{name}' already defined on line #{tasks[name].line()}")
    end
    deps = rest.split(" ").reject() do |dep| dep.empty?() end
    current = Task.new(name, deps, [], number)
    tasks[name] = current
  end
  tasks
end

# Every task `targets` need, dependencies before dependents, each once.
# Raises on an unknown task or a dependency cycle, naming the cycle.
def plan(tasks: Hash, targets: Array[String]) -> Array[String]
  order = []
  state = {}     # name -> :visiting while on the current path, :done after
  path = []

  # Depth-first walk. A task is added to `order` only AFTER all its
  # dependencies, which is what makes the order valid. `from` is the task
  # that asked for this one (for the error message). The state table does two
  # jobs: :done means "already planned, skip it" (so shared dependencies
  # appear once), and :visiting means "currently on the path", so reaching
  # such a task again is a cycle. All the state is passed as parameters
  # rather than captured.
  def visit(name, from, tasks, order, state, path)
    unless tasks.include_key?(name)
      raise TaskfileError.new(if from == nil then "no task named '#{name}'" else "'#{from}' depends on unknown task '#{name}'" end)
    end
    return nil if state[name] == :done

    # Back at a task still on the path: the slice of the path from there is
    # the cycle.
    if state[name] == :visiting
      cycle = path.drop(path.index_of(name))
      raise TaskfileError.new("dependency cycle: #{[*cycle, name].join(" -> ")}")
    end

    state[name] = :visiting
    path.push(name)
    tasks[name].deps().each() do |dep| visit(dep, name, tasks, order, state, path) end
    path.pop()
    state[name] = :done
    order.push(name)
    nil
  end

  # Plan each requested target in turn.
  targets.each() do |target| visit(target, nil, tasks, order, state, path) end
  order
end
