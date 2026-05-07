# app_2048: server-side 2048 game plugin loaded by AppHost via dlopen.
#
# The plugin .so (libapp_2048.so) depends on //third_party:gkc_plugin_runtime
# (which exposes GKC's headers and the GkcGui.cpp / GkcSAMain.cpp / GkcDef.cpp
# textual sources) — those are #include'd at the bottom of app_2048.cpp so the
# plugin gets its own GKC::g_ui_host storage and exports an extern "C"
# _SA_UIMain symbol.  AppHost itself MUST NOT link gkc_plugin_runtime.
#
# The pure board logic lives in :app_2048_logic so that
# :app_2048_logic_test can link it without pulling GKC into the test binary.

cc_library(
    name = "app_2048_logic",
    srcs = ["app_2048_logic.cpp"],
    hdrs = ["app_2048_logic.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "app_2048_logic_test",
    srcs = ["app_2048_logic_test.cpp"],
    deps = [
        ":app_2048_logic",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "libapp_2048.so",
    srcs = ["app_2048.cpp"],
    deps = [
        ":app_2048_logic",
        "//third_party:gkc_plugin_runtime",
        "//third_party:GkcSys",
    ],
    linkshared = True,
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)
