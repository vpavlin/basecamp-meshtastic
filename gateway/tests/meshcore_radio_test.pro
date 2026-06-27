QT = core serialport
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += ../src
HEADERS += ../src/mesh_radio.h ../src/meshcore_radio.h ../src/meshcore_protocol.h
SOURCES += ../src/meshcore_radio.cpp ../src/meshcore_protocol.cpp test_meshcore_radio.cpp
TARGET = mcradiotest
