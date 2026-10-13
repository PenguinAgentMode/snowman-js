// Running scripts and modules (src/engine.h). Owned by the interp workstream.
//
// The whole program runs on a large coroutine stack (Coroutine::kMainStackSize), so CheckStack() turns runaway
// recursion into a RangeError long before the stack is exhausted.
#include <malloc.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <optional>
#include <sstream>
#include <string>

#include "engine.h"
#include "interp/coroutine.h"
#include "interp/interpreter.h"
#include "interp/module.h"
#include "runtime/errors.h"
#include "runtime/heap.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman {

namespace {

using interp::InterpContext;

void printLine(const std::string& text) {
  std::string line = text + "\n";
  std::fwrite(line.data(), 1, line.size(), stdout);
}

// An uncaught exception as `Name: message` (Error-like objects) or its ToString.
std::string describeThrown(Value thrown) {
  if (thrown.isEmpty()) return "Error: unexpected generator return";
  ExecutionContext context;
  context.realm = vm().initialRealm;
  interp::ContextScope scope(&context);
  if (thrown.isObject()) {
    Object* object = thrown.asObject();
    Result<Value> name = Get(object, PropertyKey(u"name"));
    if (name.isNormal() && name.value().isString()) {
      Result<Value> message = Get(object, PropertyKey(u"message"));
      std::string text = name.value().asString()->toUtf8();
      if (message.isNormal() && !message.value().isUndefined()) {
        Result<String*> messageText = ToString(message.value());
        if (messageText.isNormal()) text += ": " + messageText.value()->toUtf8();
      } else {
        text += ":";
      }
      return text;
    }
  }
  Result<String*> text = ToString(thrown);
  if (text.isThrow()) return "Error: uncaught exception";
  std::string description = text.value()->toUtf8();
  if (thrown.isObject()) return description;
  return "Uncaught: " + description;
}

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// A --prelude or --epilogue file, evaluated as a global Script. False when it can't be read, doesn't parse or
// throws: printed after a Test262:HostScriptError line, so the runner can't take it for the test's own error.
bool runHostScript(Realm* realm, const std::string& file) {
  auto fail = [&](const std::string& error) {
    printLine("Test262:HostScriptError " + file);
    printLine(error);
    return false;
  };
  std::optional<std::string> source = readFile(file);
  if (!source) return fail("Error: cannot read " + file);
  parser::ParseResult parsed = parser::parseProgram(utf8ToUtf16(*source));
  if (!parsed.ok()) return fail("SyntaxError: " + parsed.error->message);
  auto* script = snowman::make<interp::ScriptRecord>(realm, parsed.program);
  Result<Value> result = interp::ScriptEvaluation(script);
  if (result.isThrow()) return fail(describeThrown(result.thrown()));
  return true;
}

struct MainData {
  const CliOptions* options;
  const std::string* source;
  int exitCode = 1;
};

int runProgram(const CliOptions& options, const std::string& source) {
  parser::stackLimit = &vm().stackLimit;
  Realm* realm = CreateRealm();
  vm().initialRealm = realm;
  interp::setUpRealm(realm);
  heap().setCollectorEnabled(true);
  if (const char* stress = std::getenv("SNOWMAN_GC_STRESS")) interp::gcStressInterval = unsigned(std::atoi(stress));
  std::u16string text = utf8ToUtf16(source);
  size_t slash = options.file.rfind('/');
  interp::setScriptDirectory(slash == std::string::npos ? "." : options.file.substr(0, slash));

  for (const std::string& file : options.preludes) {
    if (!runHostScript(realm, file)) return 1;
  }

  if (options.module) {
    // Drains the job queue; a rejected or never-completed evaluation is a throw.
    Result<void> result = interp::runModuleFile(realm, options.file, std::move(text));
    if (result.isThrow()) {
      printLine(describeThrown(result.thrown()));
      return 1;
    }
  } else {
    parser::ParseResult parsed = parser::parseProgram(std::move(text));
    if (!parsed.ok()) {
      printLine("SyntaxError: " + parsed.error->message);
      return 1;
    }
    auto* script = snowman::make<interp::ScriptRecord>(realm, parsed.program);
    Result<Value> result = interp::ScriptEvaluation(script);
    if (result.isThrow()) {
      printLine(describeThrown(result.thrown()));
      return 1;
    }
    Result<void> jobs = vm().runJobs();
    if (jobs.isThrow()) {
      printLine(describeThrown(jobs.thrown()));
      return 1;
    }
  }

  // The entry completed normally: the epilogues, then the jobs they queued.
  if (options.epilogues.empty()) return 0;
  for (const std::string& file : options.epilogues) {
    if (!runHostScript(realm, file)) return 1;
  }
  Result<void> jobs = vm().runJobs();
  if (jobs.isThrow()) {
    printLine(describeThrown(jobs.thrown()));
    return 1;
  }
  return 0;
}

void mainEntry(interp::Coroutine*, void* data) {
  auto* main = static_cast<MainData*>(data);
  main->exitCode = runProgram(*main->options, *main->source);
}

}  // namespace

int runMain(const CliOptions& options, const std::string& source) {
  // The collector frees many small cells at once; keep the freed memory for reuse instead of returning it to
  // the system and faulting it back in after every collection.
  mallopt(M_TRIM_THRESHOLD, 1 << 30);
  mallopt(M_TOP_PAD, 64 << 20);
  mallopt(M_MMAP_THRESHOLD, 256 << 20);
  // Out of memory (an address-space cap, ulimit -v): say so instead of aborting on std::bad_alloc.
  std::set_new_handler([] {
    std::fputs("fatal: out of memory\n", stdout);
    std::fflush(stdout);
    std::_Exit(71);
  });
  MainData data{&options, &source};
  interp::Coroutine main(mainEntry, &data, interp::Coroutine::kMainStackSize);
  if (!main.resume()) {
    std::fputs("fatal: cannot map the main stack\n", stdout);
    return 70;
  }
  std::fflush(stdout);
  return data.exitCode;
}

}  // namespace snowman
