# make                  build ./engine
# make test262          fetch test262 at the pinned commit
# make slice FILTER=test/language/literals [PARSE=1]
#                       one directory: what a workstream runs before asking to merge
# make sample           a deterministic ~1% of ES-core (shard 0/100): the pre-merge regression check
# make shard SHARD=k/N  one shard of the full run, to results/shard-k.jsonl; aggregate per t262/README.md
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra
CPPFLAGS += -Isrc
JOBS ?= 4
SOURCES := $(shell find src -name '*.cpp')
OBJECTS := $(SOURCES:src/%.cpp=build/%.o)
RUN := node t262/runner/run.mjs --engine ./engine --jobs $(JOBS) $(if $(PARSE),--parse-only)

engine: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJECTS:.o=.d)

test262:
	t262/fetch-test262.sh

slice: engine test262
	$(RUN) --filter $(FILTER) --out results/slice.jsonl

sample: engine test262
	$(RUN) --shard 0/100 --out results/sample.jsonl

shard: engine test262
	$(RUN) --shard $(SHARD) --out results/shard-$(firstword $(subst /, ,$(SHARD))).jsonl

clean:
	rm -rf build engine

.PHONY: test262 slice sample shard clean
