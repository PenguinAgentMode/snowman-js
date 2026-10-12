// driver.mjs: build/validate_driver (validate_driver.cpp), rebuilt when a source is newer; runDriver(cases).
import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, statSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const DIRS = ['src/regexp', 'src/unicode'];

function buildDriver() {
  const out = path.join(root, 'build/validate_driver');
  const sources = [
    'tools/regexp/validate_driver.cpp',
    ...DIRS.flatMap((d) => readdirSync(path.join(root, d)).filter((f) => f.endsWith('.cpp')).map((f) => `${d}/${f}`)),
  ];
  const newest = Math.max(...sources.map((s) => statSync(path.join(root, s)).mtimeMs), ...headerTimes());
  if (!existsSync(out) || statSync(out).mtimeMs < newest) {
    execFileSync('mkdir', ['-p', path.join(root, 'build')]);
    execFileSync('g++', ['-std=c++20', '-O2', '-Wall', '-Wextra', '-Isrc', '-o', out, ...sources], { cwd: root, stdio: 'inherit' });
  }
  return out;
}
function headerTimes() {
  return DIRS.flatMap((d) =>
    readdirSync(path.join(root, d)).filter((f) => /\.(h|inc)$/.test(f)).map((f) => statSync(path.join(root, d, f)).mtimeMs),
  );
}

const hex = (s) => {
  let out = '';
  for (let i = 0; i < s.length; i++) out += s.charCodeAt(i).toString(16).padStart(4, '0');
  return out;
};

export function runDriver(cases) {
  const driver = buildDriver();
  const input = cases.map((c) => `${hex(c.flags)}\t${hex(c.pattern)}`).join('\n') + '\n';
  const out = execFileSync(driver, { input, maxBuffer: 1 << 30 }).toString().split('\n');
  return cases.map((_, i) => out[i]);
}
