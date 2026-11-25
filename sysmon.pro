TEMPLATE = lib
TARGET = sysmon
QT += widgets
CONFIG += plugin c++17
CONFIG -= app_bundle
INCLUDEPATH += /usr/include/lxqt

HEADERS += sysmonplugin.h
SOURCES += sysmonplugin.cpp
DESTDIR = $$PWD/build
