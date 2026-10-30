# Reviewed candidate automation

Proposal only until this branch is merged to `master`. No schedule or automatic candidate promotion has been enabled yet.

The daily/manual observer runs on Linux, reads the three approved source refs and maintains one issue when they differ from `docs/upstream-sources.proposal.json`. Unchanged observations do not cause repeated issue edits. Divergence is reported for review; no source is fetched or executed. Mainline is the primary upstream; Plus and nx are selective import feeds.

After reviewing dependencies and preparing an isolated source candidate, update the observed source pins and `approved-candidate.json` in a maintainer-reviewed PR. Merging a change to the approved candidate file dispatches the existing Windows package workflow on `master`. That workflow uses the dedicated self-hosted Windows server, six compile workers, CTests, support-patch manifests and matching-symbol checks. Public PR code and unreviewed observed upstream heads never run automatically on that server. Existing fork Actions policy remains unchanged.

`sourceCommit` is an immutable 40-character commit, and every new binary needs a new `buildLabel`. Do not reuse an already built label with changed inputs. The initial manifest has `enabled: false` and points to the curated candidate proposal, not a gameplay-approved release. After the first validation, enable builds in a reviewed PR with a new unique build label; merging this proposal alone does not repeat the in-progress manual build. First build has been dispatched manually so the proposed automation need not be enabled to validate it.

Artifacts remain candidate downloads with the workflow's three-day retention. Promotion requires manual device validation and a separate versioned release retaining runtime, symbols and exact provenance. No automatic merging, release publication, production mapping changes or Go deployment occurs.

Daily polling requires GitHub's repository setting permitting Actions issue creation; it uses the existing GITHUB_TOKEN with issue write permission. The dispatcher uses Actions write permission only. There is no stored personal token. Schedules may be delayed/disabled by GitHub; manual workflow dispatch remains available. If a source ref becomes unavailable, the observer fails visibly instead of silently accepting another branch.

Future work: automated guest/cache integration fixtures, a disposable worker boundary before unattended external source builds, and reviewed patch retirement when upstream equivalents pass our retained tests. The existing persistent server is reserved for owner-reviewed source.
