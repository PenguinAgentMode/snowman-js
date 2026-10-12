# snowman-js

A JavaScript engine in C++, built by a team of agents and measured by test262.

**The goal:** climb the ladder in [t262/README.md](t262/README.md#milestones) on ES-core (43,305 files): a
parser (M1), then the harness bootstrap (M2), the ES5 core (M3) and on up to ES-core ≥ 95% (M8).

## Layout

- `src/`: the engine. `make` builds `./engine`; today it only keeps the CLI contract and fails every test.
- `t262/`: the runner, aggregator and milestone ladder. Owned by the lead.
- `results/`: one aggregated JSON per measured engine commit.
- `INTERFACES.md`: the seams workstreams code against.
- `STATUS.md`: owners, the latest result, waivers.
- `AGENTS.md`: how the team works.

## Commands

```
make                                      # ./engine (g++, C++20)
make test262                              # test262 at the pinned commit
make slice FILTER=test/language/literals  # one directory; PARSE=1 for parse-only
make sample                               # a deterministic ~1% of ES-core
make shard SHARD=3/8                      # one shard of the full run
```
