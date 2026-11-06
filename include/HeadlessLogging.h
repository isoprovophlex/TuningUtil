#pragma once

#include <HeliosphanServiceAPI.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <cassert>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace MPL::HeadlessLogging
{
    class HeliosphanSink final : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        HeliosphanSink(API::Heliosphan::LogMessage a_write, std::filesystem::path a_fallbackPath) :
            write(a_write), fallbackPath(std::move(a_fallbackPath))
        {
            assert(write);
        }

    protected:
        void sink_it_(const spdlog::details::log_msg& a_message) override
        {
            const std::string_view payload(a_message.payload.data(), a_message.payload.size());
            const auto text = payload.starts_with("[HeliosTuning]") ?
                                  std::string(payload) : "[HeliosTuning] " + std::string(payload);
            if (write(static_cast<API::Heliosphan::LogLevel>(a_message.level), text.data(), text.size()))
            {
                return;
            }
            if (!fallback)
            {
                fallback = std::make_shared<spdlog::sinks::basic_file_sink_mt>(fallbackPath.string(), true);
                fallback->set_formatter(formatter_->clone());
                const spdlog::details::log_msg warning{
                    a_message.logger_name, spdlog::level::warn,
                    "[HeliosTuning] Heliosphan logging failed; using HeliosTuning.log"
                };
                fallback->log(warning);
            }
            fallback->log(a_message);
        }

        void flush_() override
        {
            if (fallback) fallback->flush();
        }

    private:
        API::Heliosphan::LogMessage write;
        std::filesystem::path fallbackPath;
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> fallback;
    };
}
