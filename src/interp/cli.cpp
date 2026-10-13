// Running scripts and modules (src/engine.h). Owned by the interp workstream.
//
// The whole program runs on a 1 GB coroutine stack, so CheckStack() turns runaway recursion into a
// RangeError long before the stack is exhausted.
#include <malloc.h>

#include <cstdio>
#include <cstdlib>
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

struct MainData {
  const CliOptions* options;
  const std::string* source;
  int exitCode = 1;
};

int runProgram(const CliOptions& options, const std::string& source) {
  Realm* realm = CreateRealm();
  vm().initialRealm = realm;
  interp::setUpRealm(realm);
  heap().setCollectorEnabled(true);
  if (const char* stress = std::getenv("SNOWMAN_GC_STRESS")) interp::gcStressInterval = unsigned(std::atoi(stress));
  std::u16string text = utf8ToUtf16(source);

  if (options.module) {
    Result<void> result = interp::runModuleFile(realm, options.file, std::move(text));
    if (result.isThrow()) {
      printLine(describeThrown(result.thrown()));
      return 1;
    }
    return 0;
  }

  size_t slash = options.file.rfind('/');
  interp::setScriptDirectory(slash == std::string::npos ? "." : options.file.substr(0, slash));
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
  MainData data{&options, &source};
  interp::Coroutine main(mainEntry, &data, interp::Coroutine::kMainStackSize);
  main.resume();
  std::fflush(stdout);
  return data.exitCode;
}

}  // namespace snowman
