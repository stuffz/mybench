# One executable per tst_*.cpp, each its own CTest case, laid out to mirror the
# sources it covers: tests/editor/tst_sqlscan.cpp covers src/editor/sqlscan.cpp.
#
# Each test compiles the implementation under test and nothing else. The extra
# arguments to add_gui_test are that unit's real dependency closure, so a unit
# that can only be tested with half the app linked in has to say so here.
#
# AUTOMOC comes from the parent scope, and is what QTest needs: every test
# declares a Q_OBJECT class and includes the moc output at the end of its own
# file.

find_package(Qt6 REQUIRED COMPONENTS Test)

set(GUI_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
# theme.cpp loads its fonts and the combo chevron from the resource system,
# so anything that links it needs the qrc compiled in too.
set(GUI_QRC "${CMAKE_CURRENT_SOURCE_DIR}/../resources.qrc")

set(GUI_TEST_TARGETS "")

function(add_gui_test directory name)
    add_executable(tst_${name} "${directory}/tst_${name}.cpp" ${ARGN})
    target_include_directories(tst_${name} PRIVATE "${GUI_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(tst_${name} PRIVATE Qt6::Test)
    if(UNIX AND NOT APPLE AND HAVE_NO_DIRECT_EXTERN_ACCESS)
        target_compile_options(tst_${name} PRIVATE -mno-direct-extern-access)
    endif()
    # Named for where it sits, so `ctest -R editor` runs one layer of them.
    add_test(NAME ${directory}/${name} COMMAND tst_${name})
    # The build container has no display, and the widget tests need a platform
    # plugin either way; offscreen is the one that needs no X server.
    set_tests_properties(${directory}/${name} PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
    set(GUI_TEST_TARGETS ${GUI_TEST_TARGETS} tst_${name} PARENT_SCOPE)
endfunction()

add_gui_test(editor sqlscan "${GUI_SRC}/editor/sqlscan.cpp")
add_gui_test(ui fmt "${GUI_SRC}/ui/fmt.cpp")
add_gui_test(ui widgets "${GUI_SRC}/ui/widgets.cpp" "${GUI_SRC}/ui/switchbox.cpp"
             "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(views processlistview
    "${GUI_SRC}/views/processlistview.cpp" "${GUI_SRC}/views/panelbase.cpp"
    "${GUI_SRC}/dialogs/querydialog.cpp" "${GUI_SRC}/editor/sqlhighlighter.cpp"
    "${GUI_SRC}/ui/tableutil.cpp" "${GUI_SRC}/ui/switchbox.cpp"
    "${GUI_SRC}/ui/widgets.cpp" "${GUI_SRC}/app/api.cpp"
    "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(views graphview
    "${GUI_SRC}/views/graphview.cpp" "${GUI_SRC}/views/graphcanvas.cpp"
    "${GUI_SRC}/editor/sqlscan.cpp" "${GUI_SRC}/ui/switchbox.cpp"
    "${GUI_SRC}/ui/widgets.cpp" "${GUI_SRC}/app/api.cpp"
    "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(views simplepanels
    "${GUI_SRC}/views/usersview.cpp" "${GUI_SRC}/views/serverinfoview.cpp"
    "${GUI_SRC}/views/innodbview.cpp" "${GUI_SRC}/views/historyview.cpp"
    "${GUI_SRC}/views/panelbase.cpp"
    "${GUI_SRC}/ui/tableutil.cpp" "${GUI_SRC}/ui/widgets.cpp"
    "${GUI_SRC}/app/api.cpp" "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(views statcard "${GUI_SRC}/views/statcard.cpp" "${GUI_SRC}/ui/widgets.cpp"
             "${GUI_SRC}/app/icons.cpp" "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(editor resultmodel "${GUI_SRC}/editor/resultmodel.cpp" "${GUI_SRC}/app/api.cpp")
add_gui_test(editor sqleditor "${GUI_SRC}/editor/sqleditor.cpp"
             "${GUI_SRC}/editor/sqlhighlighter.cpp" "${GUI_SRC}/editor/sqlscan.cpp"
             "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(editor resultgrid "${GUI_SRC}/editor/resultgrid.cpp"
             "${GUI_SRC}/editor/resultmodel.cpp" "${GUI_SRC}/app/api.cpp"
             "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(ui tableutil "${GUI_SRC}/ui/tableutil.cpp")
# tabs.h is header-only, so there is no implementation to compile alongside.
add_gui_test(shell tabs)
add_gui_test(dialogs connectionsdialog "${GUI_SRC}/dialogs/connectionsdialog.cpp"
             "${GUI_SRC}/app/api.cpp" "${GUI_SRC}/app/icons.cpp"
             "${GUI_SRC}/app/theme.cpp" "${GUI_SRC}/ui/widgets.cpp" "${GUI_QRC}")
add_gui_test(dialogs prefsdialog "${GUI_SRC}/dialogs/prefsdialog.cpp"
             "${GUI_SRC}/app/api.cpp" "${GUI_SRC}/app/icons.cpp"
             "${GUI_SRC}/app/theme.cpp" "${GUI_SRC}/ui/fmt.cpp"
             "${GUI_SRC}/ui/switchbox.cpp" "${GUI_SRC}/ui/widgets.cpp" "${GUI_QRC}")
# shortcutsdialog calls quickConnectShortcut(), so one function pulls the
# whole connections dialog and its closure into this target.
add_gui_test(dialogs smalldialogs "${GUI_SRC}/dialogs/querydialog.cpp"
             "${GUI_SRC}/dialogs/aboutdialog.cpp" "${GUI_SRC}/dialogs/shortcutsdialog.cpp"
             "${GUI_SRC}/dialogs/connectionsdialog.cpp"
             "${GUI_SRC}/editor/sqlhighlighter.cpp"
             "${GUI_SRC}/app/api.cpp" "${GUI_SRC}/app/icons.cpp" "${GUI_SRC}/app/theme.cpp"
             "${GUI_SRC}/ui/widgets.cpp" "${GUI_QRC}")
add_gui_test(dialogs importrowsdialog "${GUI_SRC}/dialogs/importrowsdialog.cpp"
             "${GUI_SRC}/app/api.cpp" "${GUI_SRC}/app/theme.cpp"
             "${GUI_SRC}/ui/widgets.cpp" "${GUI_QRC}")
# Everything under src/ except main.cpp and backend.cpp: MainWindow is the
# orchestrator, so its closure is the application.
add_gui_test(shell mainwindow
    "${GUI_SRC}/app/api.cpp" "${GUI_SRC}/app/icons.cpp" "${GUI_SRC}/app/theme.cpp"
    "${GUI_SRC}/dialogs/aboutdialog.cpp" "${GUI_SRC}/dialogs/connectionsdialog.cpp" "${GUI_SRC}/dialogs/importrowsdialog.cpp"
    "${GUI_SRC}/dialogs/prefsdialog.cpp" "${GUI_SRC}/dialogs/querydialog.cpp" "${GUI_SRC}/dialogs/shortcutsdialog.cpp"
    "${GUI_SRC}/editor/editortab.cpp" "${GUI_SRC}/editor/planview.cpp" "${GUI_SRC}/editor/resultgrid.cpp"
    "${GUI_SRC}/editor/resultmodel.cpp" "${GUI_SRC}/editor/resultpage.cpp" "${GUI_SRC}/editor/sqleditor.cpp"
    "${GUI_SRC}/editor/sqlhighlighter.cpp" "${GUI_SRC}/editor/sqlscan.cpp" "${GUI_SRC}/shell/mainwindow.cpp"
    "${GUI_SRC}/shell/servertabbar.cpp" "${GUI_SRC}/shell/sidebar.cpp" "${GUI_SRC}/shell/statusstrip.cpp"
    "${GUI_SRC}/shell/workspace.cpp" "${GUI_SRC}/ui/fmt.cpp" "${GUI_SRC}/ui/switchbox.cpp"
    "${GUI_SRC}/ui/tableutil.cpp" "${GUI_SRC}/ui/widgets.cpp" "${GUI_SRC}/views/chart.cpp"
    "${GUI_SRC}/views/dashboardview.cpp" "${GUI_SRC}/views/graphcanvas.cpp" "${GUI_SRC}/views/graphview.cpp"
    "${GUI_SRC}/views/historyview.cpp" "${GUI_SRC}/views/innodbview.cpp" "${GUI_SRC}/views/inspectorview.cpp"
    "${GUI_SRC}/views/panelbase.cpp" "${GUI_SRC}/views/processlistview.cpp" "${GUI_SRC}/views/serverinfoview.cpp"
    "${GUI_SRC}/views/statcard.cpp" "${GUI_SRC}/views/usersview.cpp"
    "${GUI_QRC}")
add_gui_test(shell sidebar "${GUI_SRC}/shell/sidebar.cpp" "${GUI_SRC}/app/api.cpp"
             "${GUI_SRC}/app/icons.cpp" "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(shell servertabbar "${GUI_SRC}/shell/servertabbar.cpp"
             "${GUI_SRC}/app/icons.cpp" "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(shell statusstrip "${GUI_SRC}/shell/statusstrip.cpp" "${GUI_SRC}/app/api.cpp"
             "${GUI_SRC}/app/icons.cpp" "${GUI_SRC}/app/theme.cpp" "${GUI_SRC}/ui/fmt.cpp"
             "${GUI_SRC}/ui/widgets.cpp" "${GUI_QRC}")

# workspace.cpp reaches into EditorTab with a qobject_cast to read a live
# editor's unsaved SQL, so testing 69 lines of JSON drags the whole editor
# stack in behind it. Written out rather than hidden in a library target:
# the closure is the point, and it is what a seam here would remove.
add_gui_test(shell workspace
    "${GUI_SRC}/shell/workspace.cpp"
    "${GUI_SRC}/editor/editortab.cpp"
    "${GUI_SRC}/editor/resultpage.cpp"
    "${GUI_SRC}/editor/resultgrid.cpp"
    "${GUI_SRC}/editor/resultmodel.cpp"
    "${GUI_SRC}/editor/sqleditor.cpp"
    "${GUI_SRC}/editor/sqlhighlighter.cpp"
    "${GUI_SRC}/editor/sqlscan.cpp"
    "${GUI_SRC}/editor/planview.cpp"
    "${GUI_SRC}/app/api.cpp"
    "${GUI_SRC}/app/icons.cpp"
    "${GUI_SRC}/app/theme.cpp"
    "${GUI_SRC}/ui/widgets.cpp"
    "${GUI_QRC}")
add_gui_test(app theme "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(app api "${GUI_SRC}/app/api.cpp")
add_gui_test(app backend "${GUI_SRC}/app/backend.cpp" "${GUI_SRC}/app/api.cpp")
add_gui_test(views chart "${GUI_SRC}/views/chart.cpp" "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(views graphcanvas "${GUI_SRC}/views/graphcanvas.cpp"
             "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(views dashboardview
    "${GUI_SRC}/views/dashboardview.cpp" "${GUI_SRC}/views/panelbase.cpp"
    "${GUI_SRC}/views/chart.cpp" "${GUI_SRC}/views/statcard.cpp"
    "${GUI_SRC}/ui/fmt.cpp" "${GUI_SRC}/ui/tableutil.cpp" "${GUI_SRC}/ui/widgets.cpp"
    "${GUI_SRC}/ui/switchbox.cpp" "${GUI_SRC}/app/api.cpp" "${GUI_SRC}/app/icons.cpp"
    "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(views inspectorview "${GUI_SRC}/views/inspectorview.cpp"
             "${GUI_SRC}/ui/tableutil.cpp" "${GUI_SRC}/ui/fmt.cpp"
             "${GUI_SRC}/app/api.cpp" "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(views panelbase "${GUI_SRC}/views/panelbase.cpp" "${GUI_SRC}/ui/widgets.cpp"
             "${GUI_SRC}/app/theme.cpp" "${GUI_SRC}/app/api.cpp" "${GUI_QRC}")
add_gui_test(editor sqlhighlighter "${GUI_SRC}/editor/sqlhighlighter.cpp"
             "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(editor planview "${GUI_SRC}/editor/planview.cpp"
             "${GUI_SRC}/app/theme.cpp" "${GUI_QRC}")
add_gui_test(editor editortab
    "${GUI_SRC}/editor/editortab.cpp" "${GUI_SRC}/editor/resultpage.cpp"
    "${GUI_SRC}/editor/resultgrid.cpp" "${GUI_SRC}/editor/resultmodel.cpp"
    "${GUI_SRC}/editor/sqleditor.cpp" "${GUI_SRC}/editor/sqlhighlighter.cpp"
    "${GUI_SRC}/editor/sqlscan.cpp" "${GUI_SRC}/editor/planview.cpp"
    "${GUI_SRC}/app/api.cpp" "${GUI_SRC}/app/icons.cpp" "${GUI_SRC}/app/theme.cpp"
    "${GUI_SRC}/ui/widgets.cpp" "${GUI_QRC}")
add_gui_test(editor resultpage
    "${GUI_SRC}/editor/resultpage.cpp"
    "${GUI_SRC}/editor/resultgrid.cpp"
    "${GUI_SRC}/editor/resultmodel.cpp"
    "${GUI_SRC}/app/api.cpp"
    "${GUI_SRC}/app/theme.cpp"
    "${GUI_QRC}")

# QTEST_MAIN only expands to the QApplication variant when QT_WIDGETS_LIB is
# defined, and that define rides on the Qt6::Widgets target: without it these
# would quietly build a QCoreApplication main and every widget call would be
# unsupported at runtime.
target_link_libraries(tst_resultmodel PRIVATE Qt6::Network)
target_link_libraries(tst_resultgrid PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_sqleditor PRIVATE Qt6::Widgets Qt6::Svg)
target_link_libraries(tst_sidebar PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_mainwindow PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_api PRIVATE Qt6::Network Qt6::Widgets)
target_link_libraries(tst_backend PRIVATE Qt6::Network)
target_link_libraries(tst_tableutil PRIVATE Qt6::Widgets)
target_link_libraries(tst_theme PRIVATE Qt6::Widgets Qt6::Svg)
target_link_libraries(tst_chart PRIVATE Qt6::Widgets Qt6::Svg)
target_link_libraries(tst_workspace PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_statusstrip PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_importrowsdialog PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_connectionsdialog PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_prefsdialog PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_smalldialogs PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_graphcanvas PRIVATE Qt6::Widgets Qt6::Svg)
target_link_libraries(tst_panelbase PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
target_link_libraries(tst_dashboardview PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)
target_link_libraries(tst_widgets PRIVATE Qt6::Widgets Qt6::Svg)
target_link_libraries(tst_statcard PRIVATE Qt6::Widgets Qt6::Svg)
target_link_libraries(tst_inspectorview PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)
target_link_libraries(tst_processlistview PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)
target_link_libraries(tst_graphview PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)
target_link_libraries(tst_simplepanels PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)
target_link_libraries(tst_servertabbar PRIVATE Qt6::Widgets Qt6::Svg)
target_link_libraries(tst_sqlhighlighter PRIVATE Qt6::Widgets Qt6::Svg)
target_link_libraries(tst_planview PRIVATE Qt6::Widgets Qt6::Svg)
target_link_libraries(tst_resultpage PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)
target_link_libraries(tst_editortab PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)

# "Build the tests and nothing else": gui:test runs this rather than the
# default target, so the suite does not drag the whole application through a
# second build tree to assert on two parsers.
add_custom_target(gui_tests DEPENDS ${GUI_TEST_TARGETS})
