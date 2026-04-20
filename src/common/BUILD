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
