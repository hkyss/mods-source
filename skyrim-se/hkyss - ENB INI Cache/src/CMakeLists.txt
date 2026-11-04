set(version "1.0.1")

add_commonlibsse_plugin(hkyssEnbCache
	AUTHOR hkyss
	VERSION ${version}
	SOURCES main.cpp
)
target_precompile_headers(hkyssEnbCache PRIVATE "${PROJECT_SOURCE_DIR}/pch.h")
