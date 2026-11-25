cmake_minimum_required(VERSION 3.21)
project(qmlhello LANGUAGES CXX)
find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.8)
qt_add_executable(qmlhello main.cpp)
qt_add_qml_module(qmlhello URI NcHello VERSION 1.0 QML_FILES Main.qml)
# static Qt: platform plugin + NetCast window integration
qt_import_plugins(qmlhello INCLUDE Qt::QEglFSIntegrationPlugin Qt::QEglFSNetcastIntegrationPlugin)
target_link_libraries(qmlhello PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick)
