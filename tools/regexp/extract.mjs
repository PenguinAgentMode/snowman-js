// extract.mjs: the RegExp literals and RegExp("...") string arguments in test262 files, for oracle.mjs.
//   node tools/regexp/extract.mjs <dir-or-file>... > cases.jsonl
// Each line: {"file", "pattern", "flags", "negative"}: negative is test262's parse-negative flag of the file.
// A small JS tokenizer, good enough to tell regex literals from division in test262's code.
import { readFileSync, readdirSync, statSync } from 'node:fs';
import path from 'node:path';

const KEYWORDS_BEFORE_EXPR = new Set([
  'return', 'typeof', 'case', 'do', 'else', 'in', 'instanceof', 'new', 'delete', 'void', 'throw', 'yield', 'await', 'of',
]);
const isIdStart = (c) => /[\p{ID_Start}$_\\]/u.test(c);
const isIdPart = (c) => /[\p{ID_Continue}$\u200c\u200d\\]/u.test(c);
const LT = /[\n\r\u2028\u2029]/;

export function tokenize(src) {
  const out = [];  // {type: 'regex'|'string'|'ident'|'punct'|'num'|'template', ...}
  let i = 0;
  let prev = null;  // previous significant token
  const braces = [];  // template nesting: 'tpl' or '{'
  const regexAllowed = () => {
    if (!prev) return true;
    if (prev.type === 'num' || prev.type === 'string' || prev.type === 'regex' || prev.type === 'template') return false;
    if (prev.type === 'ident') return KEYWORDS_BEFORE_EXPR.has(prev.value);
    return ![')', ']', '}'].includes(prev.value);
  };
  const push = (t) => {
    out.push(t);
    prev = t;
  };
  const readTemplate = () => {
    // at a ` or after a } closing a substitution
    while (i < src.length) {
      const c = src[i];
      if (c === '\\') {
        i += 2;
      } else if (c === '`') {
        i++;
        return;
      } else if (c === '$' && src[i + 1] === '{') {
        i += 2;
        braces.push('tpl');
        return;
      } else {
        i++;
      }
    }
  };
  while (i < src.length) {
    const c = src[i];
    if (c === ' ' || c === '\t' || c === '\v' || c === '\f' || LT.test(c) || c === '\ufeff' || /\s/.test(c)) {
      i++;
      continue;
    }
    if (c === '/' && src[i + 1] === '/') {
      while (i < src.length && !LT.test(src[i])) i++;
      continue;
    }
    if (c === '/' && src[i + 1] === '*') {
      const end = src.indexOf('*/', i + 2);
      i = end < 0 ? src.length : end + 2;
      continue;
    }
    if (c === '/' && regexAllowed()) {
      let j = i + 1;
      let inClass = false;
      let ok = false;
      while (j < src.length && !LT.test(src[j])) {
        const d = src[j];
        if (d === '\\') {
          j += 2;
          continue;
        }
        if (d === '[') inClass = true;
        else if (d === ']') inClass = false;
        else if (d === '/' && !inClass) {
          ok = true;
          break;
        }
        j++;
      }
      if (ok) {
        const pattern = src.slice(i + 1, j);
        let k = j + 1;
        while (k < src.length && isIdPart(src[k])) k++;
        push({ type: 'regex', pattern, flags: src.slice(j + 1, k), start: i });
        i = k;
        continue;
      }
    }
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < src.length && src[j] !== c && !LT.test(src[j])) j += src[j] === '\\' ? 2 : 1;
      push({ type: 'string', raw: src.slice(i, j + 1), start: i });
      i = j + 1;
      continue;
    }
    if (c === '`') {
      i++;
      readTemplate();
      push({ type: 'template' });
      continue;
    }
    if (c === '}' && braces.length && braces[braces.length - 1] === 'tpl') {
      braces.pop();
      i++;
      readTemplate();
      push({ type: 'template' });
      continue;
    }
    if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src[i + 1]))) {
      let j = i + 1;
      while (j < src.length && /[0-9a-zA-Z_.]/.test(src[j])) j++;
      push({ type: 'num' });
      i = j;
      continue;
    }
    if (isIdStart(c) || (c >= '\ud800' && c <= '\udbff')) {
      let j = i + 1;
      while (j < src.length && (isIdPart(src[j]) || (src[j] >= '\ud800' && src[j] <= '\udfff'))) j++;
      push({ type: 'ident', value: src.slice(i, j) });
      i = j;
      continue;
    }
    if (c === '{') braces.push('{');
    if (c === '}') braces.pop();
    push({ type: 'punct', value: c });
    i++;
  }
  return out;
}

// The cooked value of a string literal token (only ever a lone string literal, so eval is safe here).
const cook = (raw) => {
  try {
    return (0, eval)(raw);
  } catch {
    return undefined;
  }
};

export function extract(file) {
  const src = readFileSync(file, 'utf8');
  const negative = /negative:\s*\n\s*phase:\s*parse/.test(src);
  const cases = [];
  let tokens;
  try {
    tokens = tokenize(src);
  } catch {
    return cases;
  }
  for (let t = 0; t < tokens.length; t++) {
    const tok = tokens[t];
    if (tok.type === 'regex') cases.push({ file, pattern: tok.pattern, flags: tok.flags, negative, kind: 'literal' });
    // RegExp("p"), new RegExp("p", "f")
    if (tok.type === 'ident' && tok.value === 'RegExp' && tokens[t + 1]?.value === '(' && tokens[t + 2]?.type === 'string') {
      const pattern = cook(tokens[t + 2].raw);
      let flags = '';
      if (tokens[t + 3]?.value === ',' && tokens[t + 4]?.type === 'string' && tokens[t + 5]?.value === ')') flags = cook(tokens[t + 4].raw);
      else if (tokens[t + 3]?.value !== ')') continue;
      if (typeof pattern === 'string' && typeof flags === 'string') cases.push({ file, pattern, flags, negative, kind: 'constructor' });
    }
  }
  return cases;
}

function* walk(p) {
  if (statSync(p).isDirectory()) {
    for (const e of readdirSync(p).sort()) yield* walk(path.join(p, e));
  } else if (p.endsWith('.js') && !p.includes('_FIXTURE')) {
    yield p;
  }
}

if (import.meta.url === `file://${process.argv[1]}`) {
  for (const arg of process.argv.slice(2)) for (const f of walk(arg)) for (const c of extract(f)) console.log(JSON.stringify(c));
}
