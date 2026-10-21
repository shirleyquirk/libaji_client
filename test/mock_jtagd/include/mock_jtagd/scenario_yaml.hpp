// Load a Scenario from YAML, and print one back for humans.
// File format: docs/mock-jtagd.md.
#pragma once

#include "mock_jtagd/scenario.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mock_jtagd {

// Message is "origin:line:col: what"; origin is the file name.
struct ScenarioError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Scenario load_scenario(const std::filesystem::path & file);
Scenario parse_scenario(std::string_view yaml, std::string_view origin = "<string>");

// jtagconfig-style tree with node, register and channel details and provenance.
std::string describe(const Scenario & scenario);

} // namespace mock_jtagd
