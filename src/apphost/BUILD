# AppHost: IoPool-based TCP server that captures pixel data and handles input events.

cc_library(
    name = "pixel_buffer",
    srcs = ["pixel_buffer.cpp"],
    hdrs = ["pixel_buffer.h"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "pixel_capture",
    srcs = ["pixel_capture.cpp"],
    hdrs = ["pixel_capture.h"],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "apphost",
    srcs = ["apphost.cpp"],
    hdrs = ["apphost.h"],
    deps = [
        ":pixel_buffer",
        ":pixel_capture",
        "//src/common:message_handler",
        "//src/protocol:protocol",
        "//third_party:GkcSys",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "apphost_bin",
    srcs = ["main.cpp"],
    deps = [":apphost"],
    copts = ["-std=c++17"],
)

cc_test(
    name = "apphost_test",
    srcs = ["apphost_test.cpp"],
    deps = [
        ":apphost",
        "//src/protocol:protocol",
        "//src/protocol:message_parser",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)
