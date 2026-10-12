// node-host.mjs: node itself as an engine that keeps the kit's CLI contract, so M0 proves the runner
// and sharding against a known-good engine before any engine of ours exists.
//
//   node --expose-gc hosts/node-host.mjs [--module] [--parse-only] file.js
//
// Contract: exit 0 on success; on an uncaught exception print "Name: message" and exit 1; print()
// writes a line to stdout; $262 is a global (createRealm, evalScript, detachArrayBuffer, gc, global).
import fs from 'node:fs';
import vm from 'node:vm';
import { pathToFileURL } from 'node:url';

const args = process.argv.slice(2);
const isModule = args.includes('--module');
const parseOnly = args.includes('--parse-only');
const file = args.filter((a) => !a.startsWith('--')).at(-1);

function report(error) {
  const name = error?.constructor?.name ?? 'Error';
  const message = error?.message ?? String(error);
  process.stdout.write(`${name}: ${message}\n`);
  process.exitCode = 1;
}

function makeHost(context) {
  const host = {
    global: context ? vm.runInContext('globalThis', context) : globalThis,
    evalScript: (source) => (context ? vm.runInContext(source, context) : vm.runInThisContext(source)),
    createRealm: () => {
      const realm = vm.createContext({});
      const realmHost = makeHost(realm);
      vm.runInContext('globalThis', realm).$262 = realmHost;
      vm.runInContext('globalThis', realm).print = print;
      return realmHost;
    },
    detachArrayBuffer: (buffer) => {
      structuredClone(buffer, { transfer: [buffer] });
    },
    gc: () => globalThis.gc?.(),
    agent: { start() {}, broadcast() {}, getReport() { return null; }, sleep() {}, monotonicNow: () => performance.now() },
  };
  return host;
}

function print(...values) {
  process.stdout.write(values.map(String).join(' ') + '\n');
}

globalThis.print = print;
globalThis.$262 = makeHost(null);
process.on('uncaughtException', report);
process.on('unhandledRejection', report);

const source = fs.readFileSync(file, 'utf8');
if (parseOnly) {
  try {
    if (isModule) new vm.SourceTextModule(source);
    else new vm.Script(source);
  } catch (error) {
    report(error);
  }
} else if (isModule) {
  await import(pathToFileURL(file).href).catch(report);
} else {
  try {
    vm.runInThisContext(source, { filename: file });
  } catch (error) {
    report(error);
  }
}
