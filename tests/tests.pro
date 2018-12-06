TEMPLATE = subdirs
SUBDIRS = parser encoder c90 cpp tojson
msvc: SUBDIRS -= tojson
QMAKE_CXXFLAGS += -std=c++11
