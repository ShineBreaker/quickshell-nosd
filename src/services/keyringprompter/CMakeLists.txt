find_package(PkgConfig REQUIRED)
pkg_check_modules(glib REQUIRED IMPORTED_TARGET glib-2.0>=2.36)
pkg_check_modules(gobject REQUIRED IMPORTED_TARGET gobject-2.0)
pkg_check_modules(gio REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(gcr REQUIRED IMPORTED_TARGET gcr-4)

qt_add_library(quickshell-service-keyringprompter STATIC
	gcrprompt.cpp
	keyringprompter.cpp
)

qt_add_qml_module(quickshell-service-keyringprompter
	URI Quickshell.Services.Keyring
	VERSION 0.1
	DEPENDENCIES QtQml
)

install_qml_module(quickshell-service-keyringprompter)

# libgcr hides the whole GcrPrompt / GcrSystemPrompter API behind this switch.
target_compile_definitions(quickshell-service-keyringprompter PRIVATE GCR_API_SUBJECT_TO_CHANGE)

target_link_libraries(quickshell-service-keyringprompter PRIVATE
	Qt::Qml
	Qt::Quick
	PkgConfig::glib
	PkgConfig::gobject
	PkgConfig::gio
	PkgConfig::gcr
)

qs_module_pch(quickshell-service-keyringprompter)

target_link_libraries(quickshell PRIVATE quickshell-service-keyringprompterplugin)
