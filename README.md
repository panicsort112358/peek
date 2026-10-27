# peek

[![Build](https://github.com/njlane314/peek/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/njlane314/peek/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)

`peek` is a single-header C++20 library for compact, bounded developer
diagnostics.

```cpp
#define PEEK_COMPILED 1
#include <peek.hpp>

#include <vector>

int main()
{
    int answer = 42;
    std::vector<int> values{1, 4, 9};
    PEEK(answer, values);
}
```

Output includes the source location and original expression names:

```text
main.cpp:10  answer = 42, values = [1, 4, 9]
```

## USE

Diagnostics are removed by default. Define `PEEK_COMPILED` to `1` before the
header to enable them; this policy is independent of `LOCAL`, `NDEBUG`, and
`ONLINE_JUDGE`.

```cpp
PEEK(x, answer, graph[u]);
PEEK_IF(distance[v] < 0, v, distance[v]);

{
    PEEK_SCOPE("dijkstra");
    run_dijkstra();
}

auto text = peek::format(value);
peek::set_sink(consume_record);
peek::reset_sink();
```

`PEEK` records values, `PEEK_IF` records them only when its condition is true,
and `PEEK_SCOPE` records entry, elapsed time, and exception unwinding. Enabled
expressions are evaluated exactly once, from left to right. They should still
not carry important side effects.

## DESIGN

With `PEEK_COMPILED=0`, the header exposes only three no-op macros and includes
no standard-library machinery. Their arguments are not evaluated and do not
need to be valid C++.

Enabled diagnostics preserve `errno`, bound every record, serialize output
between threads, and allow neither formatter nor sink failures to escape.
There is no compiled library or global registration step.

## FORMATTING

Built-in formatting covers strings, scalars, pointer addresses, optionals,
variants, tuples, maps, and forward ranges. Add an ADL customization for a user
type when needed:

```cpp
void diagnostic_format(peek::writer& out, point const& value)
{
    out << "point{" << value.x << ", " << value.y << '}';
}
```

`peek::format(value)` uses the same formatter. Default limits are five nested
levels, 32 range items, 256 string bytes, and 4096 bytes per record.
`peek::format_options` can tighten them for direct formatting.

## SINKS

Output is synchronous. The default sink writes complete records to standard
error; `peek::set_sink(callable)` replaces it, and `peek::reset_sink()` restores
it. The callable receives one complete record. An empty sink discards output,
and recursive writes from inside a sink are suppressed.

## CFX

In a [`cfx`](https://github.com/njlane314/cfx) solution archive, add `peek`
directly as a submodule:

```sh
git submodule add https://github.com/njlane314/peek include/peek
```

`cfx` makes that root available as `<peek.hpp>`, enables diagnostics for local
builds, and disables them in the exact source prepared for submission.

## LIMITS

- Expression-name recovery is best-effort because macros cannot parse C++.
- Parenthesize a `PEEK_IF` condition containing a comma.
- Put separate `PEEK_SCOPE` declarations on separate source lines.
- Output is bounded and synchronous; slow sinks delay the calling thread.
- Logging levels, files, assertions, rate limits, crash handling, and telemetry
  are intentionally outside the library.

## INSTALL

Enabled mode requires a C++20 compiler with exceptions and threading. The
library has no dependencies beyond the C++ standard library. Compile directly
against a checkout with:

```sh
c++ -std=c++20 -pthread -I/path/to/peek main.cpp -o main
```

Or install the header and license:

```sh
make install PREFIX="$HOME/.local"
```

`PREFIX`, `DESTDIR`, and `INCLUDEDIR` are supported.

## CHECKS

```sh
make check
```

## LICENSE

[MIT](LICENSE)# peek
