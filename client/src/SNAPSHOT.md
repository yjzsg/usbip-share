# Client source provenance

This directory is a snapshot of the fork's working tree, taken so the repo
contains buildable client source instead of only a patch.

| | |
|---|---|
| upstream | https://github.com/vadimgrn/usbip-win2 |
| upstream baseline | `74f5a7fa` (see `../changes.patch`) |
| fork branch | `adapt-v098` |
| snapshot HEAD | `657ae2b530d9251b7641e68e57afec5cfe7a8a14` |
| uncommitted files at snapshot time | 13 |

`vcpkg` is an upstream git submodule (`.gitmodules`, `ignore = all`) and is
intentionally not vendored here. To build, initialise it yourself:

```sh
git submodule update --init --recursive
```

Then open `usbip_win2.slnx` (Visual Studio) or use the MSBuild command in
`../README.md`.
