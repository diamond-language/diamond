group_by(.engine)[] |
  {
    engine: .[0].engine,
    server_version: .[0].server_version,
    runs: length,
    point_reads_per_second: (map(.point_reads_per_second) | add / length),
    range_reads_per_second: (map(.range_reads_per_second) | add / length),
    updates_per_second: (map(.updates_per_second) | add / length),
    connection_seconds: (map(.connection_seconds) | add / length),
    seed_seconds: (map(.seed_seconds) | add / length)
  }
