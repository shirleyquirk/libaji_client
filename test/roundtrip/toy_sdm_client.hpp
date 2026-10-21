// Stand-in for "your code": a minimal packet-service client written against
// the plain AJI API, knowing nothing about the mock. It finds the SDM node by
// its IDCODE fields, opens it, and runs command/response exchanges through
// the node's virtual IR.
//
// The node id, IRs and header layout are the same placeholders as
// scenarios/sdm_hello.yaml; real values come from captured traffic.
#pragma once

#include "aji.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace toy {

constexpr DWORD sdm_node_id = 0x7A;
constexpr DWORD sdm_mfg = 0x6E;
constexpr DWORD command_ir = 0x1;
constexpr DWORD response_ir = 0x2;
constexpr DWORD get_idcode = 0x10;

struct Response {
    DWORD error = 0;
    std::vector<DWORD> data;
};

class SdmClient {
public:
    // Look through every chain and TAP for a hub node with the SDM node id.
    AJI_ERROR find()
    {
        DWORD hw_n = 8;
        AJI_HARDWARE hw[8];
        if (AJI_ERROR e = aji_get_hardware(&hw_n, hw, 5000); e != AJI_NO_ERROR)
            return e;
        for (DWORD h = 0; h < hw_n && h < 8; ++h) {
            AJI_CHAIN_ID chain = hw[h].chain_id;
            if (aji_lock_chain(chain, 1000) != AJI_NO_ERROR)
                continue;
            DWORD dev_n = 16;
            AJI_DEVICE devs[16];
            if (aji_read_device_chain(chain, &dev_n, devs, true) == AJI_NO_ERROR) {
                for (DWORD tap = 0; tap < dev_n; ++tap) {
                    DWORD idcodes[64];
                    DWORD n = 64;
                    if (aji_get_nodes(chain, tap, idcodes, &n) != AJI_NO_ERROR)
                        continue;
                    for (DWORD i = 0; i < n; ++i)
                        if (((idcodes[i] >> 19) & 0xFF) == sdm_node_id && ((idcodes[i] >> 8) & 0x7FF) == sdm_mfg) {
                            chain_ = chain;
                            tap_ = tap;
                            node_idcode_ = idcodes[i];
                            return AJI_NO_ERROR; // keep the chain locked for open()
                        }
                }
            }
            aji_unlock_chain(chain);
        }
        return AJI_NO_DEVICES;
    }

    AJI_ERROR open()
    {
        const AJI_CLAIM claims[] = {{AJI_CLAIM_OVERLAY, command_ir}, {AJI_CLAIM_OVERLAY, response_ir}};
        if (AJI_ERROR e = aji_open_node(chain_, tap_, node_idcode_, &node_, claims, 2, "toy-sdm"); e != AJI_NO_ERROR)
            return e;
        return aji_unlock_chain_lock(chain_, node_, AJI_PACK_NEVER);
    }

    // Header: [23:12] argument count, [10:0] command code.
    AJI_ERROR call(DWORD command, const std::vector<DWORD> & args, Response & out)
    {
        std::vector<DWORD> words{(static_cast<DWORD>(args.size()) << 12) | command};
        words.insert(words.end(), args.begin(), args.end());
        if (AJI_ERROR e = write_words(words); e != AJI_NO_ERROR)
            return e;

        std::vector<DWORD> header(1);
        if (AJI_ERROR e = read_words(header); e != AJI_NO_ERROR)
            return e;
        out.error = header[0] & 0x7FF;
        out.data.assign((header[0] >> 12) & 0xFFF, 0);
        return out.data.empty() ? AJI_NO_ERROR : read_words(out.data);
    }

    AJI_ERROR read_idle(DWORD & word)
    {
        std::vector<DWORD> one(1);
        AJI_ERROR e = read_words(one);
        word = one[0];
        return e;
    }

    DWORD node_idcode() const { return node_idcode_; }
    AJI_CHAIN_ID chain() const { return chain_; }

private:
    AJI_ERROR write_words(const std::vector<DWORD> & words)
    {
        std::vector<BYTE> bytes;
        for (DWORD w : words)
            for (int i = 0; i < 4; ++i)
                bytes.push_back(static_cast<BYTE>(w >> (8 * i)));
        const DWORD bits = static_cast<DWORD>(words.size() * 32);
        if (AJI_ERROR e = aji_access_overlay(node_, command_ir, nullptr); e != AJI_NO_ERROR)
            return e;
        return aji_access_dr(node_, bits, 0, 0, bits, bytes.data(), 0, 0, nullptr);
    }

    AJI_ERROR read_words(std::vector<DWORD> & words)
    {
        const DWORD bits = static_cast<DWORD>(words.size() * 32);
        std::vector<BYTE> bytes(words.size() * 4);
        if (AJI_ERROR e = aji_access_overlay(node_, response_ir, nullptr); e != AJI_NO_ERROR)
            return e;
        if (AJI_ERROR e = aji_access_dr(node_, bits, 0, 0, 0, nullptr, 0, bits, bytes.data()); e != AJI_NO_ERROR)
            return e;
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = DWORD{bytes[4 * i]} | DWORD{bytes[4 * i + 1]} << 8 | DWORD{bytes[4 * i + 2]} << 16 |
                       DWORD{bytes[4 * i + 3]} << 24;
        return AJI_NO_ERROR;
    }

    AJI_CHAIN_ID chain_ = nullptr;
    DWORD tap_ = 0;
    DWORD node_idcode_ = 0;
    AJI_OPEN_ID node_ = nullptr;
};

} // namespace toy
