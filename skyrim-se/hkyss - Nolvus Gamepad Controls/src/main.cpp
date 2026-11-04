#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::literals;

namespace
{
	using Clock = std::chrono::steady_clock;
	using InputSink = RE::BSTEventSink<RE::InputEvent*>;

	constexpr auto kSettings = "Data/SKSE/Plugins/hkyssNolvusGamepadControls.ini";
	constexpr auto kProbe = "Data/SKSE/Plugins/hkyssNolvusGamepadControls_probe.txt";

	std::ofstream logFile;
	std::mutex    logLock;

	template <class... Args>
	void Log(std::format_string<Args...> a_format, Args&&... a_args)
	{
		std::scoped_lock lock(logLock);
		if (logFile.is_open()) {
			logFile << std::format("{:%T} ", std::chrono::floor<std::chrono::milliseconds>(std::chrono::current_zone()->to_local(std::chrono::system_clock::now()))) << std::format(a_format, std::forward<Args>(a_args)...) << std::endl;
		}
	}

	void OpenLog()
	{
		auto directory = SKSE::log::log_directory();
		if (!directory) {
			return;
		}
		std::error_code error;
		std::filesystem::create_directories(*directory, error);
		logFile.open(*directory / "hkyssNolvusGamepadControls.log", std::ios::trunc);
	}

	std::string Version()
	{
		const auto plugin = SKSE::PluginDeclaration::GetSingleton();
		if (!plugin) {
			return "unknown";
		}
		const auto version = plugin->GetVersion();
		return version.patch() ? std::format("{}.{}.{}", version.major(), version.minor(), version.patch()) : std::format("{}.{}", version.major(), version.minor());
	}

	std::string_view Trim(std::string_view a_text)
	{
		const auto first = a_text.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos) {
			return {};
		}
		return a_text.substr(first, a_text.find_last_not_of(" \t\r\n") - first + 1);
	}

	bool SameKey(std::string_view a_left, std::string_view a_right)
	{
		return a_left.size() == a_right.size() &&
		       std::equal(a_left.begin(), a_left.end(), a_right.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
	}

	std::optional<std::uint32_t> Number(std::string_view a_text)
	{
		std::uint32_t value = 0;
		const auto    end = a_text.data() + a_text.size();
		const auto [stop, error] = std::from_chars(a_text.data(), end, value);
		if (a_text.empty() || error != std::errc{} || stop != end) {
			return std::nullopt;
		}
		return value;
	}

	struct IniLine
	{
		std::string section;
		std::string key;
		std::string value;
	};

	std::vector<IniLine> ReadIni(const std::filesystem::path& a_path)
	{
		std::vector<IniLine> lines;
		std::ifstream        file(a_path);
		std::string          text;
		std::string          section;
		bool                 first = true;
		while (std::getline(file, text)) {
			if (std::exchange(first, false) && text.starts_with("\xEF\xBB\xBF")) {
				text.erase(0, 3);
			}
			const auto line = Trim(text);
			if (line.empty() || line.front() == ';' || line.front() == '#') {
				continue;
			}
			if (line.front() == '[') {
				section = std::string(Trim(line.substr(1, line.find(']') - 1)));
				continue;
			}
			const auto equals = line.find('=');
			if (equals != std::string_view::npos) {
				lines.push_back({ section, std::string(Trim(line.substr(0, equals))), std::string(Trim(line.substr(equals + 1))) });
			}
		}
		return lines;
	}

	struct Button
	{
		std::string_view name;
		std::uint32_t    id;
	};

	constexpr Button kButtons[]{
		{ "DPadUp"sv, 0x0001 },
		{ "DPadDown"sv, 0x0002 },
		{ "DPadLeft"sv, 0x0004 },
		{ "DPadRight"sv, 0x0008 },
		{ "Start"sv, 0x0010 },
		{ "View"sv, 0x0020 },
		{ "LeftStick"sv, 0x0040 },
		{ "RightStick"sv, 0x0080 },
		{ "LB"sv, 0x0100 },
		{ "RB"sv, 0x0200 },
		{ "A"sv, 0x1000 },
		{ "B"sv, 0x2000 },
		{ "X"sv, 0x4000 },
		{ "Y"sv, 0x8000 },
		{ "LT"sv, 0x0009 },
		{ "RT"sv, 0x000A },
	};

	std::string_view ButtonName(std::uint32_t a_id)
	{
		const auto button = std::ranges::find(kButtons, a_id, &Button::id);
		return button == std::end(kButtons) ? "unknown"sv : button->name;
	}

	const Button* FindButton(std::string_view a_name)
	{
		const auto button = std::ranges::find_if(kButtons, [&](const Button& a_button) { return SameKey(a_button.name, a_name); });
		return button == std::end(kButtons) ? nullptr : &*button;
	}

	enum class Kind
	{
		kNone,
		kKey,
		kEvent
	};

	struct Option
	{
		std::string              source;
		std::vector<std::string> modules;
	};

	struct Action
	{
		Kind                     kind = Kind::kNone;
		std::vector<Option>      options;
		std::string              source;
		std::vector<std::string> modules;
		RE::BSFixedString        event;
		std::uint32_t            key = 0;
		RE::ButtonEvent*         made = nullptr;
		std::vector<InputSink*>  sinks;
		bool                     reported = false;
	};

	std::string Listeners(const Action& a_action)
	{
		if (a_action.modules.empty()) {
			return "SKSE";
		}
		std::string names;
		for (const auto& module : a_action.modules) {
			names += std::format("{}{}", names.empty() ? "" : ", ", module);
		}
		return names;
	}

	struct Mapping
	{
		const Button*              button;
		Action                     press;
		Action                     tap;
		Action                     hold;
		std::vector<std::uint32_t> blockers;
		bool                       passed = false;
		bool                       heldBack = false;
		bool                       holding = false;
		bool                       tapUp = false;
		bool                       looting = false;
		bool                       down = false;
	};

	struct Watch
	{
		std::uint32_t id;
		int           events;
	};

	std::vector<Mapping>       mappings;
	std::vector<std::uint32_t> heard;
	std::vector<Watch>         watching;
	InputSink*                 skse = nullptr;
	float                      holdSeconds = 0.25f;

	void Use(Action& a_action, std::size_t a_option)
	{
		a_action.source = a_action.options[a_option].source;
		a_action.modules = a_action.options[a_option].modules;
		a_action.sinks.assign(a_action.modules.size(), nullptr);
		a_action.reported = false;
	}

	Action ParseAction(std::string_view a_text)
	{
		Action action;
		auto   text = Trim(a_text);
		if (text.starts_with("event ")) {
			action.kind = Kind::kEvent;
			action.source = std::string(Trim(text.substr(6)));
			action.event = RE::BSFixedString(action.source.c_str());
			return action;
		}
		for (auto rest = text; !rest.empty();) {
			const auto split = rest.find(" or ");
			auto       part = Trim(rest.substr(0, split));
			rest = split == std::string_view::npos ? std::string_view{} : rest.substr(split + 4);
			if (part.starts_with("key ")) {
				part = Trim(part.substr(4));
			}
			Option     option;
			const auto to = part.rfind(" to ");
			if (to != std::string_view::npos) {
				for (auto names = part.substr(to + 4); !names.empty();) {
					const auto comma = names.find(',');
					const auto module = Trim(names.substr(0, comma));
					names = comma == std::string_view::npos ? std::string_view{} : names.substr(comma + 1);
					if (module.size() <= 4 || !SameKey(module.substr(module.size() - 4), ".dll")) {
						option.modules.clear();
						break;
					}
					option.modules.emplace_back(module);
				}
				if (!option.modules.empty()) {
					part = Trim(part.substr(0, to));
				}
			}
			option.source = std::string(part);
			if (!option.source.empty()) {
				action.options.push_back(std::move(option));
			}
		}
		if (!action.options.empty()) {
			action.kind = Kind::kKey;
			Use(action, 0);
		}
		return action;
	}

	std::string Describe(const Action& a_action)
	{
		switch (a_action.kind) {
		case Kind::kEvent:
			return std::format("the game's {}", a_action.source);
		case Kind::kKey:
			return std::format("key {} to {}", a_action.key, Listeners(a_action));
		default:
			return "nothing";
		}
	}

	std::pair<std::uint32_t, std::string> Resolve(std::string_view a_source)
	{
		if (const auto number = Number(a_source)) {
			return { *number, "written in this mod's file" };
		}
		const auto slash = a_source.find('/');
		const auto colon = a_source.rfind(':');
		if (slash == std::string_view::npos || colon == std::string_view::npos || colon < slash) {
			return { 0, std::format("{} is neither a key nor a menu setting", a_source) };
		}
		const auto mod = std::string(a_source.substr(0, slash));
		const auto key = a_source.substr(slash + 1, colon - slash - 1);
		const auto section = a_source.substr(colon + 1);
		const auto mcm = std::filesystem::path("Data") / "MCM";
		const std::pair<std::filesystem::path, std::string> files[]{
			{ mcm / "Config" / mod / "settings.ini", std::format("MCM\\Config\\{}\\settings.ini", mod) },
			{ mcm / "Settings" / (mod + ".ini"), std::format("MCM\\Settings\\{}.ini", mod) },
		};
		std::uint32_t found = 0;
		std::string   from = std::format("{} is in neither menu file of {}", key, mod);
		for (const auto& [path, name] : files) {
			for (const auto& line : ReadIni(path)) {
				if (!SameKey(line.section, section) || !SameKey(line.key, key)) {
					continue;
				}
				if (const auto number = Number(line.value)) {
					found = *number;
					from = std::format("{} in {}", key, name);
				}
			}
		}
		return { found, from };
	}

	RE::BSFixedString Control(std::uint32_t a_key)
	{
		const auto map = RE::ControlMap::GetSingleton();
		const auto name = map ? map->GetUserEventName(a_key, RE::INPUT_DEVICE::kKeyboard) : ""sv;
		return RE::BSFixedString(name.empty() ? "" : name.data());
	}

	Mapping& MappingFor(const Button* a_button)
	{
		const auto mapping = std::ranges::find_if(mappings, [&](const Mapping& a_mapping) { return a_mapping.button == a_button; });
		if (mapping != mappings.end()) {
			return *mapping;
		}
		return mappings.emplace_back(Mapping{ a_button });
	}

	void Load()
	{
		const auto lines = ReadIni(kSettings);
		for (const auto& line : lines) {
			if (SameKey(line.section, "Timing")) {
				if (SameKey(line.key, "HoldSeconds")) {
					try {
						holdSeconds = std::clamp(std::stof(line.value), 0.05f, 2.0f);
					} catch (...) {
						Log("HoldSeconds = {} is not a number, keeping {:.2f}", line.value, holdSeconds);
					}
				}
				continue;
			}
			Action Mapping::*slot = SameKey(line.section, "Press") || SameKey(line.section, "Buttons") ? &Mapping::press :
			                        SameKey(line.section, "Tap")                                         ? &Mapping::tap :
			                        SameKey(line.section, "Hold")                                        ? &Mapping::hold :
			                                                                                               nullptr;
			if (!slot && !SameKey(line.section, "NotWhileHeld")) {
				Log("{} in [{}] skipped, this file has [Press], [Tap], [Hold], [NotWhileHeld] and [Timing]", line.key, line.section);
				continue;
			}
			const auto button = FindButton(line.key);
			if (!button) {
				Log("{} in [{}] is not a gamepad button, skipped", line.key, line.section);
				continue;
			}
			auto& mapping = MappingFor(button);
			if (slot) {
				mapping.*slot = ParseAction(line.value);
				continue;
			}
			for (auto rest = std::string_view(line.value); !rest.empty();) {
				const auto comma = rest.find(',');
				const auto name = Trim(rest.substr(0, comma));
				rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
				if (const auto blocker = FindButton(name)) {
					mapping.blockers.push_back(blocker->id);
					Log("{} presses nothing while {} is held", mapping.button->name, blocker->name);
				} else {
					Log("{} in [NotWhileHeld] is not a gamepad button, skipped", name);
				}
			}
		}
		Log("{} buttons in SKSE\\Plugins\\hkyssNolvusGamepadControls.ini, a hold starts after {:.2f} s", mappings.size(), holdSeconds);
	}

	void RefreshAction(const Mapping& a_mapping, std::string_view a_slot, Action& a_action)
	{
		if (a_action.kind == Kind::kEvent) {
			if (!a_action.reported) {
				a_action.reported = true;
				Log("{} {} sends the game's {}", a_mapping.button->name, a_slot, a_action.source);
			}
			return;
		}
		if (a_action.kind != Kind::kKey) {
			return;
		}
		auto [key, from] = Resolve(a_action.options.front().source);
		std::size_t chosen = 0;
		for (std::size_t i = 1; !key && i < a_action.options.size(); ++i) {
			const auto [other, otherFrom] = Resolve(a_action.options[i].source);
			if (other) {
				from = std::format("{}, since {}", otherFrom, from);
				key = other;
				chosen = i;
			}
		}
		const bool moved = a_action.source != a_action.options[chosen].source;
		if (moved) {
			Use(a_action, chosen);
		}
		if (a_action.made && key == a_action.key && !moved) {
			return;
		}
		const auto control = Control(key);
		if (!a_action.made) {
			a_action.made = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, control, key, 0.0f, 0.0f);
		} else {
			a_action.made->SetIDCode(key);
			a_action.made->SetUserEvent(control);
		}
		a_action.key = key;
		if (key) {
			Log("{} {} presses key {}, {}, for {}", a_mapping.button->name, a_slot, key, from, Listeners(a_action));
		} else {
			Log("{} {} presses nothing: {}", a_mapping.button->name, a_slot, from);
		}
	}

	void Refresh()
	{
		for (auto& mapping : mappings) {
			RefreshAction(mapping, "press", mapping.press);
			RefreshAction(mapping, "tap", mapping.tap);
			RefreshAction(mapping, "hold", mapping.hold);
		}
	}

	void LogPad()
	{
		const auto map = RE::ControlMap::GetSingleton();
		if (!map) {
			return;
		}
		std::string line;
		for (const auto& button : kButtons) {
			line += std::format("{}{} '{}'", line.empty() ? "" : ", ", button.name, map->GetUserEventName(button.id, RE::INPUT_DEVICE::kGamepad));
		}
		Log("gameplay events on the pad: {}", line);
	}

	RE::ButtonEvent* PadButton(RE::InputEvent* a_event)
	{
		if (a_event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || a_event->GetDevice() != RE::INPUT_DEVICE::kGamepad) {
			return nullptr;
		}
		return a_event->AsButtonEvent();
	}

	void Count(const RE::ButtonEvent* a_button)
	{
		const auto id = a_button->GetIDCode();
		const auto watch = std::ranges::find(watching, id, &Watch::id);
		if (watch == watching.end()) {
			return;
		}
		if (a_button->IsPressed()) {
			++watch->events;
			return;
		}
		Log("{} was held {:.2f} s and sent {} events while down", ButtonName(id), a_button->HeldDuration(), watch->events);
		watching.erase(watch);
	}

	std::string ModuleOf(const void* a_address)
	{
		HMODULE module = nullptr;
		wchar_t path[MAX_PATH]{};
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_address), &module) ||
			!GetModuleFileNameW(module, path, MAX_PATH)) {
			return "unknown";
		}
		return std::filesystem::path(path).filename().string();
	}

	InputSink* FindSink(std::string_view a_module)
	{
		const auto input = RE::BSInputDeviceManager::GetSingleton();
		if (!input) {
			return nullptr;
		}
		RE::BSSpinLockGuard locker(input->lock);
		for (const auto sink : input->sinks) {
			if (sink && SameKey(ModuleOf(*reinterpret_cast<void* const*>(sink)), a_module)) {
				return sink;
			}
		}
		return nullptr;
	}

	RE::UserEvents::INPUT_CONTEXT_ID Context()
	{
		const auto map = RE::ControlMap::GetSingleton();
		if (!map || map->GetRuntimeData().contextPriorityStack.empty()) {
			return RE::UserEvents::INPUT_CONTEXT_ID::kGameplay;
		}
		return map->GetRuntimeData().contextPriorityStack.back();
	}

	bool InGameplay()
	{
		return Context() == RE::UserEvents::INPUT_CONTEXT_ID::kGameplay;
	}

	std::vector<const RE::InputEvent*> madeEvents;

	bool IsMade(const RE::InputEvent* a_event)
	{
		return std::ranges::find(madeEvents, a_event) != madeEvents.end();
	}

	void SendKey(const Mapping& a_mapping, Action& a_action, float a_value, float a_held, RE::BSTEventSource<RE::InputEvent*>* a_source)
	{
		if (!a_action.made || !a_action.key) {
			return;
		}
		auto& data = a_action.made->GetRuntimeData();
		data.value = a_value;
		data.heldDownSecs = a_held;
		const auto send = [&](InputSink* a_sink) {
			a_action.made->next = nullptr;
			RE::InputEvent* head = a_action.made;
			a_sink->ProcessEvent(&head, a_source);
		};
		if (a_action.modules.empty()) {
			if (skse) {
				send(skse);
			}
			return;
		}
		for (std::size_t i = 0; i < a_action.modules.size(); ++i) {
			if (!a_action.sinks[i]) {
				a_action.sinks[i] = FindSink(a_action.modules[i]);
			}
			if (a_action.sinks[i]) {
				send(a_action.sinks[i]);
			} else if (!a_action.reported) {
				a_action.reported = true;
				Log("{} found no input listener in {}, its key goes nowhere there", a_mapping.button->name, a_action.modules[i]);
			}
		}
	}

	class Mirror : public InputSink
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_source) override
		{
			if (!a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const bool                 gameplay = InGameplay();
			std::vector<std::uint32_t> pressed;
			for (auto event = *a_event; event; event = event->next) {
				if (const auto button = PadButton(event); button && button->IsPressed()) {
					pressed.push_back(button->GetIDCode());
				}
			}
			for (auto event = *a_event; event; event = event->next) {
				const auto button = PadButton(event);
				if (!button || IsMade(button)) {
					continue;
				}
				const auto id = button->GetIDCode();
				const auto mapping = std::ranges::find_if(mappings, [id](const Mapping& a_mapping) { return a_mapping.button->id == id; });
				const bool mapped = mapping != mappings.end() && mapping->press.kind == Kind::kKey && mapping->press.key;
				if (mapped && button->IsDown()) {
					const auto blocker = std::ranges::find_first_of(mapping->blockers, pressed);
					mapping->passed = gameplay && blocker == mapping->blockers.end();
					if (blocker != mapping->blockers.end() && !mapping->heldBack) {
						mapping->heldBack = true;
						Log("{} pressed while {} was held, key {} held back", mapping->button->name, ButtonName(*blocker), mapping->press.key);
					}
				}
				if (button->IsDown() && std::ranges::find(heard, id) == heard.end()) {
					heard.push_back(id);
					watching.push_back({ id, 0 });
					if (mapped) {
						Log("first press of {}, {} {}", mapping->button->name, Describe(mapping->press), mapping->passed ? "sent" : "held back");
					} else {
						Log("first press of {} ({:#06x})", ButtonName(id), id);
					}
				}
				Count(button);
				if (!mapped || !mapping->passed) {
					continue;
				}
				if (!button->IsPressed()) {
					mapping->passed = false;
				}
				SendKey(*mapping, mapping->press, button->IsPressed() ? 1.0f : 0.0f, button->HeldDuration(), a_source);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	Mirror mirror;

	void Attach()
	{
		const auto input = RE::BSInputDeviceManager::GetSingleton();
		if (!input) {
			Log("no input manager, the buttons press nothing");
			return;
		}
		{
			RE::BSSpinLockGuard locker(input->lock);
			for (const auto sink : input->sinks) {
				const auto module = sink ? ModuleOf(*reinterpret_cast<void* const*>(sink)) : "none"s;
				Log("input listener in {}", module);
				if (!skse && sink && module.size() > 7 && SameKey(std::string_view(module).substr(0, 7), "skse64_")) {
					skse = sink;
				}
			}
		}
		input->AddEventSink(&mirror);
		if (skse) {
			Log("found the SKSE key events, buttons go there");
		} else {
			Log("found no SKSE key events, the buttons press nothing");
		}
	}

	constexpr auto          kWheelerControls = "Data/SKSE/Plugins/wheeler/Controls.ini";
	constexpr std::uint32_t kFirstPadCode = 266;
	constexpr std::uintptr_t kDispatchCall = 0x7B;

	std::vector<std::uint32_t> wheelerToggles;

	void ReadWheeler()
	{
		std::vector<std::uint32_t> toggles;
		for (const auto& line : ReadIni(kWheelerControls)) {
			if (!SameKey(line.section, "InputBindings.GamePad") || !(SameKey(line.key, "toggleWheel") || SameKey(line.key, "toggleWheelIfNotInInventory"))) {
				continue;
			}
			const auto code = Number(line.value);
			if (code && *code >= kFirstPadCode && *code < kFirstPadCode + std::size(kButtons)) {
				toggles.push_back(kButtons[*code - kFirstPadCode].id);
			}
		}
		if (toggles == wheelerToggles) {
			return;
		}
		wheelerToggles = toggles;
		if (wheelerToggles.empty()) {
			Log("Wheeler opens on no gamepad button");
		}
		for (const auto id : wheelerToggles) {
			Log("Wheeler opens on {}, which menus get without Wheeler", ButtonName(id));
		}
	}

	constexpr auto kQuickLootConfig = "Data/SKSE/Plugins/QuickLootIE/DefaultConfig.json";
	constexpr auto kLootMenu = "LootMenu"sv;

	std::vector<std::uint32_t> lootButtons;

	void ReadQuickLoot()
	{
		std::ifstream file(kQuickLootConfig);
		if (!file) {
			Log("found no QuickLoot settings, its buttons do what this mod gives them");
			return;
		}
		const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		const std::regex  binding(R"re("Keybinding(\w+)Gamepad"\s*:\s*(-?\d+))re");
		for (auto it = std::sregex_iterator(text.begin(), text.end(), binding); it != std::sregex_iterator(); ++it) {
			const auto code = Number((*it)[2].str());
			if (!code || *code < kFirstPadCode || *code >= kFirstPadCode + std::size(kButtons)) {
				continue;
			}
			const auto id = kButtons[*code - kFirstPadCode].id;
			if (std::ranges::find(lootButtons, id) == lootButtons.end()) {
				lootButtons.push_back(id);
			}
			Log("QuickLoot uses {} for {}, which does nothing else while its menu is open", ButtonName(id), (*it)[1].str());
		}
		if (const auto back = FindButton("B"sv); back && std::ranges::find(lootButtons, back->id) == lootButtons.end()) {
			lootButtons.push_back(back->id);
			Log("B, the button that leaves a menu, does nothing else while QuickLoot's menu is open");
		}
	}

	bool LootButton(std::uint32_t a_id)
	{
		const auto ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(kLootMenu) && std::ranges::find(lootButtons, a_id) != lootButtons.end();
	}

	void Emit(Action& a_action, std::uint32_t a_id, float a_value, float a_held, RE::InputEvent**& a_tail)
	{
		if (!a_action.made) {
			a_action.made = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kGamepad, a_action.event, a_id, a_value, a_held);
			if (!a_action.made) {
				return;
			}
			madeEvents.push_back(a_action.made);
		}
		auto& data = a_action.made->GetRuntimeData();
		data.value = a_value;
		data.heldDownSecs = a_held;
		a_action.made->next = nullptr;
		*a_tail = a_action.made;
		a_tail = &a_action.made->next;
	}

	void Fire(Mapping& a_mapping, Action& a_action, float a_value, float a_held, RE::InputEvent**& a_tail, RE::BSTEventSource<RE::InputEvent*>* a_source)
	{
		if (a_action.kind == Kind::kEvent) {
			Emit(a_action, a_mapping.button->id, a_value, a_held, a_tail);
		} else if (a_action.kind == Kind::kKey) {
			SendKey(a_mapping, a_action, a_value, a_held, a_source);
		}
	}

	void Layer(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent** a_events)
	{
		static std::vector<std::string> told;
		const auto                      tell = [](const Mapping& a_mapping, std::string_view a_what, const Action& a_action) {
			const auto line = std::format("first {} of {}, {}", a_what, a_mapping.button->name, Describe(a_action));
			if (std::ranges::find(told, line) == told.end()) {
				told.push_back(line);
				Log("{}", line);
			}
		};
		std::vector<RE::ButtonEvent*> buttons;
		RE::InputEvent**              tail = a_events;
		while (*tail) {
			if (const auto button = PadButton(*tail); button && !IsMade(button)) {
				buttons.push_back(button);
			}
			tail = &(*tail)->next;
		}
		for (auto& mapping : mappings) {
			if (mapping.tapUp) {
				mapping.tapUp = false;
				Fire(mapping, mapping.tap, 0.0f, 0.05f, tail, a_source);
			}
		}
		for (const auto button : buttons) {
			const auto id = button->GetIDCode();
			const auto found = std::ranges::find_if(mappings, [id](const Mapping& a_mapping) { return a_mapping.button->id == id; });
			if (found == mappings.end() || (found->tap.kind == Kind::kNone && found->hold.kind == Kind::kNone)) {
				continue;
			}
			auto&       mapping = *found;
			const float held = button->HeldDuration();
			if (button->IsDown()) {
				mapping.holding = false;
				mapping.looting = LootButton(id);
				const auto blocker = std::ranges::find_if(mapping.blockers, [&buttons](std::uint32_t a_blocker) {
					return std::ranges::any_of(buttons, [a_blocker](const RE::ButtonEvent* a_button) { return a_button->GetIDCode() == a_blocker && a_button->IsPressed(); });
				});
				mapping.down = blocker == mapping.blockers.end();
				if (!mapping.down) {
					Log("{} pressed while {} was held, its tap and hold are held back", mapping.button->name, ButtonName(*blocker));
				}
				continue;
			}
			if (!mapping.down) {
				continue;
			}
			if (!button->IsPressed()) {
				mapping.down = false;
			}
			if (mapping.looting) {
				mapping.looting = button->IsPressed();
				continue;
			}
			if (button->IsPressed()) {
				if (mapping.hold.kind == Kind::kNone || held < holdSeconds) {
					continue;
				}
				if (!mapping.holding) {
					mapping.holding = true;
					tell(mapping, "hold", mapping.hold);
					Fire(mapping, mapping.hold, 1.0f, 0.0f, tail, a_source);
				} else {
					Fire(mapping, mapping.hold, 1.0f, held - holdSeconds, tail, a_source);
				}
				continue;
			}
			if (mapping.holding) {
				mapping.holding = false;
				Fire(mapping, mapping.hold, 0.0f, std::max(held - holdSeconds, 0.01f), tail, a_source);
				continue;
			}
			if (mapping.tap.kind != Kind::kNone && held < holdSeconds) {
				tell(mapping, "tap", mapping.tap);
				Fire(mapping, mapping.tap, 1.0f, 0.0f, tail, a_source);
				mapping.tapUp = true;
			}
		}
	}

	struct Dispatch
	{
		static void Call(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent** a_events)
		{
			static std::vector<RE::UserEvents::INPUT_CONTEXT_ID> seen;
			const auto                                          context = Context();
			if (std::ranges::find(seen, context) == seen.end()) {
				seen.push_back(context);
				Log("first frame in input context {}", static_cast<int>(context));
			}
			if (context == RE::UserEvents::INPUT_CONTEXT_ID::kGameplay) {
				if (a_events) {
					Layer(a_source, a_events);
				}
				next(a_source, a_events);
				return;
			}
			for (auto& mapping : mappings) {
				mapping.tapUp = false;
				mapping.holding = false;
				mapping.down = false;
			}
			if (!a_events || !*a_events || wheelerToggles.empty()) {
				next(a_source, a_events);
				return;
			}
			RE::InputEvent*  kept = nullptr;
			RE::InputEvent** keptTail = &kept;
			RE::InputEvent*  aside = nullptr;
			RE::InputEvent** asideTail = &aside;
			for (auto event = *a_events; event;) {
				const auto following = event->next;
				const auto button = PadButton(event);
				auto&      tail = button && std::ranges::find(wheelerToggles, button->GetIDCode()) != wheelerToggles.end() ? asideTail : keptTail;
				event->next = nullptr;
				*tail = event;
				tail = &event->next;
				event = following;
			}
			*a_events = kept;
			next(a_source, a_events);
			if (aside) {
				a_source->SendEvent(&aside);
			}
		}

		static inline REL::Relocation<decltype(Call)> next;
	};

	std::string Target(std::uintptr_t a_address)
	{
		const auto bytes = reinterpret_cast<const std::uint8_t*>(a_address);
		if (bytes[0] == 0xFF && bytes[1] == 0x25 && *reinterpret_cast<const std::int32_t*>(bytes + 2) == 0) {
			return ModuleOf(*reinterpret_cast<void* const*>(bytes + 6));
		}
		return ModuleOf(bytes);
	}

	void HookDispatch()
	{
		REL::Relocation<std::uintptr_t> poll{ RELOCATION_ID(67315, 68617) };
		const auto                      call = poll.address() + kDispatchCall;
		if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8) {
			Log("found no input dispatch call to hook, Wheeler hears its buttons in menus too");
			return;
		}
		Dispatch::next = SKSE::GetTrampoline().write_call<5>(call, Dispatch::Call);
		Log("input dispatch hooked, the call went to {} before", Target(Dispatch::next.address()));
	}

	class WheelerSettings : public RE::BSTEventSink<SKSE::ModCallbackEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
		{
			if (a_event && std::string_view(a_event->eventName.c_str()) == "dmenu_updateSettings" && std::string_view(a_event->strArg.c_str()) == "Wheeler Controls") {
				if (const auto tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask(ReadWheeler);
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	WheelerSettings wheelerSettings;

	class Menus : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && !a_event->opening && std::string_view(a_event->menuName.c_str()) == RE::JournalMenu::MENU_NAME) {
				Refresh();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	Menus menus;

	std::atomic_bool inGame = false;

	std::string Hud(const char* a_path)
	{
		const auto   ui = RE::UI::GetSingleton();
		const auto   hud = ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr;
		RE::GFxValue value;
		if (!hud || !hud->uiMovie || !hud->uiMovie->GetVariable(&value, a_path) || !value.IsNumber()) {
			return "none";
		}
		return std::format("{:.0f}", value.GetNumber());
	}

	bool From(const RE::TESForm* a_form, std::string_view a_file)
	{
		const auto file = a_form ? a_form->GetFile(0) : nullptr;
		return file && SameKey(file->GetFilename(), a_file);
	}

	std::string QuickLight(RE::PlayerCharacter* a_player)
	{
		std::string found;
		for (const auto spell : a_player->GetActorRuntimeData().addedSpells) {
			if (From(spell, "QuickLight.esp")) {
				found += std::format("{}spell {}", found.empty() ? "" : ", ", spell->GetName());
			}
		}
		if (const auto effects = a_player->AsMagicTarget()->GetActiveEffectList()) {
			for (const auto effect : *effects) {
				const auto base = effect ? effect->GetBaseObject() : nullptr;
				if (From(base, "QuickLight.esp")) {
					found += std::format("{}effect {}", found.empty() ? "" : ", ", base->GetName());
				}
			}
		}
		return found.empty() ? "none"s : found;
	}

	void State(std::string_view a_when)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto input = RE::BSInputDeviceManager::GetSingleton();
		Log("{}: compass holder {}, compass {}, quest markers {}, quick light {}, gamepad prompts {}", a_when,
			Hud("_root.HUDMovieBaseInstance.CompassShoutMeterHolder._alpha"),
			Hud("_root.HUDMovieBaseInstance.CompassShoutMeterHolder.Compass._alpha"),
			Hud("_root.HUDMovieBaseInstance.FloatingQuestMarker_mc._alpha"),
			player ? QuickLight(player) : "no player"s,
			input && input->IsGamepadEnabled() ? "on" : "off");
	}

	struct Pad
	{
		std::uint32_t id;
		float         value;
		float         held;
	};

	void Send(std::initializer_list<Pad> a_buttons)
	{
		static std::array<RE::ButtonEvent*, 2> pool{};
		const auto                             input = RE::BSInputDeviceManager::GetSingleton();
		RE::InputEvent*                        head = nullptr;
		RE::InputEvent**                       tail = &head;
		std::size_t                            used = 0;
		for (const auto& pad : a_buttons) {
			if (used == pool.size()) {
				break;
			}
			auto& event = pool[used++];
			if (!event) {
				event = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kGamepad, RE::BSFixedString(""), pad.id, pad.value, pad.held);
			}
			if (!event) {
				return;
			}
			event->SetIDCode(pad.id);
			event->GetRuntimeData().value = pad.value;
			event->GetRuntimeData().heldDownSecs = pad.held;
			event->next = nullptr;
			*tail = event;
			tail = &event->next;
		}
		if (input && head) {
			input->SendEvent(&head);
		}
	}

	struct Release
	{
		std::uint32_t     id = 0;
		float             seconds = 0.0f;
		Clock::time_point at{};
	};

	std::optional<Release> release;

	void Press(std::uint32_t a_id, float a_seconds)
	{
		Send({ { a_id, 1.0f, 0.0f } });
		release = Release{ a_id, a_seconds, Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(a_seconds)) };
		Log("probe pressed {} for {:.2f} s", ButtonName(a_id), a_seconds);
	}

	struct Step
	{
		float at;
		void (*run)();
	};

	constexpr Step kSteps[]{
		{ 0.0f, [] { State("before"); Log("shot s00"); } },
		{ 1.0f, [] { Press(0x0004, 0.15f); } },
		{ 3.0f, [] { State("after a tap of DPadLeft"); Log("shot s01"); } },
		{ 4.0f, [] { Press(0x0004, 0.15f); } },
		{ 6.0f, [] { State("after a second tap of DPadLeft"); Log("shot s02"); } },
		{ 7.0f, [] { Press(0x0008, 0.15f); } },
		{ 10.0f, [] { State("after a tap of DPadRight"); Log("shot s03"); } },
		{ 11.0f, [] { Press(0x0008, 1.0f); } },
		{ 14.0f, [] { State("after holding DPadRight for 1 s"); Log("shot s04"); } },
		{ 15.0f, [] { Press(0x0008, 0.15f); } },
		{ 18.0f, [] { State("after a last tap of DPadRight"); Log("shot s05"); } },
		{ 19.0f, [] { Send({ { 0x0100, 1.0f, 0.5f }, { 0x0008, 1.0f, 0.0f } }); Log("probe pressed DPadRight while LB was held"); } },
		{ 19.2f, [] { Send({ { 0x0100, 1.0f, 0.7f }, { 0x0008, 0.0f, 0.2f } }); } },
		{ 19.4f, [] { Send({ { 0x0100, 0.0f, 0.9f } }); } },
		{ 22.0f, [] { State("after DPadRight with LB held"); Log("shot s06"); } },
	};

	constexpr float kSettleAfterFade = 8.0f;

	void ProbeTick()
	{
		static Clock::time_point clear{};
		static Clock::time_point started{};
		static std::size_t       step = 0;
		if (step >= std::size(kSteps) && !release) {
			return;
		}
		const auto now = Clock::now();
		if (release && now >= release->at) {
			Send({ { release->id, 0.0f, release->seconds } });
			release.reset();
		}
		if (started == Clock::time_point{}) {
			const auto ui = RE::UI::GetSingleton();
			if (!ui || ui->IsMenuOpen(RE::FaderMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
				clear = {};
				return;
			}
			if (clear == Clock::time_point{}) {
				clear = now;
				Log("screen clear, probe starts in {:.0f} s", kSettleAfterFade);
			}
			if (std::chrono::duration<float>(now - clear).count() < kSettleAfterFade) {
				return;
			}
			started = now;
			Log("probe start");
		}
		const float into = std::chrono::duration<float>(now - started).count();
		while (step < std::size(kSteps) && into >= kSteps[step].at) {
			kSteps[step++].run();
		}
		if (step >= std::size(kSteps) && !release) {
			Log("probe done");
		}
	}

	void Tick()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (inGame && player && player->Is3DLoaded()) {
			ProbeTick();
		}
	}

	void Run()
	{
		for (;;) {
			std::this_thread::sleep_for(100ms);
			if (const auto tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask(Tick);
			}
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			HookDispatch();
			break;
		case SKSE::MessagingInterface::kInputLoaded:
			Load();
			Attach();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			Refresh();
			LogPad();
			ReadWheeler();
			ReadQuickLoot();
			if (const auto events = SKSE::GetModCallbackEventSource()) {
				events->AddEventSink(&wheelerSettings);
			}
			if (const auto ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(&menus);
			}
			if (const auto console = RE::ConsoleLog::GetSingleton()) {
				console->Print("hkyss - Nolvus Gamepad Controls %s", Version().c_str());
			}
			if (std::filesystem::exists(kProbe)) {
				Log("probe on");
				std::thread(Run).detach();
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			inGame = false;
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			Refresh();
			ReadWheeler();
			inGame = true;
			Log("game loaded");
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, false);
	OpenLog();
	Log("hkyss - Nolvus Gamepad Controls {} on Skyrim {}", Version(), a_skse->RuntimeVersion().string());
	SKSE::AllocTrampoline(64);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
