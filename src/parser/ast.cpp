// AST support: type names, operator text, the node arena.
#include "parser/ast.h"

#include <cstdlib>

namespace snowman::ast {

const char* nodeTypeName(NodeType type) {
  switch (type) {
#define SNOWMAN_AST_NAME(name) \
  case NodeType::name:         \
    return #name;
    SNOWMAN_AST_NODE_TYPES(SNOWMAN_AST_NAME)
#undef SNOWMAN_AST_NAME
  }
  return "?";
}

const char* operatorText(UnaryOperator op) {
  static const char* const kText[] = {"-", "+", "!", "~", "typeof", "void", "delete"};
  return kText[static_cast<int>(op)];
}

const char* operatorText(UpdateOperator op) { return op == UpdateOperator::Increment ? "++" : "--"; }

const char* operatorText(BinaryOperator op) {
  static const char* const kText[] = {"==", "!=", "===", "!==", "<",  "<=", ">", ">=", "<<", ">>",         ">>>",
                                      "+",  "-",  "*",   "/",   "%",  "**", "|", "^",  "&",  "in", "instanceof"};
  return kText[static_cast<int>(op)];
}

const char* operatorText(LogicalOperator op) {
  static const char* const kText[] = {"||", "&&", "??"};
  return kText[static_cast<int>(op)];
}

const char* operatorText(AssignmentOperator op) {
  static const char* const kText[] = {"=",   "+=", "-=", "*=", "/=",  "%=",  "**=", "<<=",
                                      ">>=", ">>>=", "|=", "^=", "&=", "||=", "&&=", "?\?="};
  return kText[static_cast<int>(op)];
}

namespace {

struct Chunk {
  char* next = nullptr;
  char* end = nullptr;
};

// Per thread (agent): each agent parses into its own arena.
thread_local Chunk gChunk;

}  // namespace

void* arenaAllocate(size_t size, size_t align) {
  auto aligned = [&](char* p) { return reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(p) + align - 1) & ~(align - 1)); };
  char* p = gChunk.next ? aligned(gChunk.next) : nullptr;
  if (!p || p + size > gChunk.end) {
    size_t chunkSize = size + align > (256u << 10) ? size + align : (256u << 10);
    char* base = static_cast<char*>(std::malloc(chunkSize));
    if (!base) std::abort();
    gChunk.next = base;
    gChunk.end = base + chunkSize;
    p = aligned(base);
  }
  gChunk.next = p + size;
  return p;
}

}  // namespace snowman::ast
