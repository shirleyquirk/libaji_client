#include "mock_jtagd/scenario.hpp"

namespace mock_jtagd {

Scenario Scenario::default_board()
{
    // Shaped like a Cyclone V SoC dev kit: HPS DAP nearest TDO, FPGA behind it.
    // Node IDCODEs are illustrative, not taken from real IP.
    Tap hps;
    hps.idcode = 0x4BA00477;
    hps.ir_length = 4;
    hps.name = "SOCVHPS";
    hps.idcode_instruction = 0xE;

    Tap fpga;
    fpga.idcode = 0x02D120DD;
    fpga.ir_length = 10;
    fpga.name = "5CSEMA5";
    fpga.features = devfeat_possible_hub;
    fpga.idcode_instruction = 0x006;
    fpga.hub = SldHub{
        .ir_bits = 8,
        .nodes = {sld_node_idcode(6, 0x00, mfg_altera, 0), sld_node_idcode(1, 0x08, mfg_altera, 1)},
    };

    Cable cable;
    cable.hw_name = "USB-Blaster";
    cable.port = "USB-1";
    cable.taps = {hps, fpga};

    Scenario s;
    s.cables = {cable};
    return s;
}

} // namespace mock_jtagd
