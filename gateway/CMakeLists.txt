cmake_minimum_required(VERSION 3.14)
project(MeshtasticGatewayPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME meshtastic_gateway
    SOURCES
        src/meshtastic_gateway_interface.h
        src/meshtastic_gateway_plugin.h
        src/meshtastic_gateway_plugin.cpp
)

# --- Meshtastic serial transport: QtSerialPort + generated protobufs ---------
# Generate the meshtastic protobuf C++ at build time (so protoc matches the build's libprotobuf),
# from the vendored .proto files. The radio decrypts on-air traffic, so over USB we only ever
# handle plaintext — no crypto here, just the StreamAPI message types.
find_package(Protobuf REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS SerialPort)

set(MESH_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
file(MAKE_DIRECTORY ${MESH_GEN_DIR})
file(GLOB MESH_PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/proto/meshtastic/*.proto)
list(APPEND MESH_PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/proto/nanopb.proto)

protobuf_generate(
    TARGET meshtastic_gateway_module_plugin
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${MESH_GEN_DIR}
    PROTOS ${MESH_PROTOS}
)

target_include_directories(meshtastic_gateway_module_plugin PRIVATE ${MESH_GEN_DIR})
target_link_libraries(meshtastic_gateway_module_plugin PRIVATE
    protobuf::libprotobuf
    Qt6::SerialPort
)
