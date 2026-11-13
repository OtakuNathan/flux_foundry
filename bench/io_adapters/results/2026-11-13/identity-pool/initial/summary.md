| Compiler | Producer | Profile | completed/s baseline → identity | p99 µs baseline → identity |
|---|---:|---|---:|---:|
| clang++ | 2 | steady500k | 500007 → 500009 | 3.90 → 3.63 |
| clang++ | 2 | overload2m | 1999957 → 1999978 | 5.77 → 3.34 |
| clang++ | 4 | steady500k | 499999 → 499985 | 5.41 → 5.48 |
| clang++ | 4 | overload2m | 1119893 → 1421670 | 12746.97 → 6593.53 |
| g++ | 2 | steady500k | 499996 → 499997 | 3.67 → 3.71 |
| g++ | 2 | overload2m | 1999956 → 1999951 | 6.38 → 3.49 |
| g++ | 4 | steady500k | 499997 → 499986 | 5.58 → 5.56 |
| g++ | 4 | overload2m | 1197084 → 1638479 | 10889.08 → 3583.78 |
