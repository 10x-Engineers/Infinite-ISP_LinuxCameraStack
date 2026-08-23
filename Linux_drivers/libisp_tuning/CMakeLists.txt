cmake_minimum_required(VERSION 3.10)

add_library(infinite_isp_tuning
    src/tuning.cpp
    src/v4l2_backend.cpp
)

target_compile_features(infinite_isp_tuning PUBLIC cxx_std_17)
target_include_directories(infinite_isp_tuning
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

install(TARGETS infinite_isp_tuning DESTINATION lib)
install(DIRECTORY include/infinite_isp DESTINATION include)

if(BUILD_TESTING)
    add_executable(infinite_isp_tuning_test tests/tuning_test.cpp)
    target_link_libraries(infinite_isp_tuning_test PRIVATE infinite_isp_tuning)
    add_test(NAME infinite_isp_tuning_policy COMMAND infinite_isp_tuning_test)
endif()
