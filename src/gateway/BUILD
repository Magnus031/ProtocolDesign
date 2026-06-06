cc_library(
    name = "session_manager",
    srcs = ["session_manager.cpp"],
    hdrs = ["session_manager.h"],
    deps = ["//third_party:GkcSys"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "process_manager",
    srcs = ["process_manager.cpp"],
    hdrs = ["process_manager.h"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "gateway",
    srcs = ["gateway.cpp"],
    hdrs = ["gateway.h"],
    deps = [
        ":process_manager",
        ":session_manager",
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
        "//third_party:GkcSys",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "gateway_bin",
    srcs = ["main.cpp"],
    deps = [":gateway"],
    copts = ["-std=c++17"],
)

cc_binary(
    name = "gateway_forward_bench",
    srcs = ["gateway_forward_bench.cpp"],
    deps = ["//src/protocol:protocol"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "fake_e2e_client_bench",
    srcs = ["fake_e2e_client_bench.cpp"],
    deps = [
        ":gateway",
        "//src/common:input_event",
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "gateway_test",
    srcs = ["gateway_test.cpp"],
    deps = [
        ":gateway",
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)

cc_test(
    name = "gateway_lifecycle_test",
    srcs = ["gateway_lifecycle_test.cpp"],
    data = ["//src/apphost:apphost_bin"],
    deps = [
        ":gateway",
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)

cc_test(
    name = "gateway_m4_plugin_test",
    srcs = ["gateway_m4_plugin_test.cpp"],
    data = [
        "//plugins/demo_app:libdemo_app.so",
        "//src/apphost:apphost_bin",
    ],
    deps = [
        ":gateway",
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)

# M5: real Gateway routes INPUT_EVENT body unchanged; AppHost emits new
# PIXEL_DATA driven by demo_app DoMouse/DoKeyboard.
cc_test(
    name = "gateway_m5_input_test",
    srcs = ["gateway_m5_input_test.cpp"],
    data = [
        "//plugins/demo_app:libdemo_app.so",
        "//src/apphost:apphost_bin",
    ],
    deps = [
        ":gateway",
        "//src/common:input_event",
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)
