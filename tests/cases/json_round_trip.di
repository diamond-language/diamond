value = {"name": "Ada", "nested": {"list": [1, 2, 3], "flag": false}}
decoded = JSON.parse(JSON.stringify(value))
[
  decoded["name"] == value["name"],
  decoded["nested"]["list"][0] == 1,
  decoded["nested"]["list"][1] == 2,
  decoded["nested"]["list"][2] == 3,
  decoded["nested"]["flag"] == false,
]
