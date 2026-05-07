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
# Reset-button geometry lives in :app_2048_view so the integration test can
# click the same button rect the plugin draws.

cc_library(
    name = "app_2048_logic",
    srcs = ["app_2048_logic.cpp"],
    hdrs = ["app_2048_logic.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "app_2048_view",
    hdrs = ["app_2048_view.h"],
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

cc_test(
    name = "app_2048_view_test",
    srcs = ["app_2048_view_test.cpp"],
    deps = [
        ":app_2048_view",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "libapp_2048.so",
    srcs = ["app_2048.cpp"],
    deps = [
        ":app_2048_logic",
        ":app_2048_view",
        "//third_party:gkc_plugin_runtime",
        "//third_party:GkcSys",
    ],
    linkshared = True,
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

# AppHost-level integration test: spawns apphost_bin loading libapp_2048.so,
# sends MOUSE_LEFT_DOWN at the reset-button centre, and asserts a fresh
# full-window PIXEL_DATA frame comes back.  This is the
# "AppHost/plugin-level or integration test" required by docs/design/2048.md
# under "Acceptance Criteria > Automated".
cc_test(
    name = "app_2048_input_test",
    srcs = ["app_2048_input_test.cpp"],
    data = [
        ":libapp_2048.so",
        "//src/apphost:apphost_bin",
    ],
    deps = [
        ":app_2048_view",
        "//src/common:input_event",
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
    timeout = "short",
)
