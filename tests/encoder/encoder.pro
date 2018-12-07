include($$PWD/../cpp11.pri)
SOURCES += tst_encoder.cpp

CONFIG += testcase parallel_test
QT = core testlib

INCLUDEPATH += ../../src
msvc: POST_TARGETDEPS = ../../lib/tinycbor.lib
else: POST_TARGETDEPS += ../../lib/libtinycbor.a
LIBS += $$POST_TARGETDEPS
