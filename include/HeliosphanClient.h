#pragma once

#include <HeliosphanServiceAPI.h>

namespace MPL::HeliosphanClient
{
    bool Initialize(API::MMSF::Interface* a_registry);
    API::Heliosphan::IHeliosphanService* Get();
}
