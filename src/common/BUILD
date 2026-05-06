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

# INPUT_EVENT body pack/parse helper (M5).  Exposes GKC::UiMessage* in its
# header, so the public deps include //third_party:GkcSys.
cc_library(
    name = "input_event",
    srcs = ["input_event.cpp"],
    hdrs = ["input_event.h"],
    deps = [
        ":byte_order",
        "//third_party:GkcSys",
    ],
    copts = ["-std=c++17"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "input_event_test",
    srcs = ["input_event_test.cc"],
    deps = [
        ":input_event",
        "//third_party:GkcSys",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
    copts = ["-std=c++17"],
)
