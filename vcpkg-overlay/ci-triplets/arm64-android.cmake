# CI exports only release APKs. Avoid compiling every dependency twice.
include("${CMAKE_CURRENT_LIST_DIR}/../triplets/arm64-android.cmake")
set(VCPKG_BUILD_TYPE release)
