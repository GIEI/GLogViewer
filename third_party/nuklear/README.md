# Nuklear dependency

This directory contains the Nuklear single-header UI library and its SDL3 renderer adapter.

- Upstream: https://github.com/Immediate-Mode-UI/Nuklear
- Release: `v4.13.3`
- Commit: `a53ad2c658151071501372a5e0e5e978153835aa`
- License: MIT or Unlicense; see [LICENSE](LICENSE).
- The SDL3 renderer adapter is from `demo/sdl3_renderer/nuklear_sdl3_renderer.h` at the same commit.

The embedded default font is distributed with Nuklear. Its attribution and license are included in the upstream header and license file.

SDL3 is an optional system dependency for the GUI target. SDL3 is distributed under the zlib license; see the [SDL license](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt). SDL3 source and binaries are not vendored in this repository.
