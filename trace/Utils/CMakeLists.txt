project("Utils")

file(GLOB_RECURSE sources "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
#file(GLOB_RECURSE fakeDlopen "${CMAKE_CURRENT_SOURCE_DIR}/fakeDlopen/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/fakeDlopen/*.c")
file(GLOB_RECURSE fakeDlopen "${CMAKE_CURRENT_SOURCE_DIR}/dlfc/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/dlfc/*.c")

# 编译 utils 为静态库
add_library(utils STATIC
        ${sources}
        ${fakeDlopen}
        )

target_include_directories(utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
#target_include_directories(utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/fakeDlopen/)
target_include_directories(utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/dlfc/)
