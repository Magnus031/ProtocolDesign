# AppHost: outbound TCP client to Gateway, hosts a single business plugin.

cc_library(
    name = "pixel_capture",
    srcs = ["pixel_capture.cpp"],
    hdrs = ["pixel_capture.h"],
    deps = [
        "//src/common:byte_order",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "pixel_capture_test",
    srcs = ["pixel_capture_test.cpp"],
    deps = [
        ":pixel_capture",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)

cc_binary(
    name = "pixel_capture_bench",
    srcs = ["pixel_capture_bench.cpp"],
    deps = [
        ":pixel_capture",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

# Headless fake IUiHost.  Hosts (apphost_bin) link this; plugins do NOT.
cc_library(
    name = "ui_host_impl",
    srcs = ["ui_host_impl.cpp"],
    hdrs = ["ui_host_impl.h"],
    deps = [
        ":pixel_capture",
        "//third_party:GkcSys",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

# dlopen/dlsym wrapper for SA UI plugins.  Hosts only.
cc_library(
    name = "plugin_loader",
    srcs = ["plugin_loader.cpp"],
    hdrs = ["plugin_loader.h"],
    deps = [
        "//third_party:GkcSys",
    ],
    linkopts = ["-ldl"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "apphost",
    srcs = ["apphost.cpp"],
    hdrs = ["apphost.h"],
    deps = [
        ":plugin_loader",
        ":ui_host_impl",
        "//src/common:input_event",
        "//src/common:message_handler",
        "//src/protocol:protocol",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "apphost_bin",
    srcs = ["main.cpp"],
    deps = [
        ":apphost",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

# End-to-end: fake Gateway + apphost subprocess + demo plugin.
cc_test(
    name = "m4_plugin_test",
    srcs = ["m4_plugin_test.cpp"],
    data = [
        ":apphost_bin",
        "//plugins/demo_app:libdemo_app.so",
    ],
    deps = [
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
    timeout = "short",
)

# Network-layer lifecycle: APPHOST_READY, HEARTBEAT echo, CLOSE_SESSION,
# multi-packet ordering.  Inherits the role of the old apphost_test (M2).
cc_test(
    name = "apphost_lifecycle_test",
    srcs = ["apphost_lifecycle_test.cpp"],
    data = [
        ":apphost_bin",
        "//plugins/demo_app:libdemo_app.so",
    ],
    deps = [
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
    timeout = "short",
)

# M5: INPUT_EVENT closed loop via fake Gateway + real apphost_bin + demo plugin.
cc_test(
    name = "m5_input_test",
    srcs = ["m5_input_test.cpp"],
    data = [
        ":apphost_bin",
        "//plugins/demo_app:libdemo_app.so",
    ],
    deps = [
        "//src/common:input_event",
        "//src/protocol:message_parser",
        "//src/protocol:protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
    timeout = "short",
)
