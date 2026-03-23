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

# =========================================================================
# Build Intel's libbid.a using their provided makefile
# =========================================================================
set(LIBBID_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libbid")
set(LIBBID_ARCHIVE "${LIBBID_DIR}/libbid.a")

# Tell CMake how to build libbid.a by calling Intel's exact make command
add_custom_command(
    OUTPUT ${LIBBID_ARCHIVE}
    COMMAND make CC=${CMAKE_C_COMPILER} CALL_BY_REF=0 GLOBAL_RND=0 GLOBAL_FLAGS=0 UNCHANGED_BINARY_FLAGS=0
    COMMAND rm -f *.o  # Clean up object files as requested by the README
    WORKING_DIRECTORY ${LIBBID_DIR}
    COMMENT "Building Intel Decimal Floating-Point Library (libbid.a)..."
    VERBATIM
)

# Create a custom target to trigger the build
add_custom_target(build_libbid DEPENDS ${LIBBID_ARCHIVE})

# Wrap the generated .a file in a CMake imported target so we can link it
add_library(bid STATIC IMPORTED)
set_target_properties(bid PROPERTIES IMPORTED_LOCATION ${LIBBID_ARCHIVE})
add_dependencies(bid build_libbid)
# =========================================================================

# 5. Add the newly generated protobuf sources to the library target
add_library(twsapi STATIC ${SOURCES} ${PROTO_SRCS} ${PROTO_HDRS})

# Ensure libbid builds BEFORE we try to link twsapi
add_dependencies(twsapi build_libbid)

# 6. Include paths
# We must include the CMAKE_CURRENT_BINARY_DIR because that is where 
# CMake places the newly generated protobuf headers during the build process.
target_include_directories(twsapi PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/proto
)

# Link the protobuf library publicly
target_link_libraries(twsapi PUBLIC protobuf::libprotobuf bid)

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