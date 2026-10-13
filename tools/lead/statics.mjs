// statics.mjs: the mutable-static lint (AGENTS.md: no new mutable static unless thread_local, atomic, mutex-guarded or
// immutable after init). Finds function-local and namespace-scope `static` variables and non-const globals in src/,
// and compares them with tools/lead/statics-allowlist.txt ("<path> <name>  # reason" per line). A regex lint: its few
// false positives are allowlisted too.
//
//   node tools/lead/statics.mjs            exit 1 if a static is not in the allowlist (prints NEW-STATIC lines)
//   node tools/lead/statics.mjs --list     print every finding as an allowlist line (to seed or review the list)
//
// gate.mjs runs check() too, so the merge gate reports new statics.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const ALLOWLIST = path.join(ROOT, 'tools/lead/statics-allowlist.txt');

function sources(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) sources(full, out);
    else if (/\.(cpp|h)$/.test(entry.name)) out.push(full);
  }
  return out;
}

// Not a mutable variable: const/constexpr/thread_local/atomic declarations, type and namespace lines, functions.
const SAFE = /\b(const|constexpr|thread_local|consteval|constinit)\b|std::atomic\b|static_assert|static_cast/;
const NOT_DECL = /^(using|namespace|class|struct|union|enum|template|typedef|extern|return|friend|inline|public|private|protected|case|default|goto|if|else|for|while|do|switch|break|continue|#)/;

// The declared name of `text` (a declaration without its leading `static`), or null for a function declaration.
function declaredName(text) {
  const eq = text.indexOf('=');
  const paren = text.indexOf('(');
  const brace = text.indexOf('{');
  // A '(' before any '=' or '{' starts a parameter list: a function (or a constructor call, rare in statics).
  if (paren >= 0 && (eq < 0 || paren < eq) && (brace < 0 || paren < brace)) return null;
  const end = Math.min(...[eq, brace, text.indexOf(';'), text.indexOf('[')].filter((i) => i >= 0));
  const head = text.slice(0, Number.isFinite(end) ? end : text.length).trim();
  const m = head.match(/([A-Za-z_][A-Za-z0-9_]*)\s*$/);
  return m ? m[1] : null;
}

export function findStatics() {
  const found = [];
  for (const file of sources(path.join(ROOT, 'src'))) {
    const rel = path.relative(ROOT, file);
    const lines = fs.readFileSync(file, 'utf8').split('\n');
    const stack = [];  // the open scopes: 'namespace' or 'block'; a global is a declaration with only namespaces open
    let continuation = false;  // the previous line didn't end a statement or open/close a scope
    for (const raw of lines) {
      // Comments and string/char literals out, so '{' in "{" or u'}' doesn't move the scope count.
      const line = raw.replace(/\/\/.*$/, '').replace(/u?(["'])(?:\\.|(?!\1).)*\1/g, '""');
      const trimmed = line.trim();
      const atNamespaceScope = stack.every((kind) => kind === 'namespace');
      if (!continuation && /^static\s/.test(trimmed) && !SAFE.test(trimmed)) {
        const name = declaredName(trimmed.replace(/^static\s+/, ''));
        if (name) found.push({ file: rel, name, text: raw.trim().slice(0, 100) });
      } else if (!continuation && atNamespaceScope && /^[A-Za-z_]/.test(trimmed) && !NOT_DECL.test(trimmed) &&
                 !SAFE.test(trimmed) && /^[A-Za-z_][\w:<>,*&\s]*\s[*&]*[A-Za-z_]\w*\s*(\[[^\]]*\])?\s*(=[^=].*)?;\s*$/.test(trimmed)) {
        const name = declaredName(trimmed);
        if (name) found.push({ file: rel, name, text: raw.trim().slice(0, 100) });
      }
      // A function pointer variable at namespace scope: `R (*name)(params) = ...;`.
      const pointer = trimmed.match(/^[A-Za-z_][\w:<>,*&\s]*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\(.*\)\s*(=[^=].*)?;\s*$/);
      if (!continuation && atNamespaceScope && pointer && !NOT_DECL.test(trimmed) && !SAFE.test(trimmed)) {
        found.push({ file: rel, name: pointer[1], text: raw.trim().slice(0, 100) });
      }
      // Scopes: a line that opens a namespace pushes 'namespace', any other '{' pushes 'block'.
      for (const ch of line) {
        if (ch === '{') stack.push(/^\s*(inline\s+)?namespace\b/.test(line) ? 'namespace' : 'block');
        else if (ch === '}') stack.pop();
      }
      if (trimmed && !trimmed.startsWith('#')) continuation = !/[;{}:]\s*$/.test(trimmed);
    }
  }
  return found;
}

export function readAllowlist() {
  if (!fs.existsSync(ALLOWLIST)) return new Map();
  const entries = new Map();
  for (const line of fs.readFileSync(ALLOWLIST, 'utf8').split('\n')) {
    const m = line.match(/^\s*(\S+)\s+(\S+)\s*(?:#\s*(.*))?$/);
    if (m && !line.trim().startsWith('#')) entries.set(`${m[1]} ${m[2]}`, m[3] ?? '');
  }
  return entries;
}

// The findings not in the allowlist, and the allowlist entries no longer found.
export function check() {
  const allow = readAllowlist();
  const found = findStatics();
  const seen = new Set(found.map((f) => `${f.file} ${f.name}`));
  const fresh = found.filter((f) => !allow.has(`${f.file} ${f.name}`));
  const stale = [...allow.keys()].filter((k) => !seen.has(k));
  return { found, fresh, stale };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  if (process.argv.includes('--list')) {
    for (const f of findStatics()) console.log(`${f.file} ${f.name}  # ${f.text}`);
  } else {
    const { found, fresh, stale } = check();
    for (const f of fresh) console.log(`NEW-STATIC ${f.file} ${f.name}: ${f.text}`);
    for (const k of stale) console.log(`stale allowlist entry (no longer found): ${k}`);
    console.log(`statics: ${found.length} found, ${fresh.length} new, ${stale.length} stale allowlist entries`);
    process.exitCode = fresh.length ? 1 : 0;
  }
}
