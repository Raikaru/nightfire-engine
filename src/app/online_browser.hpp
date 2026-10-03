#pragma once

#include <optional>
#include <string>

#include "app/net_client.hpp"
#include "render/window.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf::app {
struct AppContext;

// Returns selected connection settings, or empty when the player cancels/leaves a scripted browser run.
std::optional<NetworkClientOptions> run_online_browser(const AppContext& ctx, Window& window,
                                                        ui::Renderer& renderer, ui::TextRenderer& text,
                                                        const std::string& press, const std::string& shot,
                                                        const std::string& player_name,
                                                        const std::string& master_endpoint);

}  // namespace nf::app
