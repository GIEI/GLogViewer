# Release Process

The release workflow is defined in [GitHub Actions CI](../.github/workflows/ci.yml). It runs for tags that start with `v`.

Before tagging a release:

1. Update `LOGVIEW_VERSION` in `src/main.c` and add release notes to [CHANGELOG.md](../CHANGELOG.md).
2. Confirm the version printed by `logview --version` matches the tag without its leading `v`.
3. Confirm the 1.0 acceptance gates in the [roadmap](ROADMAP.md) are complete before creating the stable `v1.0.0` tag.

Push the version tag to trigger the release workflow. The workflow builds and tests Windows, Linux, and macOS packages, checks the binary version against the tag, runs Linux AddressSanitizer and UndefinedBehaviorSanitizer checks, then creates a GitHub release with the platform archives and `SHA256SUMS.txt`. Tags containing a hyphen, such as `v0.9.0-rc.1`, are published as prereleases. Tags without a prerelease suffix, such as `v1.0.0`, are published as stable releases.

Each archive contains the executable, README, license, changelog, and user manual. Linux and macOS archives retain executable permission. GitHub also provides source archives for the tagged commit. The SHA-256 file verifies downloaded assets against accidental corruption; it is not a digital signature.
