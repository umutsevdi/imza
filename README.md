# ursa

A minimal C++23 coding agent.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DTESTS=ON
cmake --build build --target ursa ursa_tests
```

## Run

```sh
./build/debug/ursa
```

Configure the agent via `ursa/config.json` (see `AGENTS.md` for the
platform-specific config path and schema).

## Test

```sh
./build/debug/ursa_tests
```
