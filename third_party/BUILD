# use to manage the GKC Lib
load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

filegroup(
    name = "gkc_all_srcs",
    srcs = glob(["GKC/**"], exclude=["GKC/.git/**"]),
)

# Linux Release build
cmake(
    name = "GkcSys_linux_release",
    cache_entries = {
        "CMAKE_BUILD_TYPE": "Release",
    },
    lib_source = ":gkc_all_srcs",
    out_lib_dir = "main/assemblies",
    out_include_dir = "main/dev/include/GKC",
    # cmake installs libGkcSys.so (SONAME libGkcSys.so.1) and libGkcSys.so.1.
    # Both must be listed so Bazel includes the versioned file in test runfiles.
    out_shared_libs = ["libGkcSys.so", "libGkcSys.so.1"],
    visibility = ["//visibility:public"],
)

# Linux Debug build
cmake(
    name = "GkcSys_linux_debug",
    cache_entries = {
        "CMAKE_BUILD_TYPE": "Debug",
    },
    lib_source = ":gkc_all_srcs",
    out_lib_dir = "main/assemblies",
    out_include_dir = "main/dev/include/GKC",
    out_shared_libs = ["libGkcSys.so", "libGkcSys.so.1"],
    visibility = ["//visibility:public"],
)

# Build with tests enabled (Debug mode)
cmake(
    name = "gkc_with_tests",
    cache_entries = {
        "CMAKE_BUILD_TYPE": "Debug",
    },
    lib_source = ":gkc_all_srcs",
    out_lib_dir = "main/assemblies",
    out_include_dir = "main/dev/include/GKC",
    out_shared_libs = ["libGkcSys.so"],
    # Build all targets including tests
    targets = [],
    # Run ctest inside the sandbox before cleanup
    postfix_script = "ctest --output-on-failure",
    visibility = ["//visibility:public"],
)

# Default Linux target (uses Release)
alias(
    name = "GkcSys_linux",
    actual = ":GkcSys_linux_release",
    visibility = ["//visibility:public"],
)

# Windows: use pre-built library from GKC_BUILD
cc_import(
    name = "GkcSys_windows",
    hdrs = glob([
        "GKC/public/include/**/*.h",
        "GKC/RT/GkcSys/public/**/*.h",
    ]),
    includes = [
        "GKC/public/include",
        "GKC/RT/GkcSys/public",
    ],
    shared_library = "GKC_BUILD/release/bin/Release/GkcSys.dll",
    interface_library = "GKC_BUILD/release/bin/Release/GkcSys.lib",
    visibility = ["//visibility:public"],
)

# Unified entry point
alias(
    name = "GkcSys",
    actual = select({
        "@platforms//os:linux":   ":GkcSys_linux",
        "@platforms//os:windows": ":GkcSys_windows",
    }),
    visibility = ["//visibility:public"],
)

# gkc_plugin_runtime: header-only target that exposes GKC public headers
# AND the GKC source files (GkcDef.cpp / GkcSAMain.cpp / GkcGui.cpp / system/*.cpp)
# as textual_hdrs so plugin authors can follow the GKC convention of including
# them at the bottom of their main .cpp:
#
#   namespace GKC { class ProgramEntryPoint { ... GuiMain(args) {...} }; }
#   #include "base/GkcDef.cpp"
#   #include "base/GkcSAMain.cpp"
#   #include "base/GkcGui.cpp"
#
# This gives each plugin .so its own copy of GKC::g_ui_host and an exported
# extern "C" _SA_UIMain symbol that the host can dlsym.
#
# Plugins should also depend on :GkcSys for runtime symbols (heap, time,
# IoPool, real IUiToplevel impls, etc.).  Hosts (apphost_bin) MUST NOT depend
# on this target — it would give the host its own duplicate g_ui_host that
# the plugin would never see.
cc_library(
    name = "gkc_plugin_runtime",
    hdrs = glob([
        "GKC/public/include/**/*.h",
    ]),
    textual_hdrs = glob([
        "GKC/public/include/**/*.cpp",
    ]),
    includes = ["GKC/public/include"],
    visibility = ["//visibility:public"],
)