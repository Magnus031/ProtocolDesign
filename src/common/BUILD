cc_library(
    name = "byte_order",
    hdrs = ["byte_order.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "byte_order_test",
    srcs = ["byte_order_test.cc"],
    deps = [
        ":byte_order",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)

cc_library(
    name = "message_handler",
    srcs = ["message_handler.cpp"],
    hdrs = ["message_handler.h"],
    deps = [
        "//src/protocol:protocol",
        "//src/protocol:message_parser",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "message_handler_test",
    srcs = ["message_handler_test.cc"],
    deps = [
        ":message_handler",
        "//src/protocol:protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)
