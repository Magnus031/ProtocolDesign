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
