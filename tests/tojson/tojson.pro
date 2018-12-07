include($$PWD/../cpp11.pri)

CONFIG += testcase parallel_test
QT = core testlib

SOURCES += tst_tojson.cpp
INCLUDEPATH += ../../src
msvc: POST_TARGETDEPS = ../../lib/tinycbor.lib
else: POST_TARGETDEPS += ../../lib/libtinycbor.a
LIBS += $$POST_TARGETDEPS
