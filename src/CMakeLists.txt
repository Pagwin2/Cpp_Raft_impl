# based heavily on https://gitlab.com/CLIUtils/modern-cmake/-/blob/master/examples/extended-project/src/CMakeLists.txt
set(HEADER_LIST "${raft_cpp_impl_SOURCE_DIR}/include/raft2/lib.hpp")

add_library(raft_interface lib2.cpp ${HEADER_LIST})

# We need this directory, and users of our library will need it too
target_include_directories(raft_interface PUBLIC ../include)

# target_link_libraries(raft_interface PRIVATE Boost::boost)

target_compile_features(raft_interface PUBLIC cxx_std_20)

# IDEs should put the headers in a nice place
source_group(
  TREE "${PROJECT_SOURCE_DIR}/include"
  PREFIX "Header Files"
  FILES ${HEADER_LIST})
