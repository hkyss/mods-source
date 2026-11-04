set(version "1.0.2")

add_commonlibsse_plugin(hkyssHeadSway
	AUTHOR hkyss
	VERSION ${version}
	SOURCES main.cpp
)
target_precompile_headers(hkyssHeadSway PRIVATE "${PROJECT_SOURCE_DIR}/pch.h")
