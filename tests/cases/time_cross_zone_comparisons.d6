same_utc = Time.at(1000).utc()
same_east = Time.at(1000).localtime(50400)
same_west = Time.at(1000).localtime(-43200)
earlier_east = Time.at(999).localtime(50400)
later_west = Time.at(1001).localtime(-43200)

[
  same_utc == same_east, same_east == same_west,
  same_utc != same_west,
  earlier_east < same_west, earlier_east <= same_utc,
  later_west > same_east, later_west >= same_utc,
  same_east <= same_west, same_west >= same_utc,
]
