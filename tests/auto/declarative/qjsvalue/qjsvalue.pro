load(qttest_p4)
QT += declarative widgets
SOURCES  += tst_qjsvalue.cpp
HEADERS  += tst_qjsvalue.h

win32-msvc* {
    # With -O2, MSVC takes up to 24 minutes to compile this test!
    QMAKE_CXXFLAGS_RELEASE -= -O1 -O2
    QMAKE_CXXFLAGS_RELEASE += -Od
}
#temporary
CONFIG += insignificant_test
