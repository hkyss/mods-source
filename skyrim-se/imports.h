#pragma once

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <string_view>

inline void** FindImport(HMODULE a_module, const char* a_library, std::string_view a_function)
{
	const auto base = reinterpret_cast<std::uint8_t*>(a_module);
	const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!imports.VirtualAddress) {
		return nullptr;
	}
	for (auto library = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); library->Name; ++library) {
		if (_stricmp(reinterpret_cast<const char*>(base + library->Name), a_library) != 0) {
			continue;
		}
		auto names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + (library->OriginalFirstThunk ? library->OriginalFirstThunk : library->FirstThunk));
		auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + library->FirstThunk);
		for (; names->u1.AddressOfData; ++names, ++slots) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
				continue;
			}
			const auto byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
			if (a_function == reinterpret_cast<const char*>(byName->Name)) {
				return reinterpret_cast<void**>(&slots->u1.Function);
			}
		}
	}
	return nullptr;
}

template <class F>
bool RedirectImport(HMODULE a_module, const char* a_function, F a_replacement, F& a_original)
{
	void** slot = FindImport(a_module, "kernel32.dll", a_function);
	if (!slot) {
		return false;
	}
	DWORD protection = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) {
		return false;
	}
	a_original = reinterpret_cast<F>(*slot);
	*slot = reinterpret_cast<void*>(a_replacement);
	VirtualProtect(slot, sizeof(void*), protection, &protection);
	return true;
}
