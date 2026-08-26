# Local patches to submodules

Changes here live in a submodule's working tree, so they do **not** appear in
this repository's `git diff` and are destroyed by `git submodule update
--force` or a fresh clone. They are checked in as patch files so they can be
recovered.

## dynamorio-gnu99-type-probes.patch

Fixes `unknown type name 'bool'` when building DynamoRIO with GCC >= 15.

`deps/dynamorio/CMakeLists.txt` runs `CHECK_TYPE_SIZE(bool ...)` with the
compiler's default standard. On GCC >= 15 that default is C23, where `bool` is
a keyword, so the probe succeeds, DynamoRIO sets `DR_DO_NOT_DEFINE_bool` and
skips its own typedef -- but the actual sources build with `-std=gnu99`, where
`bool` does not exist. Every gnu99 translation unit then fails to compile. The
patch pins `CMAKE_REQUIRED_FLAGS` to `-std=gnu99` around the type probes.

Apply with:

    git -C src/deps/dynamorio apply ../../deps/patches/dynamorio-gnu99-type-probes.patch

Changing this requires deleting the cached `DR_DO_NOT_DEFINE_bool` and
`HAVE_DR_DO_NOT_DEFINE_bool` entries from `CMakeCache.txt` so the probes re-run.

Verify it is applied with `git -C src/deps/dynamorio diff --stat`.
