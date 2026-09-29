# life: Conway's Game of Life. Loads a pattern ('.' dead, '#' alive) and
# prints it, then each of GENERATIONS steps after it.
require "./lib/grid"

def usage() -> Int
  warn("usage: life.di PATTERN_FILE GENERATIONS")
  64
end

def main(argv) -> Int
  return usage() unless argv.length() == 2
  return usage() unless life_digits?(argv[1])
  begin
    grid = grid_parse(File.open(argv[0], "r").read())
    generations = argv[1].to_i()
    puts(grid_render(grid))
    generation = 0
    while generation < generations
      grid = grid_step(grid)
      generation += 1
      puts("")
      puts("Generation #{generation}")
      puts(grid_render(grid))
    end
    0
  rescue error: IOError
    warn(error.message())
    66
  rescue error: LifeError
    warn(error.message())
    65
  end
end

exit(main(ARGV))
