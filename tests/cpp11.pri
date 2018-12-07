equals(QT_MAJOR_VERSION, 4) {
    QMAKE_CXXFLAGS += -std=c++11
}

equals(QT_MAJOR_VERSION, 5) {
    CONFIG += c++11
}
