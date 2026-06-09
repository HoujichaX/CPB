#pragma once
#include <vector>
#include <cstdint>

std::vector<uint8_t> rs_encode_blockwise(
    const std::vector<uint8_t>& data,
    size_t block = 223,
    size_t parity = 32
);

std::vector<uint8_t> rs_decode_blockwise(
    const std::vector<uint8_t>& data,
    size_t block = 223,
    size_t parity = 32
);