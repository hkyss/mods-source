set(version "1.2.2")

add_commonlibsse_plugin(hkyssNolvusGamepadControls
	AUTHOR hkyss
	VERSION ${version}
	SOURCES main.cpp
)
target_precompile_headers(hkyssNolvusGamepadControls PRIVATE "${PROJECT_SOURCE_DIR}/pch.h")
