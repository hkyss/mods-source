#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../imports.h"

using namespace std::literals;

namespace
{
	constexpr auto           kModeFile = "Data/SKSE/Plugins/hkyssOarCache.txt";
	constexpr std::uintptr_t kOurSearch = std::uintptr_t{ 1 } << 62;
	constexpr int            kShownDifferences = 100;

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

	std::string Narrow(std::wstring_view a_text)
	{
		const int   size = WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
		std::string text(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), text.data(), size, nullptr, nullptr);
		return text;
	}

	struct Entry
	{
		DWORD        attributes;
		FILETIME     created;
		FILETIME     accessed;
		FILETIME     written;
		DWORD        sizeHigh;
		DWORD        sizeLow;
		DWORD        reparseTag;
		std::wstring name;
	};

	using Listing = std::vector<Entry>;

	struct Answer
	{
		BOOL                      found;
		DWORD                     error;
		WIN32_FILE_ATTRIBUTE_DATA data;
	};

	struct Search
	{
		const Listing* listing;
		std::size_t    next;
	};

	std::wstring                                                workingDirectory;
	std::wstring                                                root;
	std::unordered_map<std::wstring, Listing>                   listings;
	std::unordered_map<std::wstring, Answer>                    answers;
	std::atomic_bool                                            ready{ false };
	std::atomic_bool                                            serving{ true };
	bool                                                        use = false;

	std::atomic_uint64_t served{ 0 };
	std::atomic_uint64_t compared{ 0 };
	std::atomic_uint64_t passed{ 0 };
	std::atomic_uint64_t early{ 0 };
	std::atomic_int      differences{ 0 };

	decltype(&::FindFirstFileExW)     originalFindFirst = nullptr;
	decltype(&::FindNextFileW)        originalFindNext = nullptr;
	decltype(&::FindClose)            originalFindClose = nullptr;
	decltype(&::GetFileAttributesExW) originalAttributesW = nullptr;
	decltype(&::GetFileAttributesExA) originalAttributesA = nullptr;

	std::mutex                         checkLock;
	std::unordered_map<HANDLE, Search> checked;

	std::wstring Lower(std::wstring a_text)
	{
		CharLowerBuffW(a_text.data(), static_cast<DWORD>(a_text.size()));
		return a_text;
	}

	std::wstring Key(std::wstring_view a_path)
	{
		std::wstring path(a_path);
		for (auto& c : path) {
			if (c == L'/') {
				c = L'\\';
			}
		}
		if (path.starts_with(L"\\\\?\\")) {
			path.erase(0, 4);
		}
		const bool absolute = (path.size() >= 2 && path[1] == L':') || path.starts_with(L"\\\\");
		if (!absolute) {
			path = workingDirectory + L"\\" + path;
		}
		std::vector<std::wstring_view> parts;
		std::wstring_view              rest = path;
		while (!rest.empty()) {
			const auto              slash = rest.find(L'\\');
			const std::wstring_view part = rest.substr(0, slash);
			rest = slash == std::wstring_view::npos ? std::wstring_view{} : rest.substr(slash + 1);
			if (part.empty() || part == L".") {
				continue;
			}
			if (part == L"..") {
				if (parts.size() > 1) {
					parts.pop_back();
				}
				continue;
			}
			parts.push_back(part);
		}
		std::wstring key;
		for (const auto& part : parts) {
			if (!key.empty()) {
				key += L'\\';
			}
			key += part;
		}
		return Lower(std::move(key));
	}

	bool UnderRoot(const std::wstring& a_key)
	{
		return a_key.size() >= root.size() && a_key.compare(0, root.size(), root) == 0 && (a_key.size() == root.size() || a_key[root.size()] == L'\\');
	}

	std::wstring Parent(const std::wstring& a_key)
	{
		const auto slash = a_key.find_last_of(L'\\');
		return slash == std::wstring::npos ? std::wstring{} : a_key.substr(0, slash);
	}

	void Fill(const Entry& a_entry, LPWIN32_FIND_DATAW a_data)
	{
		*a_data = {};
		a_data->dwFileAttributes = a_entry.attributes;
		a_data->ftCreationTime = a_entry.created;
		a_data->ftLastAccessTime = a_entry.accessed;
		a_data->ftLastWriteTime = a_entry.written;
		a_data->nFileSizeHigh = a_entry.sizeHigh;
		a_data->nFileSizeLow = a_entry.sizeLow;
		a_data->dwReserved0 = a_entry.reparseTag;
		wcsncpy_s(a_data->cFileName, a_entry.name.c_str(), _TRUNCATE);
	}

	void Walk()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
		const auto  begun = std::chrono::steady_clock::now();
		std::size_t entries = 0;
		std::size_t unreachable = 0;
		std::vector<std::wstring> pending{ root };
		const auto ask = [&](const std::wstring& a_path) {
			Answer answer{};
			answer.found = GetFileAttributesExW(a_path.c_str(), GetFileExInfoStandard, &answer.data);
			answer.error = answer.found ? ERROR_SUCCESS : GetLastError();
			if (!answer.found) {
				++unreachable;
			}
			answers.emplace(a_path, answer);
		};
		ask(root);
		while (!pending.empty()) {
			const std::wstring directory = std::move(pending.back());
			pending.pop_back();
			WIN32_FIND_DATAW data{};
			const HANDLE     search = FindFirstFileExW((directory + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
			if (search == INVALID_HANDLE_VALUE) {
				continue;
			}
			Listing listing;
			do {
				Entry entry{ data.dwFileAttributes, data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime, data.nFileSizeHigh, data.nFileSizeLow, data.dwReserved0, data.cFileName };
				const std::wstring_view name = data.cFileName;
				if (name != L"." && name != L"..") {
					const std::wstring child = directory + L"\\" + Lower(entry.name);
					ask(child);
					if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) && !(entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
						pending.push_back(child);
					}
				}
				listing.push_back(std::move(entry));
			} while (FindNextFileW(search, &data));
			FindClose(search);
			entries += listing.size();
			listings.emplace(directory, std::move(listing));
		}
		SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
		const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begun);
		ready = true;
		Log("walked {}: {} folders, {} listed entries, {} paths of which {} Windows will not open, in {} ms", Narrow(root), listings.size(), entries, answers.size(), unreachable, took.count());
	}

	const Listing* CachedListing(LPCWSTR a_pattern, FINDEX_INFO_LEVELS a_level, FINDEX_SEARCH_OPS a_op, LPVOID a_filter)
	{
		if (!serving || a_level != FindExInfoBasic || a_op != FindExSearchNameMatch || a_filter || !a_pattern) {
			return nullptr;
		}
		std::wstring_view pattern = a_pattern;
		if (!(pattern.ends_with(L"\\*") || pattern.ends_with(L"/*"))) {
			return nullptr;
		}
		const auto key = Key(pattern.substr(0, pattern.size() - 2));
		if (!UnderRoot(key)) {
			return nullptr;
		}
		if (!ready) {
			++early;
			return nullptr;
		}
		const auto it = listings.find(key);
		return it == listings.end() ? nullptr : &it->second;
	}

	const Answer* CachedAttributes(const std::wstring& a_path)
	{
		static constexpr Answer missing{ FALSE, ERROR_FILE_NOT_FOUND, {} };
		const auto              key = Key(a_path);
		if (!serving || !UnderRoot(key)) {
			return nullptr;
		}
		if (!ready) {
			++early;
			return nullptr;
		}
		if (const auto it = answers.find(key); it != answers.end()) {
			return &it->second;
		}
		return listings.contains(Parent(key)) ? &missing : nullptr;
	}

	void Difference(std::string a_what)
	{
		if (++differences <= kShownDifferences) {
			Log("difference: {}", a_what);
		}
	}

	bool Same(const Entry& a_cached, const WIN32_FIND_DATAW& a_real)
	{
		return a_cached.attributes == a_real.dwFileAttributes && a_cached.sizeHigh == a_real.nFileSizeHigh && a_cached.sizeLow == a_real.nFileSizeLow && a_cached.name == a_real.cFileName;
	}

	std::string Describe(const WIN32_FIND_DATAW& a_data)
	{
		return std::format("{} ({:X}, {} bytes)", Narrow(a_data.cFileName), a_data.dwFileAttributes, (std::uint64_t{ a_data.nFileSizeHigh } << 32) | a_data.nFileSizeLow);
	}

	std::string Describe(const Entry& a_entry)
	{
		return std::format("{} ({:X}, {} bytes)", Narrow(a_entry.name), a_entry.attributes, (std::uint64_t{ a_entry.sizeHigh } << 32) | a_entry.sizeLow);
	}

	HANDLE WINAPI CachedFindFirst(LPCWSTR a_pattern, FINDEX_INFO_LEVELS a_level, LPVOID a_data, FINDEX_SEARCH_OPS a_op, LPVOID a_filter, DWORD a_flags)
	{
		const Listing* listing = CachedListing(a_pattern, a_level, a_op, a_filter);
		if (!listing) {
			++passed;
			return originalFindFirst(a_pattern, a_level, a_data, a_op, a_filter, a_flags);
		}
		const auto data = static_cast<LPWIN32_FIND_DATAW>(a_data);
		if (use) {
			++served;
			if (listing->empty()) {
				SetLastError(ERROR_FILE_NOT_FOUND);
				return INVALID_HANDLE_VALUE;
			}
			Fill(listing->front(), data);
			const auto search = new Search{ listing, 1 };
			SetLastError(ERROR_SUCCESS);
			return reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(search) | kOurSearch);
		}
		++compared;
		const HANDLE real = originalFindFirst(a_pattern, a_level, a_data, a_op, a_filter, a_flags);
		const DWORD  error = GetLastError();
		if (real == INVALID_HANDLE_VALUE) {
			if (!listing->empty()) {
				Difference(std::format("{} failed with {}, the cache lists {} entries", Narrow(a_pattern), error, listing->size()));
			}
		} else if (listing->empty()) {
			Difference(std::format("{} lists {}, the cache lists nothing", Narrow(a_pattern), Describe(*data)));
		} else {
			if (!Same(listing->front(), *data)) {
				Difference(std::format("{} entry 0 is {}, the cache has {}", Narrow(a_pattern), Describe(*data), Describe(listing->front())));
			}
			std::scoped_lock lock(checkLock);
			checked[real] = { listing, 1 };
		}
		SetLastError(error);
		return real;
	}

	BOOL WINAPI CachedFindNext(HANDLE a_search, LPWIN32_FIND_DATAW a_data)
	{
		if (reinterpret_cast<std::uintptr_t>(a_search) & kOurSearch) {
			const auto search = reinterpret_cast<Search*>(reinterpret_cast<std::uintptr_t>(a_search) & ~kOurSearch);
			if (search->next >= search->listing->size()) {
				SetLastError(ERROR_NO_MORE_FILES);
				return FALSE;
			}
			Fill((*search->listing)[search->next++], a_data);
			SetLastError(ERROR_SUCCESS);
			return TRUE;
		}
		const BOOL  result = originalFindNext(a_search, a_data);
		const DWORD error = GetLastError();
		{
			std::scoped_lock lock(checkLock);
			if (const auto it = checked.find(a_search); it != checked.end()) {
				auto& search = it->second;
				if (result) {
					if (search.next >= search.listing->size()) {
						Difference(std::format("a listing goes on past the cache's {} entries with {}", search.listing->size(), Describe(*a_data)));
					} else if (!Same((*search.listing)[search.next], *a_data)) {
						Difference(std::format("entry {} is {}, the cache has {}", search.next, Describe(*a_data), Describe((*search.listing)[search.next])));
					}
					++search.next;
				} else if (search.next != search.listing->size()) {
					Difference(std::format("a listing ended after {} entries with {}, the cache has {}", search.next, error, search.listing->size()));
				}
			}
		}
		SetLastError(error);
		return result;
	}

	BOOL WINAPI CachedFindClose(HANDLE a_search)
	{
		if (reinterpret_cast<std::uintptr_t>(a_search) & kOurSearch) {
			delete reinterpret_cast<Search*>(reinterpret_cast<std::uintptr_t>(a_search) & ~kOurSearch);
			return TRUE;
		}
		{
			std::scoped_lock lock(checkLock);
			checked.erase(a_search);
		}
		return originalFindClose(a_search);
	}

	template <class Real>
	BOOL Attributes(const std::wstring& a_path, LPVOID a_data, Real a_real)
	{
		const Answer* cached = CachedAttributes(a_path);
		if (!cached) {
			++passed;
			return a_real();
		}
		if (use) {
			++served;
			if (!cached->found) {
				SetLastError(cached->error);
				return FALSE;
			}
			*static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(a_data) = cached->data;
			SetLastError(ERROR_SUCCESS);
			return TRUE;
		}
		++compared;
		const BOOL  result = a_real();
		const DWORD error = GetLastError();
		const auto  real = static_cast<const WIN32_FILE_ATTRIBUTE_DATA*>(a_data);
		if (!result && cached->found) {
			Difference(std::format("attributes of {} failed with {}, the cache has {:X}", Narrow(a_path), error, cached->data.dwFileAttributes));
		} else if (result && !cached->found) {
			Difference(std::format("attributes of {} are {:X}, the cache has it failing with {}", Narrow(a_path), real->dwFileAttributes, cached->error));
		} else if (result && (real->dwFileAttributes != cached->data.dwFileAttributes || real->nFileSizeLow != cached->data.nFileSizeLow || real->nFileSizeHigh != cached->data.nFileSizeHigh)) {
			Difference(std::format("attributes of {} are {:X} and {} bytes, the cache has {:X} and {} bytes", Narrow(a_path), real->dwFileAttributes, real->nFileSizeLow, cached->data.dwFileAttributes, cached->data.nFileSizeLow));
		}
		SetLastError(error);
		return result;
	}

	BOOL WINAPI CachedAttributesW(LPCWSTR a_path, GET_FILEEX_INFO_LEVELS a_level, LPVOID a_data)
	{
		if (!a_path || a_level != GetFileExInfoStandard) {
			++passed;
			return originalAttributesW(a_path, a_level, a_data);
		}
		return Attributes(a_path, a_data, [&] { return originalAttributesW(a_path, a_level, a_data); });
	}

	BOOL WINAPI CachedAttributesA(LPCSTR a_path, GET_FILEEX_INFO_LEVELS a_level, LPVOID a_data)
	{
		if (!a_path || a_level != GetFileExInfoStandard) {
			++passed;
			return originalAttributesA(a_path, a_level, a_data);
		}
		const int    size = MultiByteToWideChar(CP_ACP, 0, a_path, -1, nullptr, 0);
		std::wstring path(size > 0 ? size - 1 : 0, L'\0');
		if (size > 0) {
			MultiByteToWideChar(CP_ACP, 0, a_path, -1, path.data(), size);
		}
		return Attributes(path, a_data, [&] { return originalAttributesA(a_path, a_level, a_data); });
	}

	void Install()
	{
		const HMODULE oar = GetModuleHandleW(L"OpenAnimationReplacer.dll");
		if (!oar) {
			Log("OpenAnimationReplacer.dll is not loaded, nothing to do");
			serving = false;
			return;
		}
		std::string missing;
		if (!RedirectImport(oar, "FindFirstFileExW", &CachedFindFirst, originalFindFirst)) {
			missing += " FindFirstFileExW";
		}
		if (!RedirectImport(oar, "FindNextFileW", &CachedFindNext, originalFindNext)) {
			missing += " FindNextFileW";
		}
		if (!RedirectImport(oar, "FindClose", &CachedFindClose, originalFindClose)) {
			missing += " FindClose";
		}
		if (!RedirectImport(oar, "GetFileAttributesExW", &CachedAttributesW, originalAttributesW)) {
			missing += " GetFileAttributesExW";
		}
		if (!RedirectImport(oar, "GetFileAttributesExA", &CachedAttributesA, originalAttributesA)) {
			missing += " GetFileAttributesExA";
		}
		if (!originalFindFirst || !originalFindNext || !originalFindClose) {
			Log("OpenAnimationReplacer.dll does not import{}, listings stay with Windows", missing);
		} else if (!missing.empty()) {
			Log("OpenAnimationReplacer.dll does not import{}", missing);
		}
		Log("{} OpenAnimationReplacer's calls under {}", use ? "answering" : "checking", Narrow(root));
	}

	void Report(std::string_view a_when)
	{
		Log("{}: {} calls answered from memory, {} checked against Windows, {} passed on, {} came before the walk finished, {} differences",
			a_when, served.load(), compared.load(), passed.load(), early.load(), differences.load());
	}

	class MenuWatch : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME && serving.exchange(false)) {
				Report("main menu, every call goes to Windows from now on");
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
		case SKSE::MessagingInterface::kInputLoaded:
			if (serving) {
				std::thread(Walk).detach();
			}
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
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, false);
	if (const auto directory = SKSE::log::log_directory()) {
		logFile.open(*directory / "hkyssOarCache.log", std::ios::trunc);
	}
	Log("hkyss OAR cache on Skyrim {}", a_skse->RuntimeVersion().string());
	std::string   word;
	std::ifstream file(kModeFile);
	file >> word;
	use = word != "check";
	std::wstring current(MAX_PATH * 4, L'\0');
	current.resize(GetCurrentDirectoryW(static_cast<DWORD>(current.size()), current.data()));
	workingDirectory = current;
	root = Key(L"data\\meshes");
	Log("mode {}, from {}", use ? "use" : "check", word.empty() ? "the default"s : kModeFile);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
