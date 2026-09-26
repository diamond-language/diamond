# taskrun: run tasks from a Taskfile in dependency order.
#
#   diamond taskrun.di [-f Taskfile] [-j N] [--time] [--plan] TARGET...
#
# -j N runs up to N independent tasks at once (default 1). --plan prints
# the order tasks would run in without running them. --time adds each
# task's wall-clock time. Exit status: 0 when every task succeeded, 1 when
# one failed (dependents are skipped), 2 for a Taskfile error, 64 for a
# usage error, 66 when the Taskfile can't be read.
require "./lib/runner"

def usage() -> Int
  warn("usage: taskrun [-f Taskfile] [-j N] [--time] [--plan] TARGET...")
  64
end

def read_file(path: String) -> String
  file = File.open(path, "r")
  begin
    file.read()
  ensure
    file.close()
  end
end

def main(args: Array[String]) -> Int
  path = "Taskfile"
  limit = 1
  show_time = false
  plan_only = false
  targets = []
  index = 0
  while index < args.length()
    case args[index]
    when "-f", "-j"
      return usage() if index + 1 >= args.length()
      if args[index] == "-f"
        path = args[index + 1]
      else
        limit = args[index + 1].to_i()
        return usage() if limit < 1
      end
      index += 1
    when "--time" then show_time = true
    when "--plan" then plan_only = true
    else
      return usage() if args[index].start_with?("-")
      targets.push(args[index])
    end
    index += 1
  end
  return usage() if targets.empty?()

  text = ""
  begin
    text = read_file(path)
  rescue error: IOError
    warn("taskrun: #{error.message()}")
    return 66
  end
  begin
    tasks = parse_taskfile(text)
    order = plan(tasks, targets)
    if plan_only
      order.each_with_index() do |name, position| puts("#{position + 1}. #{name}") end
      return 0
    end
    [ok, failed, skipped] = Runner.new(tasks, limit, show_time).run(order)
    puts("#{ok} ok, #{failed} failed, #{skipped} skipped")
    if failed > 0 then 1 else 0 end
  rescue error: TaskfileError
    warn("taskrun: #{path}: #{error.message()}")
    2
  end
end

exit(main(ARGV))
