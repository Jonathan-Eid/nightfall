#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=../build_support/native_xr_versions.sh
source "$SCRIPT_DIR/../build_support/native_xr_versions.sh"
DOWNLOADS_ONLY=0
STREAM_ONLY=0
if [[ "${1:-}" == "--downloads-only" ]]; then
  DOWNLOADS_ONLY=1
  shift
fi
if [[ "${1:-}" == "--stream-only" && "$DOWNLOADS_ONLY" == 0 ]]; then
  STREAM_ONLY=1
  shift
fi
if [[ $# -ne 0 ]]; then
  echo "Usage: $0 [--downloads-only | --stream-only]" >&2
  exit 2
fi

require_command() {
  command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 1; }
}
for command in curl sha256sum unzip tar git python3; do require_command "$command"; done

CACHE_ROOT="${NIGHTFALL_CI_CACHE:-$PROJECT_ROOT/.build-cache/ci-android}"
DOWNLOAD_DIR="$CACHE_ROOT/downloads"
mkdir -p "$DOWNLOAD_DIR"

build_stream() {
  VCPKG_BASELINE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["default-registry"]["baseline"])' "$PROJECT_ROOT/addons/nightfall-stream/vcpkg-configuration.json")"
  VCPKG_ROOT="${VCPKG_ROOT:-$CACHE_ROOT/vcpkg}"
  export VCPKG_ROOT
  NIGHTFALL_BUILD_JOBS="${NIGHTFALL_BUILD_JOBS:-4}"
  : "${ANDROID_NDK_HOME:?Run --downloads-only first to configure the Android toolchain}"
  require_command cmake
  require_command ninja
  require_command scons
  require_command java
  require_command nproc
  if [[ ! -d "$VCPKG_ROOT/.git" ]]; then
    git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
  fi
  git -C "$VCPKG_ROOT" checkout --detach "$VCPKG_BASELINE"
  if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
  fi

  (cd "$PROJECT_ROOT/addons/nightfall-stream"
    cmake --preset android -DCMAKE_BUILD_TYPE=Release -DVCPKG_OVERLAY_TRIPLETS="$PROJECT_ROOT/vcpkg-overlay/ci-triplets"
    cmake --build build/android --parallel "$NIGHTFALL_BUILD_JOBS")

  stream_lib="$PROJECT_ROOT/addons/nightfall-stream/bin/android/libnightfall-stream.android.template_release.arm64.so"
  [[ -f "$stream_lib" ]] || { echo "Release streaming library was not produced: $stream_lib" >&2; exit 1; }
}
if [[ "$STREAM_ONLY" == 1 ]]; then
  build_stream
  exit 0
fi

# Every downloaded artifact is pinned in android-inputs.sha256. Keep URLs here
# so the clean-runner recipe is reviewable without querying mutable release APIs.
download_checked() {
  local name="$1" url="$2"
  local expected actual file="$DOWNLOAD_DIR/$name"
  expected="$(awk -v n="$name" '$2 == n {print $1}' "$SCRIPT_DIR/android-inputs.sha256")"
  [[ "$expected" =~ ^[0-9a-f]{64}$ ]] || { echo "No SHA-256 pin for $name" >&2; exit 1; }
  if [[ ! -f "$file" ]]; then
    echo "Downloading $name" >&2
    curl --fail --location --retry 4 --retry-all-errors --retry-delay 2 "$url" --output "$file"
  fi
  actual="$(sha256sum "$file" | cut -d' ' -f1)"
  if [[ "$actual" != "$expected" ]]; then
    echo "SHA-256 mismatch for $name: expected $expected, got $actual" >&2
    rm -f "$file"
    exit 1
  fi
  printf '%s\n' "$file"
}

godot_zip="$(download_checked Godot_v4.7-stable_linux.x86_64.zip https://github.com/godotengine/godot/releases/download/4.7-stable/Godot_v4.7-stable_linux.x86_64.zip)"
templates_tpz="$(download_checked Godot_v4.7-stable_export_templates.tpz https://github.com/godotengine/godot/releases/download/4.7-stable/Godot_v4.7-stable_export_templates.tpz)"
vendors_zip="$(download_checked godotopenxrvendorsaddon.zip https://github.com/GodotVR/godot_openxr_vendors/releases/download/5.2.0-stable/godotopenxrvendorsaddon.zip)"

MODEL_BASE="https://github.com/tB0nE/nightfall/releases/download/edgepad-models-2026-10-08"
model_names=(
  zipdepth-base-256-cpu.tflite
  zipdepth-base-256-standard-packed-conv4-reduceconv-edgepad-gpu.tflite
  zipdepth-base-384-cpu.tflite
  zipdepth-base-384-standard-packed-conv4-reduceconv-edgepad-gpu.tflite
  zipdepth-wide-512x288-edgepad-gpu.tflite
)
for name in "${model_names[@]}"; do
  source="$(download_checked "$name" "$MODEL_BASE/$name")"
  mkdir -p "$PROJECT_ROOT/models"
  install -m 0644 "$source" "$PROJECT_ROOT/models/$name"
done

# Verify checked-in Android runtime dependencies too; these must exist in the
# source tree and are intentionally not downloaded by CI.
for name in litert-gpu-nightfall-1.4.2.aar libpyrowave-shared.so; do
  file="$(find "$PROJECT_ROOT" -type f -name "$name" -print -quit)"
  [[ -n "$file" ]] || { echo "Required tracked input missing: $name" >&2; exit 1; }
  expected="$(awk -v n="$name" '$2 == n {print $1}' "$SCRIPT_DIR/android-inputs.sha256")"
  actual="$(sha256sum "$file" | cut -d' ' -f1)"
  [[ "$actual" == "$expected" ]] || { echo "SHA-256 mismatch for $file" >&2; exit 1; }
done

# Install the Meta vendor addon from the pinned official release archive.
vendor_stage="$CACHE_ROOT/vendors-unpack"
rm -rf "$vendor_stage"
mkdir -p "$vendor_stage"
unzip -q "$vendors_zip" -d "$vendor_stage"
vendor_addon="$(find "$vendor_stage" -type f -path '*/addons/godotopenxrvendors/plugin.gdextension' -print -quit | sed 's#/plugin.gdextension$##')"
[[ -n "$vendor_addon" ]] || { echo "Pinned vendor archive lacks addons/godotopenxrvendors/plugin.gdextension" >&2; exit 1; }
for path in .bin/android/release/godotopenxr-meta-release.aar .bin/android/template_release/arm64/libgodotopenxrvendors.so; do
  [[ -f "$vendor_addon/$path" ]] || { echo "Pinned vendor archive lacks $path" >&2; exit 1; }
done
mkdir -p "$PROJECT_ROOT/addons/godotopenxrvendors"
cp -a "$vendor_addon/." "$PROJECT_ROOT/addons/godotopenxrvendors/"

# Validate the template archive and extract only Godot's Android source export
# template. The source template contains the Gradle project used by Android
# exports; the platform-specific android_release.apk is not needed by this path.
template_entry="$(unzip -Z1 "$templates_tpz" | awk '/(^|\/)android_source\.zip$/ && !found {print; found=1}')"
[[ -n "$template_entry" ]] || {
  echo "Godot template archive does not contain android_source.zip" >&2; exit 1;
}
template_dir="${XDG_DATA_HOME:-$HOME/.local/share}/godot/export_templates/4.7.stable"
mkdir -p "$template_dir"
android_template="$template_dir/android_source.zip"
if [[ ! -f "$android_template" ]]; then
  unzip -p "$templates_tpz" "$template_entry" > "$CACHE_ROOT/android_source.zip"
  unzip -tq "$CACHE_ROOT/android_source.zip" >/dev/null
  unzip -Z1 "$CACHE_ROOT/android_source.zip" | grep '^build.gradle$' >/dev/null || {
    echo "Godot Android source template lacks build.gradle at its archive root" >&2; exit 1;
  }
  install -m 0644 "$CACHE_ROOT/android_source.zip" "$android_template"
fi
unzip -tq "$android_template" >/dev/null

GODOT_DIR="$CACHE_ROOT/godot-editor"
mkdir -p "$GODOT_DIR"
unzip -oq "$godot_zip" -d "$GODOT_DIR"
GODOT_EDITOR="$GODOT_DIR/Godot_v4.7-stable_linux.x86_64"
[[ -x "$GODOT_EDITOR" ]] || { echo "Godot editor executable not found in official archive" >&2; exit 1; }

ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[[ -n "$ANDROID_HOME" && -d "$ANDROID_HOME" ]] || { echo "Set ANDROID_HOME (Android SDK root)" >&2; exit 1; }
ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-$ANDROID_HOME/ndk/29.0.14206865}}"
[[ -d "$ANDROID_NDK_HOME" ]] || { echo "Android NDK not found: $ANDROID_NDK_HOME" >&2; exit 1; }
ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
ndk_revision="$(sed -n 's/^Pkg.Revision *= *//p' "$ANDROID_NDK_HOME/source.properties" | head -n1)"
[[ "$ndk_revision" == "$NIGHTFALL_ANDROID_NDK_VERSION" ]] || {
  echo "Android NDK revision mismatch: expected $NIGHTFALL_ANDROID_NDK_VERSION, got ${ndk_revision:-unknown}" >&2; exit 1;
}
[[ -f "$ANDROID_HOME/platforms/android-36/android.jar" ]] || { echo "Android SDK platform 36 is missing" >&2; exit 1; }
[[ -x "$ANDROID_HOME/build-tools/36.1.0/aapt" ]] || { echo "Android SDK build-tools 36.1.0 are missing" >&2; exit 1; }
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v java)")")")}"
[[ -d "$JAVA_HOME" ]] || { echo "Java home not found: $JAVA_HOME" >&2; exit 1; }

# Godot reads Android SDK/JDK locations from EditorSettings rather than these
# environment variables. Create its per-user settings file for the headless CI
# process, with Godot string escaping for portable paths.
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
mkdir -p "$CONFIG_HOME/godot"
python3 - "$CONFIG_HOME/godot/editor_settings-4.7.tres" "$ANDROID_HOME" "$JAVA_HOME" <<'PY'
import pathlib, sys
def godot_string(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'
path = pathlib.Path(sys.argv[1])
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text(
    '[gd_resource type="EditorSettings" format=3]\n\n[resource]\n'
    'export/android/android_sdk_path = ' + godot_string(sys.argv[2]) + '\n'
    'export/android/java_sdk_path = ' + godot_string(sys.argv[3]) + '\n',
    encoding='utf-8',
)
PY

VCPKG_BASELINE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["default-registry"]["baseline"])' "$PROJECT_ROOT/addons/nightfall-stream/vcpkg-configuration.json")"
[[ "$VCPKG_BASELINE" == 026ac03d99ca88e45b5f260267b22e103ec05ec0 ]] || {
  echo "Unexpected vcpkg baseline: $VCPKG_BASELINE" >&2; exit 1;
}
VCPKG_ROOT="${VCPKG_ROOT:-$CACHE_ROOT/vcpkg}"

export ANDROID_HOME ANDROID_SDK_ROOT="$ANDROID_HOME" ANDROID_NDK_HOME ANDROID_NDK_ROOT
export ANDROID_ABI=arm64-v8a VCPKG_ROOT VCPKG_DEFAULT_TRIPLET=arm64-android
export JAVA_HOME NIGHTFALL_JAVA_HOME="$JAVA_HOME"
export NIGHTFALL_GODOT_EDITOR="$GODOT_EDITOR"
export NIGHTFALL_GODOT_TEMPLATE_DIR="$template_dir"
export NIGHTFALL_ANDROID_SOURCE_TEMPLATE="$android_template"
export NIGHTFALL_NATIVE_XR_CACHE="${NIGHTFALL_NATIVE_XR_CACHE:-$CACHE_ROOT/native-xr}"
export NIGHTFALL_BUILD_JOBS="${NIGHTFALL_BUILD_JOBS:-4}"

if [[ -n "${GITHUB_ENV:-}" ]]; then
  {
    printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\nANDROID_NDK_ROOT=%s\n' "$ANDROID_HOME" "$ANDROID_HOME" "$ANDROID_NDK_HOME" "$ANDROID_NDK_ROOT"
    printf 'ANDROID_ABI=arm64-v8a\nVCPKG_ROOT=%s\nVCPKG_DEFAULT_TRIPLET=arm64-android\n' "$VCPKG_ROOT"
    printf 'NIGHTFALL_GODOT_EDITOR=%s\nNIGHTFALL_GODOT_TEMPLATE_DIR=%s\nNIGHTFALL_ANDROID_SOURCE_TEMPLATE=%s\n' "$GODOT_EDITOR" "$template_dir" "$android_template"
    printf 'NIGHTFALL_NATIVE_XR_CACHE=%s\nNIGHTFALL_BUILD_JOBS=%s\n' "$NIGHTFALL_NATIVE_XR_CACHE" "$NIGHTFALL_BUILD_JOBS"
    printf 'JAVA_HOME=%s\nNIGHTFALL_JAVA_HOME=%s\n' "$JAVA_HOME" "$JAVA_HOME"
  } >> "$GITHUB_ENV"
fi

if [[ "$DOWNLOADS_ONLY" -eq 1 ]]; then
  echo "Pinned inputs downloaded, verified, and staged; native builds skipped."
  exit 0
fi

bash "$SCRIPT_DIR/native_xr_cache.sh" ensure
build_stream
echo "Android native build inputs are ready."
