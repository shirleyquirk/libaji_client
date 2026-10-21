// Scenario YAML loader: what it accepts, and that its errors point at the problem.
#include "mock_jtagd/scenario_yaml.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

using namespace mock_jtagd;

std::string error_of(std::string_view yaml)
{
    try {
        parse_scenario(yaml, "board.yaml");
    } catch (const ScenarioError & e) {
        return e.what();
    }
    return "no error";
}

TEST(Scenario, DefaultBoardYamlMatchesBuiltIn)
{
    const Scenario file = load_scenario(std::filesystem::path(MOCK_JTAGD_SCENARIO_DIR) / "default_board.yaml");
    const Scenario built = Scenario::default_board();
    EXPECT_EQ(describe(built), describe(file));
}

TEST(Scenario, NumbersAndShorthands)
{
    const Scenario s = parse_scenario(R"(
cables:
  - chain:
      - idcode: 0x0123_4567
        irlen: 10
        features: [bad_ir_capture]
        registers:
          0b0001010101: rw
          0x100: { kind: sequence, length: 2, values: [0, 1, 3] }
        hub:
          nodes:
            - idcode: { node: 0x08 }
)");
    const Tap & t = s.cables.at(0).taps.at(0);
    EXPECT_EQ(0x01234567u, t.idcode);
    EXPECT_EQ(devfeat_bad_ir_capture | devfeat_possible_hub, t.features) << "a hub implies possible_hub";
    EXPECT_EQ(RegisterSpec::Kind::rw, t.registers.at(0x55).kind);
    EXPECT_EQ((std::vector<std::uint64_t>{0, 1, 3}), t.registers.at(0x100).values);
    EXPECT_EQ(sld_node_idcode(1, 0x08, mfg_altera, 0), t.hub->nodes.at(0).idcode);
    EXPECT_EQ("node0", t.hub->nodes.at(0).name);
}

TEST(Scenario, UnknownKeyIsAnErrorWithPosition)
{
    EXPECT_EQ("board.yaml:4:9: unknown key 'irlength' in chain entry (expected one of: name, idcode, irlen, "
              "idcode_ir, ir_capture, features, registers, hub, provenance)",
              error_of("cables:\n  - chain:\n      - idcode: 1\n        irlength: 10\n"));
}

TEST(Scenario, ValueMustFitRegister)
{
    EXPECT_EQ("board.yaml:6:50: value: 0x1FF is out of range (max 0xFF)",
              error_of(R"(cables:
  - chain:
      - idcode: 1
        irlen: 10
        registers:
          0x10: { kind: const, length: 8, value: 0x1FF }
)"));
}

TEST(Scenario, CannotRedefineIdcodeOrBypass)
{
    EXPECT_NE(std::string::npos, error_of(R"(cables:
  - chain:
      - idcode: 1
        irlen: 4
        idcode_ir: 0x6
        registers: { 0xF: rw }
)").find("IDCODE/BYPASS"));
}

TEST(Scenario, ChannelIrsMustBeDistinct)
{
    EXPECT_NE(std::string::npos, error_of(R"(cables:
  - chain:
      - idcode: 1
        irlen: 10
        hub:
          nodes:
            - idcode: 0x08000600
              registers: { 1: rw }
              channels:
                - { command_ir: 1, response_ir: 2 }
)").find("also declared as a register"));
}

TEST(Scenario, NestedHubsAreRejectedForNow)
{
    EXPECT_NE(std::string::npos, error_of(R"(cables:
  - chain:
      - idcode: 1
        irlen: 10
        hub:
          nodes:
            - idcode: 0x07F86E00
              hub: { nodes: [] }
)").find("nested hubs"));
}

TEST(Scenario, BadProvenance)
{
    EXPECT_NE(std::string::npos,
              error_of("cables:\n  - chain:\n      - { idcode: 1, irlen: 10, provenance: guessed }\n").find("provenance"));
}

TEST(Scenario, DescribeShowsProvenance)
{
    const Scenario s = load_scenario(std::filesystem::path(MOCK_JTAGD_SCENARIO_DIR) / "sdm_hello.yaml");
    const std::string text = describe(s);
    EXPECT_NE(std::string::npos, text.find("Node 0BD06E00  sdm")) << text;
    EXPECT_NE(std::string::npos,
              text.find("channel packets: command IR 0x1, response IR 0x2, 32-bit words, 1 transcript entries  "
                        "[placeholder]"))
        << text;
}

} // namespace
