#include "PCH.h"

#include "DragonAspectFlight/DragonAspectMonitor.h"
#include "DragonAspectFlight/InputHandler.h"
#include "DragonAspectFlight/Papyrus.h"
#include "DragonAspectFlight/Settings.h"
#include "DragonAspectFlight/UI.h"
#include "DragonAspectFlight/Version.h"

namespace
{
	void InitializeLogging()
	{
		auto logDirectory = SKSE::log::log_directory();
		if (!logDirectory) {
			SKSE::stl::report_and_fail("Unable to resolve the SKSE log directory");
		}

		*logDirectory /= "DragonAspectFlight.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logDirectory->string(), true);
		auto log = std::make_shared<spdlog::logger>("DragonAspectFlight", std::move(sink));

		log->set_level(spdlog::level::trace);
		log->flush_on(spdlog::level::trace);

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

		logger::info("DragonAspectFlight logger initialized at {}", logDirectory->string());
	}

	void InitializePapyrus()
	{
		auto papyrus = SKSE::GetPapyrusInterface();

		if (papyrus) {
			papyrus->Register(DragonAspectFlight::Papyrus::Register);
			logger::info("Papyrus functions registered");
		} else {
			logger::error("Failed to get Papyrus interface");
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
			break;

	case SKSE::MessagingInterface::kDataLoaded:
		logger::info("DataLoaded message received");
		DragonAspectFlight::Settings::GetSingleton().Load();
		DragonAspectFlight::UI::Register();
		DragonAspectFlight::DragonAspectMonitor::GetSingleton().Start();
		logger::info(
			"event=dependency_snapshot oar_loaded={} bdi_loaded={} payload_interpreter_loaded={} "
			"stanzas_note=actual_animation_winner_is_reported_by_OpenAnimationReplacer.log",
			GetModuleHandleW(L"OpenAnimationReplacer.dll") != nullptr,
			GetModuleHandleW(L"BehaviorDataInjector.dll") != nullptr,
			GetModuleHandleW(L"PayloadInterpreter.dll") != nullptr);
		logger::info("Dragon Aspect Flight: settings loaded, UI registered, DA monitor started");
		break;

		default:
			break;
		}
	}
}

SKSEPluginInfo(
	.Version = REL::Version{
		DragonAspectFlight::VersionMajor,
		DragonAspectFlight::VersionMinor,
		DragonAspectFlight::VersionPatch,
		DragonAspectFlight::VersionTweak },
	.Name = "DragonAspectFlight",
	.Author = "LvxMagick",
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
	.MinimumSKSEVersion = REL::Version{ 0, 0, 0, 0 }
)

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	InitializeLogging();
	SKSE::Init(a_skse, false);

	const char* runtimeFamily = REL::Module::IsVR() ? "VR" : (REL::Module::IsAE() ? "AE" : "SE");
	logger::info(
		"event=plugin_load version={} build_label={} runtime_family={} runtime_version={} skse_release_index={}",
		DragonAspectFlight::Version,
		DragonAspectFlight::BuildVersion,
		runtimeFamily,
		a_skse->RuntimeVersion().string("."),
		a_skse->GetReleaseIndex());

	InitializePapyrus();

	auto messaging = SKSE::GetMessagingInterface();

	if (messaging) {
		messaging->RegisterListener(MessageHandler);
		logger::info("Messaging listener registered");
	} else {
		logger::error("Failed to get messaging interface");
	}

	logger::info("DragonAspectFlight loaded");

	return true;
}
