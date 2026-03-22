# 1. Find Protobuf FIRST so we have the compiler (protoc) available
find_package(Protobuf REQUIRED)

# Find the raw .proto files inside the proto directory we just copied
file(GLOB PROTO_FILES "proto/*.proto")

# 3. Tell CMake to generate fresh .pb.cc and .pb.h files using your system's protoc
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# 4. Include ONLY the standard cpp files
file(GLOB SOURCES "*.cpp")
file(GLOB HEADERS "*.h")

set(twsapi_VERSION 9.79.02)

# 5. Add the newly generated protobuf sources to the library target
add_library(twsapi STATIC ${SOURCES} ${PROTO_SRCS} ${PROTO_HDRS})

# 6. Include paths
# We must include the CMAKE_CURRENT_BINARY_DIR because that is where 
# CMake places the newly generated protobuf headers during the build process.
target_include_directories(twsapi PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/proto
)

# Link the protobuf library publicly
target_link_libraries(twsapi PUBLIC protobuf::libprotobuf)

# On Windows (including MinGW), link against Winsock 2
if( WIN32 )
    target_link_libraries( twsapi PUBLIC ws2_32 )
endif()

if( MSVC )
    target_compile_options( twsapi PRIVATE /W3 )
else()
    target_compile_options( twsapi PRIVATE -Wall -Wextra -pedantic -Wno-unused-parameter )
endif()

set_target_properties(twsapi
    PROPERTIES
    PREFIX "lib"
    DEBUG_POSTFIX "d"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    PUBLIC_HEADER "${HEADERS}"
)

install(TARGETS twsapi 
    EXPORT twsapi-targets 
    PUBLIC_HEADER DESTINATION include/twsapi 
    ARCHIVE DESTINATION lib
)