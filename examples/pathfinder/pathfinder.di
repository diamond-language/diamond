# pathfinder: shortest routes across an ASCII map.
#
#   diamond pathfinder.di [--algo bfs|dijkstra|astar] [--diagonal] [--stats] MAP
#
# MAP is a text file (or "-" for stdin) with one character per cell: # is a
# wall, . is open ground, 2-9 is rough ground that costs that much to enter,
# S is the start and G the goal. The map is printed with the route drawn
# as *. --stats also prints the route's length and cost and how many cells
# the search looked at.
#
# Exit status: 0 when a route exists, 1 when the goal can't be reached, 2 for
# a usage error or an unreadable or malformed map.
require "./lib/grid"
require "./lib/search"

def usage() = "usage: pathfinder [--algo bfs|dijkstra|astar] [--diagonal] [--stats] MAP"

def read_map(path: String) -> String
  if path == "-"
    text = ""
    loop do
      line = gets()
      break if line == nil
      text += line + "\n"
    end
    text
  else
    File.read(path)
  end
end

def main(args: Array[String]) -> Int
  algo = "astar"
  diagonal = false
  stats = false
  path = nil
  index = 0
  while index < args.length()
    arg = args[index]
    case arg
    when "--algo"
      index += 1
      if index >= args.length() || !["bfs", "dijkstra", "astar"].include?(args[index])
        warn("pathfinder: --algo takes bfs, dijkstra, or astar\n#{usage()}")
        return 2
      end
      algo = args[index]
    when "--diagonal" then diagonal = true
    when "--stats" then stats = true
    else
      if arg.start_with?("-") && arg != "-" || path != nil
        warn("pathfinder: unexpected #{arg}\n#{usage()}")
        return 2
      end
      path = arg
    end
    index += 1
  end
  if path == nil
    warn(usage())
    return 2
  end

  begin
    grid = Grid.new(read_map(path))
  rescue error: MapError
    warn("pathfinder: #{path}: #{error.message()}")
    return 2
  rescue error: IOError
    warn("pathfinder: #{error.message()}")
    return 2
  end

  found = case algo
  when "bfs" then search_bfs(grid, diagonal)
  when "dijkstra" then search_dijkstra(grid, diagonal)
  else search_astar(grid, diagonal)
  end

  if found == nil
    puts(grid.render([]))
    warn("pathfinder: no route from S to G")
    return 1
  end
  puts(grid.render(found.path()))
  if stats
    puts("#{algo}: #{found.path().length() - 1} steps, cost #{found.cost()}, looked at #{found.expanded()} cells")
  end
  0
end

exit(main(ARGV))
