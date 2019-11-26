TEMPLATE = app
CONFIG +=  c++11

QMAKE_CXXFLAGS += \
    -Wno-deprecated-declarations \
    -fpermissive

SOURCES +=  main.cpp \
    player.cpp

INCLUDEPATH += \
    $$PWD/dependentlib/ffmpeg/include \
    $$PWD/dependentlib/SDL2/include

Debug:DESTDIR = $$PWD/bin/debug
Release:DESTDIR = $$PWD/bin/release

LIBS += -L$$PWD/dependentlib/SDL2/lib -lSDL2main -lSDL2
LIBS += -L$$PWD/dependentlib/ffmpeg/lib -lavcodec -lavformat -lavdevice -lavutil -lswscale -lswresample

HEADERS += \
    player.h


