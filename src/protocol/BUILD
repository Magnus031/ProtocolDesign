cc_library(
    name = "protocol",
    srcs = ["protocol.cpp"],
    hdrs = ["protocol.h"],
    deps = ["//src/common:byte_order"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "message_parser",
    srcs = ["message_parser.cpp"],
    hdrs = ["message_parser.h"],
    deps = [":protocol"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "protocol_test",
    srcs = ["protocol_test.cpp"],
    deps = [
        ":protocol",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "message_parser_test",
    srcs = ["message_parser_test.cpp"],
    deps = [
        ":message_parser",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
