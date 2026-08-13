**Edge check**: 24 predecessor-edges vs 24 successor-edges -> **MATCH**

Edge sets are identical, so the two rendered graphs would necessarily be identical too.

## predecessors view (rebuilt from the original file)
```mermaid
flowchart LR
    0 --> 1
    0 --> 2
    1 --> 3
    1 --> 4
    2 --> 3
    2 --> 4
    2 --> 12
    3 --> 5
    3 --> 6
    3 --> 14
    4 --> 5
    4 --> 6
    5 --> 7
    6 --> 7
    8 --> 9
    8 --> 10
    9 --> 11
    9 --> 12
    10 --> 11
    11 --> 13
    12 --> 13
    12 --> 14
    13 --> 15
    14 --> 15
```

## successors view (rebuilt from the generated file)
```mermaid
flowchart LR
    0 --> 1
    0 --> 2
    1 --> 3
    1 --> 4
    2 --> 3
    2 --> 4
    2 --> 12
    3 --> 5
    3 --> 6
    3 --> 14
    4 --> 5
    4 --> 6
    5 --> 7
    6 --> 7
    8 --> 9
    8 --> 10
    9 --> 11
    9 --> 12
    10 --> 11
    11 --> 13
    12 --> 13
    12 --> 14
    13 --> 15
    14 --> 15
```
