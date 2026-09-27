#!/usr/bin/env bash
# Runs pathfinder through the interpreter and as a `diamond build` binary:
# every algorithm on an open maze and on weighted ground, diagonal moves,
# stdin input, the unreachable-goal exit status, and the map errors.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" build pathfinder.di -o "$work/pathfinder" > "$work/build.log"

for pathfinder in "$diamond pathfinder.di" "$work/pathfinder"; do
    for map in maze mud; do
        for algo in bfs dijkstra astar; do
            $pathfinder --algo "$algo" --stats "testdata/$map.txt" | cmp "testdata/$map.$algo.expected" -
        done
    done
    $pathfinder --diagonal --stats testdata/maze.txt | cmp testdata/maze.diagonal.expected -
    $pathfinder --diagonal --algo dijkstra --stats testdata/mud.txt | cmp testdata/mud.diagonal.expected -
    # A* is the default and reads the map from stdin with "-".
    $pathfinder --stats - < testdata/maze.txt | cmp testdata/maze.astar.expected -

    # BFS counts steps, so on rough ground it can cost more than the
    # cheapest route; Dijkstra and A* agree on the cheapest.
    bfs_cost="$($pathfinder --algo bfs --stats testdata/mud.txt | sed -n 's/.*cost \([0-9]*\),.*/\1/p')"
    dijkstra_cost="$($pathfinder --algo dijkstra --stats testdata/mud.txt | sed -n 's/.*cost \([0-9]*\),.*/\1/p')"
    astar_cost="$($pathfinder --algo astar --stats testdata/mud.txt | sed -n 's/.*cost \([0-9]*\),.*/\1/p')"
    [[ "$dijkstra_cost" == "$astar_cost" && "$bfs_cost" -gt "$dijkstra_cost" ]]

    # No route: the map is still printed, status 1.
    status=0; $pathfinder testdata/blocked.txt > "$work/out" 2> "$work/err" || status=$?
    [[ "$status" == 1 ]] && grep -q "no route from S to G" "$work/err" && grep -q '^#S#G#$' "$work/out"

    # Map and usage errors: status 2, and the message says where.
    status=0; $pathfinder testdata/ragged.txt 2> "$work/err" > /dev/null || status=$?
    [[ "$status" == 2 ]] && grep -q "line 2 is 4 wide, expected 5" "$work/err"
    status=0; $pathfinder testdata/badchar.txt 2> "$work/err" > /dev/null || status=$?
    [[ "$status" == 2 ]] && grep -q "unexpected 'X' at line 2, column 4" "$work/err"
    status=0; $pathfinder testdata/nostart.txt 2> "$work/err" > /dev/null || status=$?
    [[ "$status" == 2 ]] && grep -q "no start (S)" "$work/err"
    status=0; $pathfinder "$work/missing.txt" 2> "$work/err" > /dev/null || status=$?
    [[ "$status" == 2 ]] && grep -q "cannot open" "$work/err"
    status=0; $pathfinder --algo nonsense testdata/maze.txt 2> /dev/null > /dev/null || status=$?
    [[ "$status" == 2 ]]
    status=0; $pathfinder 2> /dev/null > /dev/null || status=$?
    [[ "$status" == 2 ]]
    status=0; $pathfinder --bogus testdata/maze.txt 2> /dev/null > /dev/null || status=$?
    [[ "$status" == 2 ]]
done

echo "pathfinder smoke test passed"
