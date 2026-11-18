#!/usr/bin/env bash
set -euo pipefail
PROJECT_ROOT="${NIGHTFALL_CI_PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
source "$PROJECT_ROOT/tools/build_support/native_xr_versions.sh"
CACHE_ROOT="${NIGHTFALL_CI_CACHE:-$PROJECT_ROOT/.build-cache/ci-android}"
NATIVE_ROOT="${NIGHTFALL_NATIVE_XR_CACHE:-$CACHE_ROOT/native-xr}"
ARCHIVE="$CACHE_ROOT/native-xr-release.tar.gz"
MANIFEST="$NATIVE_ROOT/ci-release.sha256"
required=(
  "templates/$NIGHTFALL_GODOT_TEMPLATE_VERSION/android_release_arm64.so"
  "templates/$NIGHTFALL_GODOT_TEMPLATE_VERSION/android_release_arm64.so.build-stamp"
  godot-cpp/bin/libgodot-cpp.android.template_release.arm64.a
  godot-cpp/extension_api.json
  godot-cpp/gdextension/gdextension_interface.json
  godot-cpp/gen/include/godot_cpp/classes/open_xr_extension_wrapper_extension.hpp
  godot/thirdparty/openxr/include/openxr/openxr.h
  godot-cpp-api.version
  godot-patches.sha256
)
fingerprint() {
  (
    cd "$PROJECT_ROOT"
    sha256sum tools/build_support/native_xr_versions.sh \
      tools/build_support/bootstrap_native_xr.sh tools/ci/native_xr_cache.sh
    for patch in "${NIGHTFALL_ENGINE_PATCH_NAMES[@]}"; do sha256sum "patches/$patch"; done
  ) | sha256sum | cut -d' ' -f1
}
manifest() {
  printf 'inputs %s\n' "$(fingerprint)"
  (cd "$NATIVE_ROOT" && sha256sum "${required[@]}")
}
valid() {
  local file
  [[ -f "$MANIFEST" ]] || return 1
  for file in "${required[@]}"; do [[ -s "$NATIVE_ROOT/$file" ]] || return 1; done
  nightfall_validate_runtime_build "$PROJECT_ROOT" \
    "$NATIVE_ROOT/templates/$NIGHTFALL_GODOT_TEMPLATE_VERSION/android_release_arm64.so" || return 1
  [[ "$(cat "$NATIVE_ROOT/godot-cpp-api.version")" == "Godot $NIGHTFALL_PINNED_GODOT_COMMIT / godot-cpp $NIGHTFALL_PINNED_GODOT_CPP_COMMIT" ]] || return 1
  cmp -s "$MANIFEST" <(manifest)
}
case "${1:-}" in
  fingerprint) fingerprint ;;
  check) valid ;;
  ensure)
    if valid; then
      echo 'Reusing verified compiled native-XR release toolchain; no engine rebuild.'
    else
      echo 'Compiled native-XR cache is missing or invalid; building release toolchain.'
      NIGHTFALL_NATIVE_XR_CACHE="$NATIVE_ROOT" bash "$PROJECT_ROOT/tools/build_support/bootstrap_native_xr.sh" --release-only
      manifest > "$MANIFEST"
      valid || { echo 'Native-XR toolchain validation failed' >&2; exit 1; }
    fi
    ;;
  pack)
    valid || { echo 'Refusing to cache an incomplete native-XR toolchain' >&2; exit 1; }
    mkdir -p "$CACHE_ROOT"
    # Keep sources, generated headers, static bindings and runtimes, but omit
    # compiler object files. A validated warm hit skips SCons entirely.
    tar --exclude='*.o' --exclude='*.os' -czf "$ARCHIVE.tmp" -C "$NATIVE_ROOT" .
    mv "$ARCHIVE.tmp" "$ARCHIVE"
    ;;
  restore)
    if [[ -f "$ARCHIVE" ]]; then
      mkdir -p "$NATIVE_ROOT"
      tar -xzf "$ARCHIVE" -C "$NATIVE_ROOT"
      valid || { echo 'Restored native-XR cache failed validation' >&2; exit 1; }
      echo 'Restored compiled native-XR release toolchain.'
    fi
    ;;
  *) echo "Usage: $0 fingerprint|check|ensure|pack|restore" >&2; exit 2 ;;
esac
