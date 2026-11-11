set(version "1.0.5")

add_commonlibsse_plugin(hkyssOarCache
	AUTHOR hkyss
	VERSION ${version}
	SOURCES main.cpp
)
target_precompile_headers(hkyssOarCache PRIVATE "${PROJECT_SOURCE_DIR}/pch.h")
