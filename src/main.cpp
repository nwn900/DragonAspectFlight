#include "PCH.h"

#include "DragonAspectFlight/InputHandler.h"
#include "DragonAspectFlight/Papyrus.h"

#include <Windows.h>
#include <fstream>

namespace
{
	std::filesystem::path GetPluginDirectory()
	{
		HMODULE module = nullptr;
		const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
		const auto address = reinterpret_cast<LPCSTR>(&GetPluginDirectory);

		if (!GetModuleHandleExA(flags, address, std::addressof(module)) || !module) {
			return std::filesystem::current_path() / "Data" / "SKSE" / "Plugins";
		}

		char pathBuffer[MAX_PATH]{};
		const auto length = GetModuleFileNameA(module, pathBuffer, static_cast<DWORD>(std::size(pathBuffer)));

		if (length == 0) {
			return std::filesystem::current_path() / "Data" / "SKSE" / "Plugins";
		}

		return std::filesystem::path(pathBuffer).parent_path();
	}

	std::filesystem::path GetLogPath()
	{
		return GetPluginDirectory() / "DragonAspectFlight.log";
	}

	void RawLog(std::string_view a_message)
	{
		const auto logPath = GetLogPath();

		std::error_code error;
		std::filesystem::create_directories(logPath.parent_path(), error);

		std::ofstream file(logPath, std::ios::app);

		if (file.is_open()) {
			file << a_message << '\n';
		}
	}

	void InitializeLogging()
	{
		const auto logPath = GetLogPath();

		RawLog("Raw logger reached InitializeLogging");

		try {
			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
			auto log = std::make_shared<spdlog::logger>("DragonAspectFlight", std::move(sink));

			log->set_level(spdlog::level::trace);
			log->flush_on(spdlog::level::trace);

			spdlog::set_default_logger(std::move(log));
			spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

			logger::info("DragonAspectFlight logger initialized at {}", logPath.string());
		} catch (const std::exception& e) {
			RawLog(std::string("spdlog logger failed: ") + e.what());
		} catch (...) {
			RawLog("spdlog logger failed with unknown exception");
		}
	}

	void InitializePapyrus()
	{
		auto papyrus = SKSE::GetPapyrusInterface();

		if (papyrus) {
			papyrus->Register(DragonAspectFlight::Papyrus::Register);
			logger::info("Papyrus functions registered");
			RawLog("Papyrus functions registered");
		} else {
			logger::error("Failed to get Papyrus interface");
			RawLog("Failed to get Papyrus interface");
		}
	}

	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}

		switch (a_msg->type) {
		case SKSE::MessagingInterface::kInputLoaded:
			DragonAspectFlight::InputHandler::GetSingleton()->Register();
			logger::info("InputLoaded message received");
			RawLog("InputLoaded message received");
			break;

		case SKSE::MessagingInterface::kDataLoaded:
			logger::info("DataLoaded message received");
			RawLog("DataLoaded message received");
			break;

		default:
			break;
		}
	}
}

extern "C" __declspec(dllexport) SKSE::PluginVersionData SKSEPlugin_Version = []()
{
	SKSE::PluginVersionData data{};

	data.PluginName("DragonAspectFlight");
	data.PluginVersion(REL::Version{ 0, 8, 10, 0 });
	data.AuthorName("LvxMagick");
	data.UsesAddressLibrary(true);
	data.UsesStructsPost629(false);
	data.CompatibleVersions({ SKSE::RUNTIME_SSE_LATEST_SE, SKSE::RUNTIME_SSE_LATEST_AE });

	return data;
}();

extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = "DragonAspectFlight";
	a_info->version = 1;
	return true;
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	RawLog("SKSEPlugin_Load entered");

	SKSE::Init(a_skse);

	InitializeLogging();

	logger::info("DragonAspectFlight loading");
	RawLog("DragonAspectFlight loading");

	InitializePapyrus();

	auto messaging = SKSE::GetMessagingInterface();

	if (messaging) {
		messaging->RegisterListener(MessageHandler);
		logger::info("Messaging listener registered");
		RawLog("Messaging listener registered");
	} else {
		logger::error("Failed to get messaging interface");
		RawLog("Failed to get messaging interface");
	}

	logger::info("DragonAspectFlight loaded");
	RawLog("DragonAspectFlight loaded");

	return true;
}
