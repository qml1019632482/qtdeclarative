TEMPLATE = lib
CONFIG += qt plugin
QT += qml quick

TARGET = qmltextballoonplugin

HEADERS += TextBalloonPlugin/plugin.h \
    textballoon.h

SOURCES += textballoon.cpp

RESOURCES += painteditem.qrc

DESTDIR = TextBalloonPlugin

target.path = $$[QT_INSTALL_EXAMPLES]/quick/customitems/painteditem/TextBalloonPlugin
qmldir.files = TextBalloonPlugin/qmldir
qmldir.path = $$[QT_INSTALL_EXAMPLES]/quick/customitems/painteditem/TextBalloonPlugin

INSTALLS += qmldir target
