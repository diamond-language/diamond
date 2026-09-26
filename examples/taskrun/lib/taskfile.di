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

def parse_taskfile(text: String) -> Hash
  tasks = {}
  current = nil
  number = 0
  text.split("\n").each() do |raw|
    number += 1
    line = raw.strip()
    next if line.empty?() || line.start_with?("#")
    if raw.start_with?(" ") || raw.start_with?("\t")
      if current == nil
        raise TaskfileError.new("line #{number}: command outside a task")
      end
      current.commands().push(line)
      next
    end
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
  def visit(name, from, tasks, order, state, path)
    unless tasks.include_key?(name)
      raise TaskfileError.new(if from == nil then "no task named '#{name}'" else "'#{from}' depends on unknown task '#{name}'" end)
    end
    return nil if state[name] == :done
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
  targets.each() do |target| visit(target, nil, tasks, order, state, path) end
  order
end
