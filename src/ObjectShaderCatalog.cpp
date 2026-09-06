#include <ObjectShaderCatalog.h>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>

namespace MPL::ObjectShaderCatalog
{
    Capability Get(const std::string_view a_modelPath)
    {
        if (a_modelPath.empty()) return Capability::none;
        auto path = std::string(a_modelPath);
        std::ranges::replace(path, '/', '\\');
        std::ranges::transform(path, path.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!path.starts_with("meshes\\")) path.insert(0, "meshes\\");

        static std::mutex lock;
        static std::unordered_map<std::string, Capability> cache;
        const std::scoped_lock guard(lock);
        if (const auto found = cache.find(path); found != cache.end()) return found->second;

        RE::NiPointer<RE::NiNode> root;
        const RE::BSModelDB::DBTraits::ArgsType arguments;
        auto capabilities = Capability::none;
        if (RE::BSModelDB::Demand(path.c_str(), root, arguments) == RE::BSResource::ErrorCode::kNone && root)
        {
            RE::BSVisit::TraverseScenegraphGeometries(root.get(), [&](RE::BSGeometry* geometry)
            {
                auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
                if (!property || !property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance))
                    return RE::BSVisit::BSVisitControl::kContinue;
                if (netimmerse_cast<RE::BSLightingShaderProperty*>(property)) capabilities = capabilities | Capability::lighting;
                else if (netimmerse_cast<RE::BSEffectShaderProperty*>(property)) capabilities = capabilities | Capability::effect;
                return Has(capabilities, Capability::lighting | Capability::effect) ?
                    RE::BSVisit::BSVisitControl::kStop : RE::BSVisit::BSVisitControl::kContinue;
            });
        }
        cache.emplace(std::move(path), capabilities);
        return capabilities;
    }
}
