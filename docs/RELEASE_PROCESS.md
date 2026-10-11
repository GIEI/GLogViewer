# Release Process

The release workflow is defined in [GitHub Actions CI](../.github/workflows/ci.yml). It runs for tags that start with `v`.

Before tagging a release:

1. Update `LOGVIEW_VERSION` in `src/main.c` and add release notes to [CHANGELOG.md](../CHANGELOG.md).
2. Confirm the version printed by `logview --version` matches the tag without its leading `v`.
3. Confirm the release's platform test, sanitizer, and packaging jobs pass before treating its `vX.Y.Z` tag as published.

Push the version tag to trigger the release workflow. The workflow builds and tests Windows, Linux, and macOS packages, checks the binary version against the tag, runs Linux AddressSanitizer and UndefinedBehaviorSanitizer checks, creates `SHA256SUMS.txt`, generates GitHub artifact attestations for the release assets, then publishes the GitHub release. Tags containing a hyphen, such as `v1.1.0-rc.1`, are published as prereleases. Tags without a prerelease suffix, such as `v1.1.0`, are published as stable releases.

Each archive contains the executable, README, license, changelog, and user manual. Linux and macOS archives retain executable permission. GitHub also provides source archives for the tagged commit. The SHA-256 file detects accidental corruption. Artifact attestations provide signed build provenance for the release assets; verify a downloaded archive with GitHub CLI:

```sh
gh attestation verify logview-linux-x86_64.tar.gz --repo GIEI/GLogViewer
```

Use the matching downloaded archive path on Windows and macOS. Verification requires a supported GitHub CLI version and an online connection.
