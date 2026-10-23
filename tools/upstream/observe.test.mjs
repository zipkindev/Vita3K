import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(new URL('./observe.mjs', import.meta.url), 'utf8').replace("import fs from 'node:fs';", '');
const pins = Object.fromEntries(['mainline', 'plus', 'nx'].map(k => [k, { observedCommit: 'a'.repeat(40) }]));
async function run({ changed = false, status = 'ahead', existing = null } = {}) {
  const writes = [];
  const code = `(async()=>{${source}})()`;
  const context = vm.createContext({
    fs: { readFileSync: () => JSON.stringify({ sources: pins }) },
    process: { env: { GITHUB_REPOSITORY: 'zipkindev/Vita3K-ZipZ', GH_TOKEN: 'test' }, exit: () => { throw Error('EXIT'); } },
    console: { log() {} },
    fetch: async (url, options) => {
      const path = url.replace('https://api.github.com/', '');
      if (options.method !== 'GET') {
        writes.push({ path, method: options.method, body: JSON.parse(options.body) });
        return { ok: true, json: async () => ({}) };
      }
      const data = path.includes('/commits/') ? { sha: (changed && path.includes('/Vita3K/Vita3K/')) ? 'b'.repeat(40) : 'a'.repeat(40) }
        : path.includes('/compare/') ? { status, ahead_by: 1 }
        : existing ? [existing] : [];
      return { ok: true, json: async () => data };
    },
  });
  try { await vm.runInContext(code, context); } catch (e) { if (e.message !== 'EXIT') throw e; }
  return writes;
}
assert.equal((await run()).length, 0, 'unchanged heads do not write');
const initial = await run({ changed: true });
assert.equal(initial.length, 1);
assert.equal(initial[0].method, 'POST');
assert.match(initial[0].body.body, /ahead/);
const existing = { number: 17, title: initial[0].body.title, body: initial[0].body.body };
assert.equal((await run({ changed: true, existing })).length, 0, 'unchanged report is quiet');
const diverged = await run({ changed: true, status: 'diverged', existing });
assert.equal(diverged[0].method, 'PATCH');
assert.match(diverged[0].body.body, /diverged/);
console.log('Observer checks passed: unchanged, new change, repeated observation, diverged ref.');
