import fs from 'node:fs';

// Metadata only: never fetch or execute source code on the persistent build VM.
const sources = [
  ['mainline', 'Vita3K/Vita3K', 'master'],
  ['plus', 'nckstwrt/Vita3K-Plus', 'all-enhancements'],
  ['nx', 'NaGaa95/Vita3K-nx', 'master'],
];
const repository = process.env.GITHUB_REPOSITORY;
const token = process.env.GH_TOKEN;
if (repository !== 'zipkindev/Vita3K-ZipZ' || !token) throw Error('Unexpected repository or missing token');
async function api(path, method = 'GET', body) {
  const response = await fetch(`https://api.github.com/${path}`, {
    method,
    headers: { Authorization: `Bearer ${token}`, Accept: 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (!response.ok) throw Error(`GitHub API ${response.status}: ${path}`);
  return response.json();
}
const pins = JSON.parse(fs.readFileSync('docs/upstream-sources.proposal.json', 'utf8')).sources;
const changes = [];
for (const [name, repo, branch] of sources) {
  const current = await api(`repos/${repo}/commits/${branch}`);
  const old = pins[name].observedCommit;
  if (current.sha === old) continue;
  const comparison = await api(`repos/${repo}/compare/${old}...${current.sha}`);
  changes.push({ name, repo, old, sha: current.sha, status: comparison.status, ahead: comparison.ahead_by });
}
if (!changes.length) { console.log('Approved source refs unchanged.'); process.exit(0); }
const title = 'Upstream source changes awaiting candidate review';
const body = 'Metadata observation only; no source was merged, built or deployed.\n\n' + changes.map(c =>
  `- ${c.name}: [${c.old.slice(0, 9)} → ${c.sha.slice(0, 9)}](https://github.com/${c.repo}/compare/${c.old}...${c.sha}), ${c.status}, ${c.ahead} ahead.`
).join('\n') + '\n\nReview dependencies, duplicate patches and rewritten/diverged refs before updating the accepted candidate. After review, update the observed pins and approved-candidate.json in a maintainer-reviewed PR. Gameplay remains a separate promotion gate.';
const issues = await api(`repos/${repository}/issues?state=open&creator=github-actions%5Bbot%5D&per_page=100`);
const existing = issues.find(i => !i.pull_request && i.title === title);
if (existing) {
  if (existing.body !== body) await api(`repos/${repository}/issues/${existing.number}`, 'PATCH', { body });
  else console.log('Source changes already reported; no notification repeated.');
} else await api(`repos/${repository}/issues`, 'POST', { title, body });
