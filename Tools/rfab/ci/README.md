# Proposed CI workflow

`release.yml.proposed` is the intended replacement for `.github/workflows/release.yml`.
The agent's GitHub App token has no `workflows` permission, so a maintainer must copy it manually:

```
cp Tools/rfab/ci/release.yml.proposed .github/workflows/release.yml
```

What it changes:
- push to `main` / `arena/**` -> build + TPTests + packaging only, Quick_Update zip attached as a workflow artifact;
- publishing only via **Run workflow** (workflow_dispatch) with `publish = true` and a NEW `version_tag`;
- `concurrency` cancels outdated runs of the same branch;
- the `-VersionTag "v1.0.0"` hardcode is gone.

Even without this file, `Build-CloudRelease.ps1` is already safe: without `-Publish` it never modifies GitHub Releases.
