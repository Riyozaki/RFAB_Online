# Upstream workflows disabled in the RFAB Online fork

These GitHub Actions definitions were inherited from
rfortier/TiltedEvolution-rwf on import and are disabled (kept outside
`.github/workflows/`) because:

1. `docker-server-image.yml` publishes images to the upstream
   `tiltedphoques` registry namespace, which this fork must not do.
2. The Arena GitHub App connection used for development has no
   `workflows` permission, so commits touching `.github/workflows/`
   cannot be pushed through it.

To re-enable a workflow, move it back to `.github/workflows/` in a
commit pushed with a token that has the `workflows` permission
(e.g. from a local clone with your own GitHub credentials), and
adjust registries/secrets first. Local builds do not need these
files -- see `BUILD.md`.
