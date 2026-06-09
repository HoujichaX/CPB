#pragma once
#include "container.hpp"
#include <vector>

std::vector<Byte> run_vm(
    const std::vector<Byte>& code,
    const std::vector<Blob>& blobs
);