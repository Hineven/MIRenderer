/*
 * Created: 2026/2/1
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <random>
#include <mutex>
#include "core/util/unordered_hashing.h"
MI_NAMESPACE_BEGIN

static uint32_t g_zobrist_table[256][256];
static std::once_flag g_zobrist_table_init_flag;

static void InitializeZobristTable() {
    std::mt19937 rng(123456); // Fixed seed for reproducibility
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    for (size_t i = 0; i < 256; ++i) {
        for (size_t j = 0; j < 256; ++j) {
            g_zobrist_table[i][j] = dist(rng);
        }
    }
}

uint32_t ZobristMap(const void * data, size_t size) {
    // THread safe
    std::call_once(g_zobrist_table_init_flag, InitializeZobristTable);
    const uint8_t * bytes = static_cast<const uint8_t *>(data);
    uint32_t hash = 0;
    for (size_t i = 0; i < size; ++i) {
        hash ^= g_zobrist_table[i % 256][bytes[i]];
    }
    return hash;
}

uint32_t ZobristSetHashing::Add(const void * data, size_t size)  {
    uint32_t element_hash = ZobristMap(data, size);
    hash_ ^= element_hash;
    return hash_;
}

uint32_t ZobristSetHashing::GetResult() const {
    return hash_;
}

MI_NAMESPACE_END