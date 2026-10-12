// breakdown.mjs results/<sha>.json [depth] [prefix]: passing/total files per directory, sorted by failures,
// plus the most common error messages from the JSONL next to it (results/<sha>/full.jsonl).
import fs from 'node:fs';
const [file, depthArg = '3', prefix = 'test/'] = process.argv.slice(2);
const depth = Number(depthArg);
const result = JSON.parse(fs.readFileSync(file, 'utf8'));
const rows = Object.entries(result.dirs)
  .filter(([dir]) => dir.startsWith(prefix) && dir.split('/').length === depth)
  .map(([dir, { files, passed }]) => ({ dir, files, passed, failed: files - passed }))
  .sort((a, b) => b.failed - a.failed);
for (const r of rows.slice(0, 40)) console.log(`${String(r.failed).padStart(6)} failing  ${String(r.passed).padStart(6)}/${String(r.files).padEnd(6)} ${r.dir}`);
const jsonl = file.replace(/\.json$/, '/full.jsonl');
if (fs.existsSync(jsonl)) {
  const counts = new Map();
  for (const line of fs.readFileSync(jsonl, 'utf8').split('\n')) {
    if (!line.trim()) continue;
    const x = JSON.parse(line);
    if (x.status === 'pass' || !x.file.startsWith(prefix)) continue;
    const key = `${x.status} ${x.error_name ?? ''}: ${(x.message ?? '').replace(/\d+/g, 'N').slice(0, 90)}`;
    counts.set(key, (counts.get(key) ?? 0) + 1);
  }
  console.log('\ntop failure messages (scenarios):');
  for (const [k, v] of [...counts].sort((a, b) => b[1] - a[1]).slice(0, 30)) console.log(`${String(v).padStart(6)}  ${k}`);
}
