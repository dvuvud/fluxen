# Contributing

All contributions are welcome, whether that be bug reports, fixes, new examples, or ideas.

## Getting started

```bash
git clone --recursive https://github.com/dvuvud/fluxen.git
cd fluxen
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

All library code is in `fluxen.hpp`. Tests are in `tests/test.cpp`. Examples are in `examples/`.

## Guidelines

- Open an issue before starting on anything large
- Add or update tests for any code changes
- Keep pull requests small and focused

That's it. Don't hesitate to open an issue if you're unsure about anything.
