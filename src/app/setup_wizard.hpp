#pragma once

#include <string>

namespace nf::app {

struct AppConfig;

enum class SetupResult { Configured, Screenshot, Cancelled };

SetupResult run_setup_wizard(AppConfig& config, const std::string& screenshot);

}  // namespace nf::app
