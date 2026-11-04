#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <numbers>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::literals;

namespace
{
	using Clock = std::chrono::steady_clock;

	constexpr float         kDegrees = 180.0f / std::numbers::pi_v<float>;
	constexpr int           kMaxInsertions = 8;
	constexpr std::uint32_t kTraceMessage = 0x54524345;
	constexpr float         kRunGait = 0.5f;

	struct Settings
	{
		float enable = 1.0f;
		float strength = 1.0f;
		float idleAmount = 1.0f;
		float idleSpeed = 1.0f;
		float moveAmount = 1.0f;
		float moveRoll = 1.0f;
		float moveYaw = 0.7f;
		float moveSway = 0.6f;
		float moveLead = 1.0f;
		float moveBank = 1.0f;
		float moveVariance = 1.0f;
		float runSpeed = 350.0f;
		float sneaking = 0.5f;
		float aiming = 0.0f;
		float inertiaAmount = 1.0f;
		float inertiaReturn = 1.0f;
		float inertiaDamping = 0.7f;
		float inertiaMaxLag = 2.5f;
		float armLag = 1.0f;
		float armDelay = 0.09f;
		float armDamping = 0.5f;
		float armMaxLag = 10.0f;
		float handStride = 1.0f;
		float handBreath = 1.0f;
		float handImpact = 1.0f;
		float neckBack = 4.6f;
		float neckDown = 3.8f;
		float water = 1.0f;
	};

	Settings settings;

	std::ofstream logFile;
	std::mutex    logLock;

	template <class... Args>
	void Log(std::format_string<Args...> a_format, Args&&... a_args)
	{
		std::scoped_lock lock(logLock);
		if (logFile.is_open()) {
			logFile << std::format(a_format, std::forward<Args>(a_args)...) << std::endl;
		}
	}

	constexpr std::pair<std::string_view, float Settings::*> kSettingKeys[]{
		{ "General:bEnable", &Settings::enable },
		{ "General:fStrength", &Settings::strength },
		{ "Idle:fAmount", &Settings::idleAmount },
		{ "Idle:fSpeed", &Settings::idleSpeed },
		{ "Movement:fAmount", &Settings::moveAmount },
		{ "Movement:fRoll", &Settings::moveRoll },
		{ "Movement:fYaw", &Settings::moveYaw },
		{ "Movement:fSway", &Settings::moveSway },
		{ "Movement:fLead", &Settings::moveLead },
		{ "Movement:fBank", &Settings::moveBank },
		{ "Movement:fVariance", &Settings::moveVariance },
		{ "Movement:fRunSpeed", &Settings::runSpeed },
		{ "Conditions:fSneaking", &Settings::sneaking },
		{ "Conditions:fAiming", &Settings::aiming },
		{ "Inertia:fAmount", &Settings::inertiaAmount },
		{ "Inertia:fReturn", &Settings::inertiaReturn },
		{ "Inertia:fDamping", &Settings::inertiaDamping },
		{ "Inertia:fMaxLag", &Settings::inertiaMaxLag },
		{ "Arms:fLag", &Settings::armLag },
		{ "Arms:fDelay", &Settings::armDelay },
		{ "Arms:fDamping", &Settings::armDamping },
		{ "Arms:fMaxLag", &Settings::armMaxLag },
		{ "Hands:fStride", &Settings::handStride },
		{ "Hands:fBreath", &Settings::handBreath },
		{ "Hands:fImpact", &Settings::handImpact },
		{ "Head:fNeckBack", &Settings::neckBack },
		{ "Head:fNeckDown", &Settings::neckDown },
		{ "Water:fAmount", &Settings::water },
	};

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

	class SettingsFile
	{
	public:
		void Check(std::chrono::steady_clock::time_point a_now)
		{
			if (a_now - _checked < 1s) {
				return;
			}
			_checked = a_now;
			if (_files[0].path.empty()) {
				const auto mcm = std::filesystem::current_path() / "Data" / "MCM";
				_files[0] = { mcm / "Config" / "hkyssHeadSway" / "settings.ini", "MCM\\Config\\hkyssHeadSway\\settings.ini" };
				_files[1] = { mcm / "Settings" / "hkyssHeadSway.ini", "MCM\\Settings\\hkyssHeadSway.ini" };
			}
			bool changed = !_read;
			for (auto& file : _files) {
				std::error_code error;
				auto            time = std::filesystem::last_write_time(file.path, error);
				if (error) {
					time = {};
				}
				if (time != file.time) {
					file.time = time;
					changed = true;
				}
			}
			if (changed) {
				Read();
			}
		}

	private:
		struct File
		{
			std::filesystem::path           path;
			std::string_view                name;
			std::filesystem::file_time_type time{};
		};

		static int ReadInto(const File& a_file, Settings& a_settings)
		{
			std::ifstream file(a_file.path);
			if (!file) {
				return -1;
			}
			std::string line;
			std::string section;
			int         read = 0;
			bool        first = true;
			while (std::getline(file, line)) {
				if (std::exchange(first, false) && line.starts_with("\xEF\xBB\xBF")) {
					line.erase(0, 3);
				}
				const auto text = Trim(line);
				if (text.empty() || text.front() == ';' || text.front() == '#') {
					continue;
				}
				if (text.front() == '[') {
					section = std::string(Trim(text.substr(1, text.find(']') - 1)));
					continue;
				}
				const auto equals = text.find('=');
				if (equals == std::string_view::npos) {
					continue;
				}
				const auto key = section + ":" + std::string(Trim(text.substr(0, equals)));
				const auto value = std::string(Trim(text.substr(equals + 1)));
				const auto known = std::ranges::find_if(kSettingKeys, [&](const auto& a_entry) { return SameKey(a_entry.first, key); });
				if (known == std::end(kSettingKeys)) {
					Log("{} in {} is not a setting of this mod, skipped", key, a_file.name);
					continue;
				}
				try {
					a_settings.*known->second = std::stof(value);
					++read;
				} catch (...) {
					Log("{} = {} in {} is not a number, keeping {}", key, value, a_file.name, a_settings.*known->second);
				}
			}
			return read;
		}

		void Read()
		{
			Settings  next;
			const int defaults = ReadInto(_files[0], next);
			const int own = ReadInto(_files[1], next);
			settings = next;
			_read = true;
			if (defaults < 0) {
				Log("found no {}, using the built in defaults", _files[0].name);
			}
			Log("read {} settings from {} and {} from {}", std::max(defaults, 0), _files[0].name, std::max(own, 0), _files[1].name);
		}

		File                                  _files[2];
		std::chrono::steady_clock::time_point _checked{};
		bool                                  _read = false;
	};

	void OpenLog()
	{
		auto directory = SKSE::log::log_directory();
		if (!directory) {
			return;
		}
		std::error_code error;
		std::filesystem::create_directories(*directory, error);
		logFile.open(*directory / "hkyssHeadSway.log", std::ios::trunc);
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

	std::string NameOf(const RE::NiAVObject* a_object)
	{
		if (!a_object) {
			return "none";
		}
		const char* name = a_object->name.c_str();
		return name && *name ? name : "unnamed";
	}

	std::string ChainOf(const RE::NiAVObject* a_object)
	{
		std::string chain;
		for (auto object = a_object; object; object = object->parent) {
			if (!chain.empty()) {
				chain += " < ";
			}
			chain += NameOf(object);
		}
		return chain.empty() ? "none"s : chain;
	}

	float Wrap(float a_degrees)
	{
		return std::remainder(a_degrees, 360.0f);
	}

	float Seconds(Clock::duration a_duration)
	{
		return std::chrono::duration<float>(a_duration).count();
	}

	float Smoothing(float a_dt, float a_seconds)
	{
		const float x = a_dt / a_seconds;
		return x / (1.0f + x);
	}

	class Trace
	{
	public:
		void Start(float a_seconds)
		{
			_started = Clock::now();
			_until = _started + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(a_seconds));
			Log("trace for {:.0f} s", a_seconds);
			Log("t dt | player heading pitch | view heading pitch roll | sway pitch roll yaw | lag yaw pitch roll | speed z graph state | run gait gear stride phase");
		}

		bool Active(Clock::time_point a_now) const
		{
			return a_now <= _until;
		}

		float Time(Clock::time_point a_now) const
		{
			return Seconds(a_now - _started);
		}

	private:
		Clock::time_point _started;
		Clock::time_point _until;
	};

	Trace trace;

	enum class Foot
	{
		kLeft,
		kRight
	};

	struct Footfall
	{
		Clock::time_point at;
		Foot              foot;
	};

	struct FootTag
	{
		std::string_view name;
		Foot             foot;
	};

	constexpr FootTag kFootTags[]{
		{ "FootLeft"sv, Foot::kLeft },
		{ "FootRight"sv, Foot::kRight },
		{ "FootSprintLeft"sv, Foot::kLeft },
		{ "FootSprintRight"sv, Foot::kRight },
	};

	class Footfalls : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		std::uint32_t Watch(RE::Actor* a_player)
		{
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!a_player->GetAnimationGraphManager(manager) || !manager) {
				return 0;
			}
			const auto active = manager->GetRuntimeData().activeGraph;
			if (manager.get() == _manager) {
				return active;
			}
			_manager = manager.get();
			std::scoped_lock lock(_lock);
			_graphs.clear();
			for (const auto& graph : manager->graphs) {
				if (!graph) {
					continue;
				}
				const auto source = graph->GetEventSource<RE::BSAnimationGraphEvent>();
				source->AddEventSink(this);
				_graphs.push_back({ source, graph->projectName.c_str() });
				Log("listening to animation graph {}", graph->projectName.c_str());
			}
			return active;
		}

		std::vector<Footfall> Take()
		{
			std::scoped_lock lock(_lock);
			return std::exchange(_pending, {});
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source) override
		{
			if (!a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const auto             now = Clock::now();
			const std::string_view tag = a_event->tag.c_str();
			std::scoped_lock       lock(_lock);
			if (trace.Active(now)) {
				std::string_view graph = "?";
				for (const auto& known : _graphs) {
					if (known.source == a_source) {
						graph = known.name;
					}
				}
				Log("event {:.4f} {} {} {}", trace.Time(now), graph, tag, a_event->payload.c_str());
			}
			const auto known = std::ranges::find(kFootTags, tag, &FootTag::name);
			if (known == std::end(kFootTags)) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const auto bit = 1u << static_cast<std::uint32_t>(known - std::begin(kFootTags));
			if (!(_heard & bit)) {
				_heard |= bit;
				Log("heard {} for the first time", tag);
			}
			const auto foot = known->foot;
			if (_hasLast && foot == _last.foot && now - _last.at < 150ms) {
				return RE::BSEventNotifyControl::kContinue;
			}
			_last = { now, foot };
			_hasLast = true;
			_pending.push_back(_last);
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		struct Graph
		{
			RE::BSTEventSource<RE::BSAnimationGraphEvent>* source;
			std::string                                    name;
		};

		std::mutex                         _lock;
		const RE::BSAnimationGraphManager* _manager = nullptr;
		std::vector<Graph>                 _graphs;
		std::vector<Footfall>              _pending;
		Footfall                           _last{};
		bool                               _hasLast = false;
		std::uint32_t                      _heard = 0;
	};

	class Gait
	{
	public:
		void Restart()
		{
			*this = Gait();
		}

		void Update(float a_dt, const RE::NiPoint3& a_position, float a_runSpeed, bool a_sprinting)
		{
			if (_hasLast) {
				const float dx = a_position.x - _last.x;
				const float dy = a_position.y - _last.y;
				const float distance = std::sqrt(dx * dx + dy * dy);
				if (distance < a_runSpeed * a_dt * 4.0f + 40.0f) {
					_speed += (distance / a_dt - _speed) * Smoothing(a_dt, 0.10f);
				}
			}
			_last = a_position;
			_hasLast = true;

			const float target = std::clamp(_speed / a_runSpeed, 0.0f, 1.15f);
			_gait = std::max(0.0f, _gait + (target - _gait) * Smoothing(a_dt, target > _gait ? 0.12f : 0.20f));

			int gear = 0;
			if (_gait > 0.04f) {
				gear = _gait > (_gear > 1 ? kRunGait * 0.85f : kRunGait) ? 2 : 1;
				if (a_sprinting) {
					gear = 3;
				}
			}
			if (gear > 0 && gear < _gear) {
				_dwell += a_dt;
				if (_dwell < 0.12f) {
					gear = _gear;
				} else {
					_dwell = 0.0f;
				}
			} else {
				_dwell = 0.0f;
			}
			_gear = gear;

			constexpr float kWeights[]{ 0.0f, 0.30f, 0.55f, 0.65f };
			_sway += (kWeights[_gear] - _sway) * Smoothing(a_dt, 0.35f);
			if (_sway < 0.001f) {
				_sway = 0.0f;
			}
		}

		float Speed() const { return _speed; }
		float Value() const { return _gait; }
		float Sway() const { return _sway; }
		float Stride() const { return std::min(0.5f * _sway + 0.5f * _gait, 1.1f); }
		int   Gear() const { return _gear; }

	private:
		RE::NiPoint3 _last;
		bool         _hasLast = false;
		float        _speed = 0.0f;
		float        _gait = 0.0f;
		float        _sway = 0.0f;
		float        _dwell = 0.0f;
		int          _gear = 0;
	};

	class Cycle
	{
	public:
		void Restart()
		{
			*this = Cycle();
		}

		void Land(const Footfall& a_footfall, Clock::time_point a_now, bool a_visible)
		{
			const bool restarting = !_hasLast || _quiet > 1.5f * 180.0f / Rate();
			if (restarting) {
				_hasRate = false;
			} else if (a_footfall.foot != _lastFoot) {
				const float interval = Seconds(a_footfall.at - _lastAt);
				if (interval > 0.2f && interval < 1.2f) {
					const float measured = 180.0f / interval;
					auto&       rate = _rates[static_cast<int>(_lastFoot)];
					if (!_hasRate || std::abs(measured - rate) > 0.2f * rate) {
						_rates[0] = _rates[1] = measured;
					} else {
						rate += (measured - rate) * 0.5f;
					}
					_hasRate = true;
				}
			}
			a_visible = a_visible && !restarting;
			_lastFoot = a_footfall.foot;
			_lastAt = a_footfall.at;
			_hasLast = true;
			_quiet = 0.0f;

			const float target = (a_footfall.foot == Foot::kLeft ? 0.0f : 180.0f) + Rate() * Seconds(a_now - a_footfall.at);
			if (trace.Active(a_now)) {
				Log("foot {:.4f} {} error {:.1f} rate {:.0f} visible {}", trace.Time(a_now), a_footfall.foot == Foot::kLeft ? "left" : "right",
					Wrap(_phase - target), Rate(), a_visible);
			}
			if (a_visible) {
				_debt = Wrap(target - _phase);
			} else {
				_phase = std::fmod(target + 360.0f, 360.0f);
				_debt = 0.0f;
			}
			std::uniform_real_distribution<float> spread(-0.1f, 0.1f);
			_varianceTarget = 1.0f + spread(_random) * settings.moveVariance;
		}

		void Advance(float a_dt)
		{
			_quiet += a_dt;
			const float rate = _quiet > 1.5f * 180.0f / Rate() ? 0.0f : Rate();
			_stepping += ((_hasLast && rate > 0.0f ? 1.0f : 0.0f) - _stepping) * Smoothing(a_dt, 0.15f);
			float       step = rate * a_dt;
			if (_debt != 0.0f && step > 0.0f) {
				const float limit = step * 0.85f;
				const float pay = std::clamp(_debt * a_dt / 0.12f, -limit, limit);
				_debt -= pay;
				step += pay;
				if (std::abs(_debt) < 0.5f) {
					_debt = 0.0f;
				}
			}
			_phase = std::fmod(_phase + step, 360.0f);
			_variance += (_varianceTarget - _variance) * Smoothing(a_dt, 0.08f);
		}

		float Phase() const { return _phase; }
		float Variance() const { return _variance; }
		float Stepping() const { return _stepping; }

	private:
		float Rate() const { return _rates[static_cast<int>(_lastFoot)]; }

		float             _phase = 0.0f;
		float             _stepping = 0.0f;
		float             _rates[2]{ 400.0f, 400.0f };
		float             _debt = 0.0f;
		float             _quiet = 0.0f;
		float             _variance = 1.0f;
		float             _varianceTarget = 1.0f;
		bool              _hasRate = false;
		bool              _hasLast = false;
		Foot              _lastFoot = Foot::kLeft;
		Clock::time_point _lastAt;
		std::minstd_rand  _random{ static_cast<std::uint32_t>(Clock::now().time_since_epoch().count()) };
	};

	struct Spring
	{
		float offset = 0.0f;
		float velocity = 0.0f;

		void Push(float a_inject, float a_limit, float a_stiffness, float a_damping, float a_lagTime, float a_dt)
		{
			const float soft = std::abs(a_inject * a_lagTime / a_dt);
			const float knee = a_limit * 0.75f;
			float       scale = 1.0f;
			if (soft > knee) {
				const float over = (soft - knee) * 4.0f / a_limit;
				scale = (knee + a_limit * 0.25f * over / (1.0f + over)) / soft;
			}
			scale = std::sqrt(scale);
			const float stiffness = a_stiffness / (scale * scale);
			const float damping = a_damping / scale;
			offset += a_inject * scale;
			velocity = (velocity - stiffness * offset * a_dt) / (1.0f + damping * a_dt + stiffness * a_dt * a_dt);
			offset += velocity * a_dt;
			if (offset > a_limit) {
				offset = a_limit;
				velocity = std::min(velocity, 0.0f);
			} else if (offset < -a_limit) {
				offset = -a_limit;
				velocity = std::max(velocity, 0.0f);
			}
		}
	};

	struct Pose
	{
		float pitch = 0.0f;
		float roll = 0.0f;
		float yaw = 0.0f;
	};

	struct Conditions
	{
		bool  suppressed = false;
		bool  aiming = false;
		bool  swimming = false;
		bool  airborne = false;
		float multiplier = 1.0f;
	};

	class Sway
	{
	public:
		void Resume()
		{
			_hasLast = false;
		}

		void Restart()
		{
			*this = Sway();
		}

		Pose Update(float a_dt, float a_heading, float a_pitch, const Gait& a_gait, const Cycle& a_cycle, const Conditions& a_conditions)
		{
			_fade = std::clamp(_fade + a_dt * (a_conditions.suppressed ? -8.0f : 4.0f), 0.0f, 1.0f);
			_multiplier += (a_conditions.multiplier - _multiplier) * Smoothing(a_dt, 0.11f);
			_ground += ((a_conditions.swimming || a_conditions.airborne ? 0.0f : 1.0f) - _ground) * Smoothing(a_dt, 0.10f);
			_swim += ((a_conditions.swimming ? 1.0f : 0.0f) - _swim) * Smoothing(a_dt, 0.30f);
			_armGate += ((a_conditions.suppressed || a_conditions.aiming ? 0.0f : 1.0f) - _armGate) * Smoothing(a_dt, 1.0f / 7.0f);

			_breath = std::fmod(_breath + a_dt * 75.6f * settings.idleSpeed, 360.0f);
			const float cycle = _breath < 151.2f ? _breath * 1.19048f : 180.0f + (_breath - 151.2f) * 0.86207f;
			const float breath = std::sin((cycle - 90.0f) / kDegrees);
			_breathWave = breath;
			_drift = std::fmod(_drift + a_dt * 26.3f * settings.idleSpeed, 36000.0f);
			const float posture = std::max(1.0f - 0.70f * a_gait.Value(), 0.3f);

			Pose pose;
			pose.pitch = 0.18f * settings.idleAmount * breath;
			pose.roll = 0.16f * settings.idleAmount * posture * std::sin(_drift / kDegrees);
			pose.yaw = 0.34f * settings.idleAmount * posture * std::sin(_drift * 1.7f / kDegrees);

			const float move = Move(a_gait, a_cycle);
			const float phase = a_cycle.Phase() / kDegrees;
			pose.pitch += 0.85f * move * std::sin(2.0f * phase);
			pose.roll += 0.95f * move * settings.moveRoll * std::sin(phase) * (1.0f + a_gait.Sway() * settings.moveSway * 0.6f);
			pose.yaw += 0.30f * move * settings.moveYaw * std::cos(phase);

			const float swell = _swim * settings.water;
			pose.pitch += 0.85f * swell * std::sin(_drift * 0.8f / kDegrees);
			pose.roll += 1.10f * swell * std::sin((_drift * 0.55f + 60.0f) / kDegrees);

			float turn = 0.0f;
			float nod = 0.0f;
			if (_hasLast && a_dt <= 0.15f && !a_conditions.suppressed) {
				turn = std::clamp(Wrap(a_heading - _lastHeading), -45.0f, 45.0f);
				nod = std::clamp(Wrap(a_pitch - _lastPitch), -45.0f, 45.0f);
			}
			_lastHeading = a_heading;
			_lastPitch = a_pitch;
			_hasLast = true;

			const float rateSmoothing = Smoothing(a_dt, 0.018f);
			_turnRate += (turn / a_dt - _turnRate) * rateSmoothing;
			_nodRate += (nod / a_dt - _nodRate) * rateSmoothing;
			turn = _turnRate * a_dt;
			nod = _nodRate * a_dt;

			const float lead = std::clamp(_turnRate * 0.0025f * a_gait.Sway() * settings.moveLead, -1.2f, 1.2f);
			_lead += (lead - _lead) * Smoothing(a_dt, 0.25f);
			pose.yaw += _lead;
			const float bank = std::clamp(std::clamp(_turnRate / 90.0f, -2.0f, 2.0f) * a_gait.Value() * _ground * 2.5f * settings.moveBank, -4.0f, 4.0f);
			_bank += (bank - _bank) * Smoothing(a_dt, 0.20f);
			pose.roll -= _bank;

			const float stiffness = std::max(55.0f * settings.inertiaReturn, 5.0f);
			const float damping = 2.0f * std::sqrt(stiffness) * std::max(settings.inertiaDamping, 0.2f);
			const float lagGain = settings.inertiaAmount * 0.010f * stiffness / damping;
			const float lagTime = damping / stiffness;
			const float limit = std::max(settings.inertiaMaxLag, 0.05f);
			_yawLag.Push(-turn * lagGain, limit, stiffness, damping, lagTime, a_dt);
			_pitchLag.Push(-nod * lagGain, limit, stiffness, damping, lagTime, a_dt);
			_rollLag.Push(-turn * lagGain * 0.4f, limit * 0.6f, stiffness, damping, lagTime, a_dt);
			pose.yaw += _yawLag.offset;
			pose.pitch += _pitchLag.offset;
			pose.roll += _rollLag.offset;

			const float scale = Scale();
			pose.pitch *= scale;
			pose.roll *= scale;
			pose.yaw *= scale;
			return pose;
		}

		float Move(const Gait& a_gait, const Cycle& a_cycle) const
		{
			return settings.moveAmount * a_gait.Stride() * a_cycle.Variance() * a_cycle.Stepping() * _ground;
		}

		float Scale() const
		{
			return settings.strength * _fade * _multiplier;
		}

		const Spring& YawLag() const { return _yawLag; }
		const Spring& PitchLag() const { return _pitchLag; }
		const Spring& RollLag() const { return _rollLag; }
		float         TurnRate() const { return _turnRate; }
		float         NodRate() const { return _nodRate; }
		float         Breath() const { return _breathWave; }
		float         Drift() const { return _drift; }
		float         Bank() const { return _bank; }
		float         Fade() const { return _fade; }
		float         Ground() const { return _ground; }
		float         ArmGate() const { return _armGate; }

	private:
		float  _multiplier = 1.0f;
		float  _ground = 1.0f;
		float  _swim = 0.0f;
		float  _armGate = 1.0f;
		float  _breath = 0.0f;
		float  _breathWave = -1.0f;
		float  _drift = 0.0f;
		float  _lastHeading = 0.0f;
		float  _lastPitch = 0.0f;
		bool   _hasLast = false;
		float  _turnRate = 0.0f;
		float  _nodRate = 0.0f;
		float  _lead = 0.0f;
		float  _bank = 0.0f;
		Spring _yawLag;
		Spring _pitchLag;
		Spring _rollLag;
		float  _fade = 0.0f;
	};

	RE::NiNode* InsertAbove(RE::NiAVObject* a_child, const char* a_name)
	{
		const auto parent = a_child->parent;
		if (!parent) {
			Log("{} has no parent to insert under", NameOf(a_child));
			return nullptr;
		}
		std::uint32_t index = 0;
		while (index < parent->children.capacity() && parent->children[index].get() != a_child) {
			++index;
		}
		if (index == parent->children.capacity()) {
			Log("{} is not among the children of {}", NameOf(a_child), NameOf(parent));
			return nullptr;
		}

		const auto node = RE::NiNode::Create(1);
		node->name = a_name;
		node->local = RE::NiTransform();
		node->flags.set(RE::NiAVObject::Flag::kSelectiveUpdate, RE::NiAVObject::Flag::kSelectiveUpdateTransforms);
		RE::NiPointer<RE::NiAVObject> held;
		parent->SetAt(index, node, held);
		node->AttachChild(held.get(), true);
		Log("inserted {} between {} and {}", a_name, NameOf(parent), NameOf(a_child));
		return a_child->parent == node ? node : nullptr;
	}

	class Rig
	{
	public:
		bool Attach(RE::NiAVObject* a_camera)
		{
			if (a_camera->parent && a_camera->parent == _node.get()) {
				return true;
			}
			if (_insertions >= kMaxInsertions) {
				if (_insertions++ == kMaxInsertions) {
					Log("gave up after {} insertions", kMaxInsertions);
				}
				return false;
			}
			++_insertions;
			_node.reset(InsertAbove(a_camera, "hkyssHeadSway"));
			return _node != nullptr;
		}

		void Write(RE::NiAVObject* a_camera, const Pose& a_pose)
		{
			RE::NiMatrix3 yaw;
			RE::NiMatrix3 pitch;
			RE::NiMatrix3 roll;
			yaw.MakeZRotation(a_pose.yaw / kDegrees);
			pitch.MakeXRotation(a_pose.pitch / kDegrees);
			roll.MakeYRotation(a_pose.roll / kDegrees);

			RE::NiTransform motion;
			motion.rotate = yaw * pitch * roll;
			const RE::NiPoint3 neck{ 0.0f, -settings.neckBack, -settings.neckDown };
			motion.translate = neck - motion.rotate * neck;

			const auto& local = a_camera->local;
			_node->local = local * motion * local.Invert();
		}

	private:
		RE::NiPointer<RE::NiNode> _node;
		int                       _insertions = 0;
	};

	struct ArmSpring
	{
		float offset = 0.0f;
		float velocity = 0.0f;

		void Chase(float a_inject, float a_delay, float a_damping, float a_limit, float a_dt)
		{
			const float settle = 2.0f * std::abs(a_inject * a_delay / a_dt) / a_limit;
			const float soft = std::sqrt(2.0f / (1.0f + std::sqrt(1.0f + settle * settle)));
			const float omega = 2.0f * a_damping / (a_delay * soft);
			offset += a_inject * soft;
			velocity = (velocity - omega * omega * offset * a_dt) / (1.0f + 2.0f * a_damping * omega * a_dt + omega * omega * a_dt * a_dt);
			offset += velocity * a_dt;
			if (offset > a_limit) {
				offset = a_limit;
				velocity = std::min(velocity, 0.0f);
			} else if (offset < -a_limit) {
				offset = -a_limit;
				velocity = std::max(velocity, 0.0f);
			}
		}
	};

	struct ArmPose
	{
		float pitch = 0.0f;
		float roll = 0.0f;
		float yaw = 0.0f;
		float x = 0.0f;
		float z = 0.0f;
	};

	class Arms
	{
	public:
		void Restart()
		{
			*this = Arms();
		}

		ArmPose Update(float a_dt, const Sway& a_sway, const Gait& a_gait, const Cycle& a_cycle)
		{
			const float rateSmoothing = Smoothing(a_dt, 0.030f);
			_turnRate += (a_sway.TurnRate() - _turnRate) * rateSmoothing;
			_nodRate += (a_sway.NodRate() - _nodRate) * rateSmoothing;
			const float amount = settings.armLag * (1.0f + 0.3f * a_gait.Sway()) * a_sway.ArmGate();
			const float delay = std::max(settings.armDelay, 0.03f);
			const float damping = std::clamp(settings.armDamping, 0.1f, 1.5f);
			const float limit = std::max(settings.armMaxLag, 0.5f);
			_yawLag.Chase(-_turnRate * a_dt * amount, delay, damping, limit, a_dt);
			_pitchLag.Chase(-_nodRate * a_dt * amount, delay, damping, limit, a_dt);

			const float move = a_sway.Move(a_gait, a_cycle);
			const int   half = a_cycle.Phase() < 180.0f ? 0 : 1;
			if (half != _half) {
				_half = half;
				if (a_gait.Stride() > 0.15f) {
					_thudVelocity -= (7.0f + 9.0f * move) * a_sway.Ground() * a_cycle.Stepping();
				}
			}
			_thudVelocity = (_thudVelocity - 640.0f * _thud * a_dt) / (1.0f + 35.0f * a_dt + 640.0f * a_dt * a_dt);
			_thud = std::clamp(_thud + _thudVelocity * a_dt, -1.5f, 1.5f);

			ArmPose     pose;
			const float phase = a_cycle.Phase() / kDegrees;
			const float stride = move * settings.handStride;
			pose.yaw = -2.6f * stride * std::cos(phase);
			pose.roll = 1.4f * stride * std::sin(phase);
			pose.pitch = 0.9f * stride * std::sin(2.0f * phase - 50.0f / kDegrees);
			pose.z = 0.70f * stride * std::sin(2.0f * phase + 220.0f / kDegrees);
			pose.x = 0.35f * stride * std::sin(phase);

			const float breath = settings.idleAmount * settings.handBreath;
			pose.z += 0.45f * breath * a_sway.Breath();
			pose.pitch -= 0.35f * breath * a_sway.Breath();
			pose.x += 0.25f * breath * std::sin((a_sway.Drift() * 0.7f + 30.0f) / kDegrees);
			pose.z += 0.20f * breath * std::sin((a_sway.Drift() * 0.45f + 200.0f) / kDegrees);

			pose.z += _thud * settings.handImpact;
			pose.roll += (half == 0 ? -1.2f : 1.2f) * _thud * settings.handImpact;
			pose.roll -= 0.6f * a_sway.Bank();

			pose.z = std::clamp(pose.z, -3.0f, 3.0f);
			pose.x = std::clamp(pose.x, -2.0f, 2.0f);
			pose.pitch = std::clamp(pose.pitch, -8.0f, 8.0f);
			pose.roll = std::clamp(pose.roll, -6.0f, 6.0f);

			const float hands = a_sway.Scale();
			pose.yaw = pose.yaw * hands + _yawLag.offset * a_sway.Fade();
			pose.pitch = pose.pitch * hands + _pitchLag.offset * a_sway.Fade();
			pose.roll *= hands;
			pose.x *= hands;
			pose.z *= hands;
			return pose;
		}

		const ArmSpring& YawLag() const { return _yawLag; }
		const ArmSpring& PitchLag() const { return _pitchLag; }

	private:
		float     _turnRate = 0.0f;
		float     _nodRate = 0.0f;
		ArmSpring _yawLag;
		ArmSpring _pitchLag;
		int       _half = 0;
		float     _thud = 0.0f;
		float     _thudVelocity = 0.0f;
	};

	class ArmRig
	{
	public:
		bool Attach(RE::NiAVObject* a_root, const RE::NiAVObject* a_camera)
		{
			if (a_root != _root) {
				_root = a_root;
				_neck = nullptr;
				_hand = nullptr;
				const auto left = a_root->GetObjectByName("NPC L Clavicle [LClv]");
				const auto right = a_root->GetObjectByName("NPC R Clavicle [RClv]");
				_hand = a_root->GetObjectByName("NPC R Hand [RHnd]");
				Log("left clavicle: {}", ChainOf(left));
				Log("right clavicle: {}", ChainOf(right));
				for (auto ancestor = left ? left->parent : nullptr; ancestor && !_neck; ancestor = ancestor->parent) {
					for (auto other = right ? right->parent : nullptr; other; other = other->parent) {
						if (other == ancestor) {
							_neck = ancestor;
							break;
						}
					}
				}
				for (auto above = a_camera; above && _neck; above = above->parent) {
					if (above == _neck) {
						Log("{} also carries the camera, the hands stay still", NameOf(_neck));
						_neck = nullptr;
						_reported = true;
					}
				}
				if (_neck) {
					Log("the hands hang from {}", NameOf(_neck));
				} else if (!_reported) {
					Log("found no common ancestor of the clavicles, the hands stay still");
					_reported = true;
				}
			}
			if (!_neck) {
				return false;
			}
			if (_neck->parent && _neck->parent == _node.get()) {
				return true;
			}
			if (_insertions >= kMaxInsertions) {
				return false;
			}
			++_insertions;
			_node.reset(InsertAbove(_neck, "hkyssHeadSwayArms"));
			return _node != nullptr;
		}

		void Write(const ArmPose& a_pose, const RE::NiMatrix3& a_view)
		{
			const auto parent = _node->parent;
			if (!parent) {
				return;
			}
			RE::NiMatrix3 yaw;
			RE::NiMatrix3 pitch;
			RE::NiMatrix3 roll;
			yaw.MakeZRotation(a_pose.yaw / kDegrees);
			pitch.MakeXRotation(a_pose.pitch / kDegrees);
			roll.MakeYRotation(a_pose.roll / kDegrees);
			const auto turn = a_view * (yaw * pitch * roll) * a_view.Transpose();

			const auto pivot = (parent->world * _neck->local).translate;
			RE::NiTransform motion;
			motion.rotate = turn;
			motion.translate = pivot - turn * pivot + a_view * RE::NiPoint3{ a_pose.x, 0.0f, a_pose.z };
			_node->local = parent->world.Invert() * motion * parent->world;
		}

		void Verify()
		{
			if (_verified || !_hand || !_node->parent || ++_frames < 120) {
				return;
			}
			_verified = true;
			RE::NiTransform chain;
			const RE::NiAVObject* object = _hand;
			for (; object && object != _node.get(); object = object->parent) {
				chain = object->local * chain;
			}
			if (!object) {
				Log("{} does not hang from hkyssHeadSwayArms", NameOf(_hand));
				return;
			}
			const auto expected = _node->parent->world * _node->local * chain;
			Log("{} sits {:.3f} units from where hkyssHeadSwayArms puts it", NameOf(_hand), (_hand->world.translate - expected.translate).Length());
		}

		const RE::NiAVObject* Hand() const { return _hand; }

	private:
		RE::NiPointer<RE::NiNode> _node;
		const RE::NiAVObject*     _root = nullptr;
		RE::NiAVObject*           _neck = nullptr;
		RE::NiAVObject*           _hand = nullptr;
		int                       _insertions = 0;
		int                       _frames = 0;
		bool                      _reported = false;
		bool                      _verified = false;
	};

	struct ViewAngles
	{
		float heading = 0.0f;
		float pitch = 0.0f;
		float roll = 0.0f;
	};

	ViewAngles AnglesOfRoot(const RE::NiMatrix3& a_rotation)
	{
		const auto& m = a_rotation.entry;
		return { std::atan2(m[0][1], m[1][1]) * kDegrees, -std::asin(std::clamp(m[2][1], -1.0f, 1.0f)) * kDegrees, std::atan2(m[2][0], m[2][2]) * kDegrees };
	}

	ViewAngles AnglesOfCamera(const RE::NiMatrix3& a_rotation)
	{
		const auto& m = a_rotation.entry;
		return { std::atan2(m[0][0], m[1][0]) * kDegrees, -std::asin(std::clamp(m[2][0], -1.0f, 1.0f)) * kDegrees, std::atan2(m[2][2], m[2][1]) * kDegrees };
	}

	class LateChanges
	{
	public:
		void Before()
		{
			const auto root = CameraRoot();
			const auto camera = RE::Main::WorldRootCamera();
			if (_valid && root && camera) {
				_root = Difference(AnglesOfRoot(root->world.rotate), _rootAfter);
				_camera = Difference(AnglesOfCamera(camera->world.rotate), _cameraAfter);
			}
		}

		void After()
		{
			const auto root = CameraRoot();
			const auto camera = RE::Main::WorldRootCamera();
			_valid = root && camera;
			if (_valid) {
				_rootAfter = AnglesOfRoot(root->world.rotate);
				_cameraAfter = AnglesOfCamera(camera->world.rotate);
			}
		}

		const ViewAngles& RootChange() const { return _root; }
		const ViewAngles& CameraChange() const { return _camera; }

	private:
		static RE::NiNode* CameraRoot()
		{
			const auto camera = RE::PlayerCamera::GetSingleton();
			return camera ? camera->cameraRoot.get() : nullptr;
		}

		static ViewAngles Difference(const ViewAngles& a_now, const ViewAngles& a_then)
		{
			return { Wrap(a_now.heading - a_then.heading), a_now.pitch - a_then.pitch, Wrap(a_now.roll - a_then.roll) };
		}

		ViewAngles _rootAfter;
		ViewAngles _cameraAfter;
		ViewAngles _root;
		ViewAngles _camera;
		bool       _valid = false;
	};

	void LogCameraMods()
	{
		constexpr const wchar_t* kNames[]{ L"CameraShake.dll", L"ImprovedCameraSE.dll", L"ImprovedCamera.dll", L"SmoothCam.dll", L"TrueDirectionalMovement.dll", L"FirstPersonInteractions.dll" };
		for (const auto name : kNames) {
			if (REX::W32::GetModuleHandleW(name)) {
				std::string narrow;
				for (auto p = name; *p; ++p) {
					narrow += static_cast<char>(*p);
				}
				Log("{} is loaded{}", narrow, narrow.starts_with("ImprovedCamera") ? ", which moves the first person camera itself and was never tested with Head Sway" : "");
			}
		}
	}

	LateChanges       lateChanges;
	SettingsFile      settingsFile;
	Footfalls         footfalls;
	Gait              gait;
	Cycle             cycle;
	Sway              sway;
	Rig               rig;
	Arms              arms;
	ArmRig            armRig;
	Clock::time_point lastUpdate;

	Conditions ConditionsOf(RE::Actor* a_player)
	{
		using Sit = RE::SIT_SLEEP_STATE;
		using Attack = RE::ATTACK_STATE_ENUM;
		Conditions conditions;
		if (const auto state = a_player->AsActorState()) {
			const auto sit = state->GetSitSleepState();
			const auto attack = state->GetAttackState();
			conditions.suppressed = sit == Sit::kWantToSleep || sit == Sit::kWaitingForSleepAnim || sit == Sit::kIsSleeping || sit == Sit::kWantToWake;
			conditions.aiming = attack >= Attack::kBowDraw && attack <= Attack::kBowReleasing;
			conditions.swimming = state->IsSwimming();
			conditions.multiplier = (conditions.aiming ? settings.aiming : 1.0f) * (state->IsSneaking() ? settings.sneaking : 1.0f);
		}
		conditions.suppressed = conditions.suppressed || settings.enable == 0.0f || a_player->IsInKillMove() || a_player->IsOnMount();
		conditions.airborne = a_player->IsInMidair();
		return conditions;
	}

	float RunSpeed(RE::Actor* a_player)
	{
		const float mult = a_player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeedMult) / 100.0f;
		return settings.runSpeed * std::clamp(mult, 0.1f, 10.0f);
	}

	std::uint32_t StateOf(RE::Actor* a_player)
	{
		const auto state = a_player->AsActorState();
		if (!state) {
			return 0;
		}
		return (state->IsWalking() ? 1u : 0u) | (state->IsRunning() ? 2u : 0u) | (state->IsSprinting() ? 4u : 0u) |
		       (state->IsSneaking() ? 8u : 0u) | (state->IsSwimming() ? 16u : 0u) | (a_player->IsInMidair() ? 32u : 0u) |
		       (state->IsWeaponDrawn() ? 64u : 0u);
	}

	void Record(Clock::time_point a_now, float a_dt, float a_heading, float a_pitch, const Pose& a_pose, float a_runSpeed, const RE::NiPoint3& a_position, std::uint32_t a_graph, std::uint32_t a_state, const ArmPose& a_arm)
	{
		if (!trace.Active(a_now)) {
			return;
		}
		const auto   camera = RE::PlayerCamera::GetSingleton();
		const auto   root = camera ? camera->cameraRoot.get() : nullptr;
		float        heading = 0.0f;
		float        pitch = 0.0f;
		float        roll = 0.0f;
		RE::NiPoint3 hand;
		if (root) {
			const auto& m = root->world.rotate.entry;
			heading = std::atan2(m[0][1], m[1][1]) * kDegrees;
			pitch = -std::asin(std::clamp(m[2][1], -1.0f, 1.0f)) * kDegrees;
			roll = std::atan2(m[2][0], m[2][2]) * kDegrees;
			if (const auto rightHand = armRig.Hand()) {
				hand = root->world.rotate.Transpose() * (rightHand->world.translate - root->world.translate);
			}
		}
		const auto& lateRoot = lateChanges.RootChange();
		const auto& lateCamera = lateChanges.CameraChange();
		Log("{:.4f} {:.4f} | {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} | {:.2f} {:.2f} {} {} | {:.1f} {:.3f} {} {:.3f} {:.1f} | {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}",
			trace.Time(a_now), a_dt, a_heading, a_pitch, heading, pitch, roll,
			a_pose.pitch, a_pose.roll, a_pose.yaw, sway.YawLag().offset, sway.PitchLag().offset, sway.RollLag().offset,
			gait.Speed(), a_position.z, a_graph, a_state,
			a_runSpeed, gait.Value(), gait.Gear(), gait.Stride(), cycle.Phase(),
			a_arm.pitch, a_arm.roll, a_arm.yaw, a_arm.x, a_arm.z, arms.YawLag().offset, arms.PitchLag().offset,
			hand.x, hand.y, hand.z, sway.Fade(), sway.Scale(), sway.Ground(), cycle.Stepping(),
			lateRoot.heading, lateRoot.pitch, lateRoot.roll, lateCamera.heading, lateCamera.pitch, lateCamera.roll);
	}

	void Update(RE::FirstPersonState* a_state)
	{
		const auto camera = a_state->firstPersonCameraObj;
		if (!camera || !rig.Attach(camera)) {
			return;
		}
		const auto now = Clock::now();
		const auto gap = now - lastUpdate;
		lastUpdate = now;
		if (gap > 1s) {
			sway.Restart();
			gait.Restart();
			cycle.Restart();
			arms.Restart();
		} else if (gap > 250ms) {
			sway.Resume();
		}

		settingsFile.Check(now);
		const float dt = std::min(RE::GetSecondsSinceLastFrame(), 0.2f);
		const auto  player = RE::PlayerCharacter::GetSingleton();
		if (dt <= 0.0f || !player) {
			return;
		}
		const auto graph = footfalls.Watch(player);
		const auto state = StateOf(player);
		const auto position = player->GetPosition();
		const auto runSpeed = RunSpeed(player);
		gait.Update(dt, position, runSpeed, (state & 4u) != 0);
		for (const auto& footfall : footfalls.Take()) {
			cycle.Land(footfall, now, gait.Stride() > 0.15f);
		}
		cycle.Advance(dt);

		const float heading = player->data.angle.z * kDegrees;
		const float pitch = player->data.angle.x * kDegrees;
		const auto  pose = sway.Update(dt, heading, pitch, gait, cycle, ConditionsOf(player));
		rig.Write(camera, pose);

		ArmPose    arm;
		const auto playerCamera = RE::PlayerCamera::GetSingleton();
		const auto view = playerCamera ? playerCamera->cameraRoot.get() : nullptr;
		const auto body = player->Get3D(true);
		if (view && body && armRig.Attach(body, camera)) {
			armRig.Verify();
			arm = arms.Update(dt, sway, gait, cycle);
			armRig.Write(arm, view->world.rotate);
		}
		Record(now, dt, heading, pitch, pose, runSpeed, position, graph, state, arm);
	}

	struct FirstPersonUpdate
	{
		static void Thunk(RE::FirstPersonState* a_state, RE::BSTSmartPointer<RE::TESCameraState>& a_next)
		{
			const bool tracing = trace.Active(Clock::now());
			if (tracing) {
				lateChanges.Before();
			}
			Original(a_state, a_next);
			if (tracing) {
				lateChanges.After();
			}
			Update(a_state);
		}

		static inline REL::Relocation<decltype(Thunk)> Original;
	};

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_FirstPersonState[0] };
		FirstPersonUpdate::Original = vtable.write_vfunc(0x3, FirstPersonUpdate::Thunk);
		Log("hooked FirstPersonState::Update");
	}

	void OnDriverMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (a_message->type == kTraceMessage && a_message->dataLen == sizeof(float)) {
			trace.Start(*static_cast<const float*>(a_message->data));
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			SKSE::GetMessagingInterface()->RegisterListener("hkyssTestDriver", OnDriverMessage);
			LogCameraMods();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			if (const auto console = RE::ConsoleLog::GetSingleton()) {
				console->Print("hkyss - Head Sway %s", Version().c_str());
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
	OpenLog();
	Log("hkyss - Head Sway {} on Skyrim {}", Version(), a_skse->RuntimeVersion().string());
	Install();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
