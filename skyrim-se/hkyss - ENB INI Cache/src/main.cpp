#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../imports.h"

using namespace std::literals;

namespace
{
	constexpr auto kModeFile = "Data/SKSE/Plugins/hkyssEnbCache.txt";
	constexpr int  kShownDifferences = 100;
	constexpr int  kIntBuffer = 20;

	std::ofstream logFile;
	std::mutex    logLock;
	const auto    started = std::chrono::steady_clock::now();

	template <class... Args>
	void Log(std::format_string<Args...> a_format, Args&&... a_args)
	{
		std::scoped_lock lock(logLock);
		if (logFile.is_open()) {
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
			logFile << std::format("{:8} ", elapsed.count()) << std::format(a_format, std::forward<Args>(a_args)...) << std::endl;
		}
	}

	bool             use = false;
	std::atomic_bool serving{ true };
	std::string      gameFolder;
	std::string      dataFolder;

	enum Function : std::size_t
	{
		kString,
		kInt,
		kSection,
		kSectionNames,
		kWrite,
		kCreate,
		kFunctions
	};

	constexpr const char* kFunctionNames[kFunctions]{
		"GetPrivateProfileStringA",
		"GetPrivateProfileIntA",
		"GetPrivateProfileSectionA",
		"GetPrivateProfileSectionNamesA",
		"WritePrivateProfileStringA",
		"CreateFileA",
	};

	struct Tally
	{
		std::atomic_uint64_t calls{ 0 };
		std::atomic_uint64_t served{ 0 };
		std::atomic_uint64_t compared{ 0 };
		std::atomic_uint64_t passed{ 0 };
		std::atomic_uint64_t ticks{ 0 };
	};

	Tally            tallies[kFunctions];
	std::atomic_int  differences{ 0 };
	std::atomic_int  errorDifferences{ 0 };
	std::atomic_uint64_t overlaps{ 0 };

	decltype(&::GetPrivateProfileStringA)        originalString = nullptr;
	decltype(&::GetPrivateProfileIntA)           originalInt = nullptr;
	decltype(&::GetPrivateProfileSectionA)       originalSection = nullptr;
	decltype(&::GetPrivateProfileSectionNamesA)  originalSectionNames = nullptr;
	decltype(&::WritePrivateProfileStringA)      originalWrite = nullptr;
	decltype(&::CreateFileA)                     originalCreate = nullptr;
	decltype(&::CloseHandle)                     originalClose = nullptr;

	std::uint64_t Ticks()
	{
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		return static_cast<std::uint64_t>(now.QuadPart);
	}

	class Timed
	{
	public:
		explicit Timed(Function a_function) :
			_function(a_function)
		{
			++tallies[a_function].calls;
		}

		~Timed()
		{
			tallies[_function].ticks += Ticks() - _start;
		}

	private:
		Function      _function;
		std::uint64_t _start = Ticks();
	};

	std::mutex                                                fileLock;
	std::map<std::string, std::uint64_t, std::less<>>         files[kFunctions];

	void NoteFile(Function a_function, LPCSTR a_file)
	{
		std::scoped_lock lock(fileLock);
		++files[a_function][a_file ? a_file : "(none)"];
	}

	void Difference(std::string a_what)
	{
		if (++differences <= kShownDifferences) {
			Log("difference: {}", a_what);
		}
	}

	bool Space(char a_c)
	{
		return a_c == ' ' || a_c == '\t' || a_c == '\r' || a_c == '\v' || a_c == '\f';
	}

	std::string_view Trim(std::string_view a_text)
	{
		while (!a_text.empty() && Space(a_text.front())) {
			a_text.remove_prefix(1);
		}
		while (!a_text.empty() && Space(a_text.back())) {
			a_text.remove_suffix(1);
		}
		return a_text;
	}

	std::string_view TrimSpaces(std::string_view a_text)
	{
		while (!a_text.empty() && a_text.front() == ' ') {
			a_text.remove_prefix(1);
		}
		while (!a_text.empty() && a_text.back() == ' ') {
			a_text.remove_suffix(1);
		}
		return a_text;
	}

	std::string Lower(std::string_view a_text)
	{
		std::string text(a_text);
		if (!text.empty()) {
			CharLowerBuffA(text.data(), static_cast<DWORD>(text.size()));
		}
		return text;
	}

	std::string Key(std::string_view a_path)
	{
		std::string path(a_path);
		for (auto& c : path) {
			if (c == '/') {
				c = '\\';
			}
		}
		if (path.starts_with("\\\\?\\")) {
			path.erase(0, 4);
		}
		const bool absolute = (path.size() >= 2 && path[1] == ':') || path.starts_with("\\\\");
		if (!absolute) {
			path = gameFolder + "\\" + path;
		}
		std::vector<std::string_view> parts;
		std::string_view              rest = path;
		while (!rest.empty()) {
			const auto             slash = rest.find('\\');
			const std::string_view part = rest.substr(0, slash);
			rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
			if (part.empty() || part == ".") {
				continue;
			}
			if (part == "..") {
				if (parts.size() > 1) {
					parts.pop_back();
				}
				continue;
			}
			parts.push_back(part);
		}
		std::string key;
		for (const auto& part : parts) {
			if (!key.empty()) {
				key += '\\';
			}
			key += part;
		}
		return Lower(key);
	}

	bool Under(const std::string& a_key, const std::string& a_folder)
	{
		return a_key.size() > a_folder.size() && a_key.compare(0, a_folder.size(), a_folder) == 0 && a_key[a_folder.size()] == '\\';
	}

	bool Cacheable(const std::string& a_key)
	{
		return Under(a_key, gameFolder) && !Under(a_key, dataFolder) && a_key != dataFolder;
	}

	struct Pair
	{
		std::string name;
		std::string lower;
		std::string value;
		bool        equals;
		bool        comment;
	};

	struct Section
	{
		std::string       lower;
		std::vector<Pair> pairs;
	};

	struct Ini
	{
		bool                 missing = false;
		bool                 simple = true;
		std::string          why;
		std::vector<Section> sections;
	};

	void Parse(std::string_view a_text, Ini& a_ini)
	{
		if (a_text.starts_with("\xEF\xBB\xBF") || a_text.starts_with("\xFF\xFE") || a_text.starts_with("\xFE\xFF")) {
			a_ini.simple = false;
			a_ini.why = "a byte order mark";
			return;
		}
		if (a_text.find('\0') != std::string_view::npos || a_text.find('\x1A') != std::string_view::npos) {
			a_ini.simple = false;
			a_ini.why = "a zero or end of file byte";
			return;
		}
		Section* current = nullptr;
		while (!a_text.empty()) {
			const auto       end = a_text.find('\n');
			std::string_view line = Trim(a_text.substr(0, end));
			a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
			if (line.empty()) {
				continue;
			}
			if (line.front() == '[') {
				const auto close = line.find(']');
				if (close != std::string_view::npos && line.find(']', close + 1) != std::string_view::npos) {
					a_ini.simple = false;
					a_ini.why = std::format("the section line {}", line);
					return;
				}
				const auto name = close == std::string_view::npos ? line.substr(1) : line.substr(1, close - 1);
				current = &a_ini.sections.emplace_back(Section{ Lower(Trim(name)), {} });
				continue;
			}
			if (!current) {
				continue;
			}
			const auto equals = line.find('=');
			const auto name = Trim(line.substr(0, equals));
			const auto value = equals == std::string_view::npos ? std::string_view{} : Trim(line.substr(equals + 1));
			current->pairs.push_back({ std::string(name), Lower(name), std::string(value), equals != std::string_view::npos, line.front() == ';' });
		}
	}

	std::shared_ptr<Ini> Load(const std::string& a_key)
	{
		auto         ini = std::make_shared<Ini>();
		const HANDLE file = CreateFileA(a_key.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			ini->missing = true;
			return ini;
		}
		std::string   text;
		LARGE_INTEGER size{};
		if (GetFileSizeEx(file, &size) && size.QuadPart < 64 * 1024 * 1024) {
			text.resize(static_cast<std::size_t>(size.QuadPart));
			DWORD read = 0;
			if (!text.empty() && (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size())) {
				ini->simple = false;
				ini->why = "a short read";
			}
		} else {
			ini->simple = false;
			ini->why = "its size";
		}
		CloseHandle(file);
		if (ini->simple) {
			Parse(text, *ini);
		}
		return ini;
	}

	std::mutex                                            cacheLock;
	std::unordered_map<std::string, std::shared_ptr<Ini>> inis;
	std::atomic_uint64_t                                  loaded{ 0 };

	std::shared_ptr<const Ini> CachedIni(LPCSTR a_file)
	{
		if (!a_file || !serving) {
			return nullptr;
		}
		const std::string_view file = a_file;
		if (file.find_first_of("\\/") == std::string_view::npos) {
			return nullptr;
		}
		const auto key = Key(file);
		if (!Cacheable(key)) {
			return nullptr;
		}
		std::scoped_lock lock(cacheLock);
		auto&            ini = inis[key];
		if (!ini) {
			ini = Load(key);
			++loaded;
			if (!ini->simple) {
				Log("{} is left to Windows because of {}", key, ini->why);
			}
		}
		return ini->simple ? ini : nullptr;
	}

	void Forget(LPCSTR a_file);

	const Section* FindSection(const Ini& a_ini, LPCSTR a_section)
	{
		if (a_ini.missing) {
			return nullptr;
		}
		const auto section = Lower(TrimSpaces(a_section));
		for (const auto& candidate : a_ini.sections) {
			if (candidate.lower == section) {
				return &candidate;
			}
		}
		return nullptr;
	}

	const Pair* Find(const Ini& a_ini, LPCSTR a_section, LPCSTR a_key)
	{
		const auto section = FindSection(a_ini, a_section);
		if (!section) {
			return nullptr;
		}
		const auto key = Lower(TrimSpaces(a_key));
		for (const auto& pair : section->pairs) {
			if (pair.equals && pair.lower == key) {
				return &pair;
			}
		}
		return nullptr;
	}

	std::string Fallback(LPCSTR a_default)
	{
		std::string_view fallback = a_default ? a_default : "";
		while (!fallback.empty() && fallback.back() == ' ') {
			fallback.remove_suffix(1);
		}
		return std::string(fallback);
	}

	std::string Answer(const Ini& a_ini, LPCSTR a_section, LPCSTR a_key, LPCSTR a_default, bool& a_found)
	{
		a_found = false;
		if (const auto pair = Find(a_ini, a_section, a_key)) {
			a_found = true;
			std::string_view text = pair->value;
			if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) {
				text = text.substr(1, text.size() - 2);
			}
			return std::string(text);
		}
		return Fallback(a_default);
	}

	DWORD Copy(const std::string& a_text, LPSTR a_buffer, DWORD a_size)
	{
		const DWORD length = static_cast<DWORD>(std::min<std::size_t>(a_text.size(), a_size - 1));
		std::memcpy(a_buffer, a_text.data(), length);
		a_buffer[length] = '\0';
		return length;
	}

	struct Listed
	{
		DWORD length;
		bool  truncated;
		DWORD written;
	};

	Listed CopyList(const std::vector<std::string>& a_entries, LPSTR a_buffer, DWORD a_size)
	{
		DWORD position = 0;
		for (const auto& entry : a_entries) {
			const DWORD need = static_cast<DWORD>(entry.size()) + 1;
			if (position + need + 2 > a_size) {
				const DWORD room = a_size - position;
				if (room >= 2) {
					std::memcpy(a_buffer + position, entry.data(), room - 2);
					a_buffer[position + room - 2] = '\0';
					a_buffer[position + room - 1] = '\0';
				} else if (room == 1) {
					a_buffer[position] = '\0';
				}
				return { a_size >= 2 ? a_size - 2 : 0, true, a_size };
			}
			std::memcpy(a_buffer + position, entry.data(), entry.size());
			a_buffer[position + entry.size()] = '\0';
			position += need;
		}
		a_buffer[position] = '\0';
		return { position, false, position + 1 };
	}

	std::vector<std::string> KeyNames(const Section& a_section)
	{
		std::vector<std::string> names;
		for (const auto& pair : a_section.pairs) {
			if (pair.equals && !pair.comment) {
				names.push_back(pair.name);
			}
		}
		return names;
	}

	std::vector<std::string> SectionLines(const Section& a_section)
	{
		std::vector<std::string> lines;
		for (const auto& pair : a_section.pairs) {
			if (!pair.comment) {
				lines.push_back(pair.equals ? pair.name + "=" + pair.value : pair.name);
			}
		}
		return lines;
	}

	DWORD ErrorFor(bool a_found, std::size_t a_length = 0, DWORD a_size = 0)
	{
		if (!a_found) {
			return ERROR_FILE_NOT_FOUND;
		}
		return a_size && a_length + 1 >= a_size ? ERROR_MORE_DATA : ERROR_SUCCESS;
	}

	UINT Integer(std::string_view a_text)
	{
		std::size_t i = 0;
		while (i < a_text.size() && static_cast<unsigned char>(a_text[i]) <= ' ') {
			++i;
		}
		bool negative = false;
		if (i < a_text.size() && (a_text[i] == '-' || a_text[i] == '+')) {
			negative = a_text[i] == '-';
			++i;
		}
		unsigned base = 10;
		if (i + 1 < a_text.size() && a_text[i] == '0') {
			const char prefix = a_text[i + 1];
			if (prefix == 'x' || prefix == 'X') {
				base = 16;
				i += 2;
			} else if (prefix == 'o' || prefix == 'O') {
				base = 8;
				i += 2;
			} else if (prefix == 'b' || prefix == 'B') {
				base = 2;
				i += 2;
			}
		}
		std::uint32_t value = 0;
		for (; i < a_text.size(); ++i) {
			const char c = a_text[i];
			unsigned   digit = 0;
			if (c >= '0' && c <= '9') {
				digit = static_cast<unsigned>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				digit = static_cast<unsigned>(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				digit = static_cast<unsigned>(c - 'A' + 10);
			} else {
				break;
			}
			if (digit >= base) {
				break;
			}
			value = value * base + digit;
		}
		return negative ? static_cast<UINT>(0u - value) : static_cast<UINT>(value);
	}

	std::string Show(std::string_view a_text)
	{
		std::string shown(a_text.substr(0, 60));
		for (auto& c : shown) {
			if (c == '\0') {
				c = '|';
			}
		}
		return a_text.size() > 60 ? shown + "..." : shown;
	}

	struct Emulated
	{
		std::string bytes;
		DWORD       length = 0;
		DWORD       written = 0;
		bool        setsError = true;
		DWORD       error = ERROR_SUCCESS;
	};

	Emulated EmulateString(const Ini& a_ini, LPCSTR a_section, LPCSTR a_key, LPCSTR a_default, DWORD a_size)
	{
		Emulated result;
		result.bytes.assign(a_size, '\0');
		if (a_key) {
			bool       found = false;
			const auto text = Answer(a_ini, a_section, a_key, a_default, found);
			result.length = Copy(text, result.bytes.data(), a_size);
			result.written = result.length + 1;
			result.error = ErrorFor(found, text.size(), a_size);
			return result;
		}
		const auto section = FindSection(a_ini, a_section);
		if (!section) {
			result.length = Copy(Fallback(a_default), result.bytes.data(), a_size);
			result.written = result.length + 1;
			result.error = ERROR_FILE_NOT_FOUND;
			return result;
		}
		const auto listed = CopyList(KeyNames(*section), result.bytes.data(), a_size);
		result.length = listed.length;
		result.written = listed.written;
		result.error = listed.truncated ? ERROR_MORE_DATA : ERROR_SUCCESS;
		return result;
	}

	Emulated EmulateSection(const Ini& a_ini, LPCSTR a_section, DWORD a_size)
	{
		Emulated result;
		result.bytes.assign(a_size, '\0');
		const auto section = FindSection(a_ini, a_section);
		if (!section) {
			result.length = 0;
			result.written = 1;
			result.error = ERROR_FILE_NOT_FOUND;
			return result;
		}
		const auto listed = CopyList(SectionLines(*section), result.bytes.data(), a_size);
		result.length = listed.length;
		result.written = listed.written;
		result.setsError = !listed.truncated;
		result.error = ERROR_SUCCESS;
		return result;
	}

	std::size_t Compared(const Emulated& a_emulated, DWORD a_size)
	{
		return std::min<std::size_t>(a_size, a_emulated.written);
	}

	DWORD WINAPI CachedString(LPCSTR a_section, LPCSTR a_key, LPCSTR a_default, LPSTR a_buffer, DWORD a_size, LPCSTR a_file)
	{
		Timed timed(kString);
		NoteFile(kString, a_file);
		const auto ini = a_section && a_buffer && a_size > 0 ? CachedIni(a_file) : nullptr;
		if (!ini) {
			++tallies[kString].passed;
			return originalString(a_section, a_key, a_default, a_buffer, a_size, a_file);
		}
		const auto emulated = EmulateString(*ini, a_section, a_key, a_default, a_size);
		const auto compared = Compared(emulated, a_size);
		if (use) {
			++tallies[kString].served;
			std::memcpy(a_buffer, emulated.bytes.data(), compared);
			SetLastError(emulated.error);
			return emulated.length;
		}
		++tallies[kString].compared;
		const DWORD result = originalString(a_section, a_key, a_default, a_buffer, a_size, a_file);
		const DWORD error = GetLastError();
		if (result != emulated.length || std::memcmp(a_buffer, emulated.bytes.data(), compared) != 0) {
			Difference(std::format("[{}] {} in {}: Windows gave {} \"{}\", the cache {} \"{}\"", a_section, a_key ? a_key : "(every key)", a_file, result, Show(std::string_view(a_buffer, compared)), emulated.length, Show(std::string_view(emulated.bytes.data(), compared))));
		} else if (error != emulated.error && ++errorDifferences <= kShownDifferences) {
			Log("error difference: [{}] {} in {}: Windows left {}, the cache would leave {}", a_section, a_key ? a_key : "(every key)", a_file, error, emulated.error);
		}
		SetLastError(error);
		return result;
	}

	UINT WINAPI CachedInt(LPCSTR a_section, LPCSTR a_key, INT a_default, LPCSTR a_file)
	{
		Timed timed(kInt);
		NoteFile(kInt, a_file);
		const auto ini = a_section && a_key ? CachedIni(a_file) : nullptr;
		if (!ini) {
			++tallies[kInt].passed;
			return originalInt(a_section, a_key, a_default, a_file);
		}
		bool       found = false;
		auto       text = Answer(*ini, a_section, a_key, "", found);
		text.resize(std::min<std::size_t>(text.size(), kIntBuffer - 1));
		const UINT value = text.empty() ? static_cast<UINT>(a_default) : Integer(text);
		if (use) {
			++tallies[kInt].served;
			SetLastError(ErrorFor(found));
			return value;
		}
		++tallies[kInt].compared;
		const UINT  result = originalInt(a_section, a_key, a_default, a_file);
		const DWORD error = GetLastError();
		if (result != value) {
			Difference(std::format("[{}] {} in {} as a number: Windows gave {}, the cache {} from \"{}\"", a_section, a_key, a_file, result, value, Show(text)));
		}
		SetLastError(error);
		return result;
	}

	DWORD WINAPI CachedSection(LPCSTR a_section, LPSTR a_buffer, DWORD a_size, LPCSTR a_file)
	{
		Timed timed(kSection);
		NoteFile(kSection, a_file);
		const auto ini = a_section && a_buffer && a_size > 0 ? CachedIni(a_file) : nullptr;
		if (!ini) {
			++tallies[kSection].passed;
			return originalSection(a_section, a_buffer, a_size, a_file);
		}
		const auto emulated = EmulateSection(*ini, a_section, a_size);
		const auto compared = Compared(emulated, a_size);
		if (use) {
			++tallies[kSection].served;
			std::memcpy(a_buffer, emulated.bytes.data(), compared);
			if (emulated.setsError) {
				SetLastError(emulated.error);
			}
			return emulated.length;
		}
		++tallies[kSection].compared;
		const DWORD before = GetLastError();
		const DWORD result = originalSection(a_section, a_buffer, a_size, a_file);
		const DWORD error = GetLastError();
		if (result != emulated.length || std::memcmp(a_buffer, emulated.bytes.data(), compared) != 0) {
			Difference(std::format("section [{}] in {}: Windows gave {} \"{}\", the cache {} \"{}\"", a_section, a_file, result, Show(std::string_view(a_buffer, compared)), emulated.length, Show(std::string_view(emulated.bytes.data(), compared))));
		} else if (error != (emulated.setsError ? emulated.error : before) && ++errorDifferences <= kShownDifferences) {
			Log("error difference: section [{}] in {}: Windows left {}, the cache would leave {}", a_section, a_file, error, emulated.setsError ? emulated.error : before);
		}
		SetLastError(error);
		return result;
	}

	DWORD WINAPI CachedSectionNames(LPSTR a_buffer, DWORD a_size, LPCSTR a_file)
	{
		Timed timed(kSectionNames);
		NoteFile(kSectionNames, a_file);
		++tallies[kSectionNames].passed;
		return originalSectionNames(a_buffer, a_size, a_file);
	}

	struct Opened
	{
		HANDLE      master = nullptr;
		DWORD       error = ERROR_SUCCESS;
		DWORD       access = 0;
		DWORD       share = 0;
		DWORD       flags = 0;
		bool        failed = false;
		bool        worked = false;
		std::size_t alive = 0;
	};

	std::unordered_map<std::string, Opened> opened;
	std::unordered_map<HANDLE, std::string> handles;
	std::map<std::string, std::uint64_t>    shapes;

	void ForgetKey(const std::string& a_key)
	{
		inis.erase(a_key);
		const auto prefix = a_key + "|";
		for (auto& [slot, entry] : opened) {
			if (!slot.starts_with(prefix)) {
				continue;
			}
			if (entry.master) {
				originalClose(entry.master);
				entry.master = nullptr;
			}
			entry.failed = false;
			entry.worked = false;
		}
	}

	void Forget(LPCSTR a_file)
	{
		if (!a_file) {
			return;
		}
		std::scoped_lock lock(cacheLock);
		ForgetKey(Key(a_file));
	}

	BOOL WINAPI CachedWrite(LPCSTR a_section, LPCSTR a_key, LPCSTR a_value, LPCSTR a_file)
	{
		Timed timed(kWrite);
		NoteFile(kWrite, a_file);
		++tallies[kWrite].passed;
		Forget(a_file);
		return originalWrite(a_section, a_key, a_value, a_file);
	}

	bool ReadOnly(DWORD a_access, DWORD a_disposition, DWORD a_flags, HANDLE a_template)
	{
		constexpr DWORD readRights = GENERIC_READ | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA | READ_CONTROL | SYNCHRONIZE;
		constexpr DWORD unsafeFlags = FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OVERLAPPED | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_POSIX_SEMANTICS;
		return a_disposition == OPEN_EXISTING && !(a_access & ~readRights) && !(a_flags & unsafeFlags) && !a_template;
	}

	void Track(HANDLE a_handle, const std::string& a_key, Opened& a_entry)
	{
		handles[a_handle] = a_key;
		++a_entry.alive;
	}

	HANDLE WINAPI CachedCreate(LPCSTR a_path, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_security, DWORD a_disposition, DWORD a_flags, HANDLE a_template)
	{
		Timed timed(kCreate);
		NoteFile(kCreate, a_path);
		if (!a_path || !serving) {
			++tallies[kCreate].passed;
			return originalCreate(a_path, a_access, a_share, a_security, a_disposition, a_flags, a_template);
		}
		const auto key = Key(a_path);
		if (!Cacheable(key)) {
			++tallies[kCreate].passed;
			return originalCreate(a_path, a_access, a_share, a_security, a_disposition, a_flags, a_template);
		}
		std::unique_lock lock(cacheLock);
		++shapes[std::format("access {:X}, share {:X}, disposition {}, flags {:X}{}", a_access, a_share, a_disposition, a_flags, a_security ? ", with security" : "")];
		const bool plainSecurity = !a_security || (!a_security->lpSecurityDescriptor && !a_security->bInheritHandle);
		if (!ReadOnly(a_access, a_disposition, a_flags, a_template) || !plainSecurity) {
			ForgetKey(key);
			lock.unlock();
			++tallies[kCreate].passed;
			return originalCreate(a_path, a_access, a_share, a_security, a_disposition, a_flags, a_template);
		}
		const auto slot = std::format("{}|{:X}|{:X}|{:X}", key, a_access, a_share, a_flags);
		auto&      entry = opened[slot];
		const bool known = entry.failed || entry.worked;
		const bool same = known;
		if (use && same && entry.failed) {
			++tallies[kCreate].served;
			SetLastError(entry.error);
			return INVALID_HANDLE_VALUE;
		}
		if (use && same && entry.master && entry.alive == 0) {
			HANDLE copy = nullptr;
			if (DuplicateHandle(GetCurrentProcess(), entry.master, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
				SetFilePointer(copy, 0, nullptr, FILE_BEGIN);
				Track(copy, slot, entry);
				++tallies[kCreate].served;
				SetLastError(ERROR_SUCCESS);
				return copy;
			}
		}
		if (same && entry.alive > 0) {
			++overlaps;
		}
		const HANDLE real = originalCreate(a_path, a_access, a_share, a_security, a_disposition, a_flags, a_template);
		const DWORD  error = GetLastError();
		if (known && same) {
			++tallies[kCreate].compared;
			if (entry.failed != (real == INVALID_HANDLE_VALUE)) {
				Difference(std::format("opening {}: Windows {}, the cache remembers {}", a_path, real == INVALID_HANDLE_VALUE ? std::format("failed with {}", error) : "opened it"s, entry.failed ? std::format("a failure with {}", entry.error) : "an open file"s));
			}
		} else {
			++tallies[kCreate].passed;
		}
		if (!known) {
			entry.access = a_access;
			entry.share = a_share;
			entry.flags = a_flags;
			if (real == INVALID_HANDLE_VALUE) {
				if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
					entry.failed = true;
					entry.error = error;
				}
			} else {
				entry.worked = true;
				if (use) {
					DuplicateHandle(GetCurrentProcess(), real, GetCurrentProcess(), &entry.master, 0, FALSE, DUPLICATE_SAME_ACCESS);
				}
			}
		}
		if (real != INVALID_HANDLE_VALUE) {
			Track(real, slot, entry);
		}
		SetLastError(error);
		return real;
	}

	BOOL WINAPI CachedClose(HANDLE a_handle)
	{
		{
			std::scoped_lock lock(cacheLock);
			if (const auto it = handles.find(a_handle); it != handles.end()) {
				if (const auto entry = opened.find(it->second); entry != opened.end() && entry->second.alive > 0) {
					--entry->second.alive;
				}
				handles.erase(it);
			}
		}
		return originalClose(a_handle);
	}

	void Report(std::string_view a_when)
	{
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		const double perMs = static_cast<double>(frequency.QuadPart) / 1000.0;
		Log("{}: {} INI files read, {} differences, {} error code differences, {} opens of a file already open", a_when, loaded.load(), differences.load(), errorDifferences.load(), overlaps.load());
		for (std::size_t i = 0; i < kFunctions; ++i) {
			const auto& tally = tallies[i];
			if (tally.calls == 0) {
				continue;
			}
			Log("  {}: {} calls, {} answered from memory, {} checked against Windows, {} passed on, {:.0f} ms", kFunctionNames[i], tally.calls.load(), tally.served.load(), tally.compared.load(), tally.passed.load(), tally.ticks / perMs);
		}
		std::scoped_lock lock(cacheLock);
		{
			std::scoped_lock fileGuard(fileLock);
			for (std::size_t i = 0; i < kFunctions; ++i) {
				if (files[i].empty()) {
					continue;
				}
				std::vector<std::pair<std::string, std::uint64_t>> sorted(files[i].begin(), files[i].end());
				std::ranges::sort(sorted, std::greater{}, &std::pair<std::string, std::uint64_t>::second);
				Log("  {} asked about {} files", kFunctionNames[i], sorted.size());
				for (std::size_t j = 0; j < sorted.size() && j < 8; ++j) {
					Log("    {} x {}", sorted[j].second, sorted[j].first);
				}
			}
		}
		for (const auto& [shape, count] : shapes) {
			Log("  CreateFileA {}: {} calls", shape, count);
		}
	}

	void StopServing()
	{
		if (!serving.exchange(false)) {
			return;
		}
		Report("main menu, every call goes to Windows from now on");
		std::scoped_lock lock(cacheLock);
		for (auto& [key, entry] : opened) {
			if (entry.master) {
				originalClose(entry.master);
				entry.master = nullptr;
			}
		}
		inis.clear();
	}

	void Install()
	{
		std::wstring folder(MAX_PATH * 4, L'\0');
		folder.resize(GetCurrentDirectoryW(static_cast<DWORD>(folder.size()), folder.data()));
		const HMODULE enb = GetModuleHandleW((folder + L"\\d3d11.dll").c_str());
		if (!enb) {
			Log("ENB's d3d11.dll is not loaded from the game folder, nothing to do");
			serving = false;
			return;
		}
		std::string missing;
		const auto  redirect = [&](const char* a_name, auto a_replacement, auto& a_original) {
			if (!RedirectImport(enb, a_name, a_replacement, a_original)) {
				missing += std::format(" {}", a_name);
			}
		};
		redirect("CloseHandle", &CachedClose, originalClose);
		redirect("GetPrivateProfileStringA", &CachedString, originalString);
		redirect("GetPrivateProfileIntA", &CachedInt, originalInt);
		redirect("GetPrivateProfileSectionA", &CachedSection, originalSection);
		redirect("GetPrivateProfileSectionNamesA", &CachedSectionNames, originalSectionNames);
		redirect("WritePrivateProfileStringA", &CachedWrite, originalWrite);
		if (originalClose) {
			redirect("CreateFileA", &CachedCreate, originalCreate);
		}
		if (!missing.empty()) {
			Log("d3d11.dll does not import{}", missing);
		}
		Log("{} ENB's INI reads and opens under {}", use ? "answering" : "checking", gameFolder);
	}

	class MenuWatch : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME) {
				StopServing();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	MenuWatch menuWatch;

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			Install();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			if (const auto ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuWatch);
			}
			break;
		default:
			break;
		}
	}

	void SetFolders()
	{
		std::string current(MAX_PATH * 4, '\0');
		GetCurrentDirectoryA(static_cast<DWORD>(current.size()), current.data());
		current.resize(std::strlen(current.c_str()));
		gameFolder = Lower(current);
		while (!gameFolder.empty() && gameFolder.back() == '\\') {
			gameFolder.pop_back();
		}
		dataFolder = gameFolder + "\\data";
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, false);
	if (const auto directory = SKSE::log::log_directory()) {
		logFile.open(*directory / "hkyssEnbCache.log", std::ios::trunc);
	}
	Log("hkyss ENB cache on Skyrim {}", a_skse->RuntimeVersion().string());
	std::string   word;
	std::ifstream file(kModeFile);
	file >> word;
	use = word != "check";
	SetFolders();
	Log("mode {}, from {}", use ? "use" : "check", word.empty() ? "the default"s : kModeFile);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
