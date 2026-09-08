#include <HeliosphanClient.h>

#include <atomic>

namespace MPL::HeliosphanClient
{
    namespace
    {
        std::atomic<API::Heliosphan::IHeliosphanService*> service{ nullptr };
    }

    bool Initialize(API::MMSF::Interface* a_registry)
    {
        if (Get()) return true;

        API::MMSF::IPluginService* candidate = nullptr;
        const char* failure = nullptr;
        if (!a_registry)
        {
            failure = "MMSF-unavailable";
        }
        else
        {
            const auto features = a_registry->GetVersion();
            if (API::MMSF::GetVersion(features) != 2)
            {
                failure = "MMSF-version-mismatch";
            }
            else if ((features & API::MMSF::MMSFAPIFeatures::kCoreService) == API::MMSF::MMSFAPIFeatures{})
            {
                failure = "service-registry-unavailable";
            }
            else
            {
                candidate = a_registry->QueryService(API::Heliosphan::kServiceName);
                if (!candidate) failure = "service-unavailable";
                else if (candidate->GetVersion() != API::Heliosphan::kVersion) failure = "service-version-mismatch";
            }
        }
        if (!failure)
        {
            service.store(static_cast<API::Heliosphan::IHeliosphanService*>(candidate), std::memory_order_release);
        }
        logger::info(
            "[Heliosphan Connection] method=MMSF | service=HELIOSPHAN | phase=DataLoaded | required={} | reported={} | accepted={} | reason={}",
            API::Heliosphan::kVersion,
            candidate ? std::to_string(candidate->GetVersion()) : "<unavailable>",
            !failure,
            failure ? failure : "accepted");
        return !failure;
    }

    API::Heliosphan::IHeliosphanService* Get()
    {
        return service.load(std::memory_order_acquire);
    }
}
