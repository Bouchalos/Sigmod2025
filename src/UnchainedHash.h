#pragma once
#include <vector>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <nmmintrin.h>  
#include <atomic>

template<typename K, typename V>
class UnchainedHashTable {
public:
    struct Tuple {
        K key;
        V value;
    };

private:
    std::vector<uint64_t> directory;  
    std::vector<Tuple> adjacency;

    std::vector<uint64_t> bloom_bits;   
    size_t bloom_bit_count = 0;     

public:
    uint64_t hash_key(const K& key) const {
        if constexpr (sizeof(K) <= 4) {
            return static_cast<uint64_t>(_mm_crc32_u32(0, static_cast<uint32_t>(key))); 
        } else {
            uint32_t lo = static_cast<uint32_t>(key & 0xFFFFFFFF);
            uint32_t hi = static_cast<uint32_t>((key >> 32) & 0xFFFFFFFF);
            uint64_t h = _mm_crc32_u32(0, lo);
            h = _mm_crc32_u32(static_cast<uint32_t>(h), hi);
            return h;
        }
    }

private:
    static uint64_t bloom_hash1(uint64_t x) {   
        x ^= x >> 16;
        x ^= x << 3;
        return x;
    }

    static uint64_t bloom_hash2(uint64_t x) {   
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    }

    static uint16_t mini_hash(uint64_t hash){ 
        return static_cast<uint16_t>(hash & 0xFFFFu); 
    }

    static uint16_t fingerprint_to_mask(uint16_t fp) {
        uint16_t mask = 0;
        mask |= (1u << ((fp      ) & 0xF));
        mask |= (1u << ((fp >>  4) & 0xF));
        mask |= (1u << ((fp >>  8) & 0xF));
        mask |= (1u << ((fp >> 12) & 0xF));
        return mask;
    }

    inline void bloom_set_bit(uint64_t bitpos) {    
        if (bloom_bit_count == 0) return;
        bitpos %= bloom_bit_count;
        __atomic_fetch_or(&bloom_bits[bitpos >> 6], (1ULL << (bitpos & 63)), __ATOMIC_RELAXED);
    }

    inline bool bloom_may_contain(uint64_t key) const {
        if (bloom_bit_count == 0)   
            return true;    
        uint64_t h1 = bloom_hash1(key) % bloom_bit_count;       
        uint64_t h2 = bloom_hash2(key) % bloom_bit_count;        
        return (bloom_bits[h1 >> 6] & (1ULL << (h1 & 63))) && (bloom_bits[h2 >> 6] & (1ULL << (h2 & 63)));  
    }

    static size_t next_power_of_two(size_t n) {
        if (n == 0) return 1;
        n--;
        n |= n >> 1; n |= n >> 2; n |= n >> 4;
        n |= n >> 8; n |= n >> 16;
        if constexpr(sizeof(size_t) == 8) n |= n >> 32;
        return n + 1;
    }

public:
    explicit UnchainedHashTable(size_t expected_tuples = 0) {
        size_t dir_size = next_power_of_two(expected_tuples > 0 ? expected_tuples*2  : 4);
        directory.resize(dir_size, 0);
        bloom_bit_count = next_power_of_two(expected_tuples > 0 ? expected_tuples * 4 : 64);
        bloom_bits.resize((bloom_bit_count + 63) / 64, 0);
    }

    // Helpers for Parallel Build
    void resize_adjacency(size_t size) {
        adjacency.resize(size);
    }

    void set_directory_entry(size_t slot, uint64_t value) {
        directory[slot] = value;
    }

    size_t get_directory_size() const {
        return directory.size();
    }
    
    void insert_parallel(size_t slot, size_t offset, const K& key, const V& value) {
        adjacency[offset] = {key, value};
        
        uint64_t h = hash_key(key);
        uint16_t fp = mini_hash(h);
        uint16_t mask = fingerprint_to_mask(fp);
        
        __atomic_fetch_or(&directory[slot], (uint64_t)mask, __ATOMIC_RELAXED);
        bloom_set_bit(bloom_hash1(h));
        bloom_set_bit(bloom_hash2(h));
    }

    std::vector<V*> find(const K& key) {
        std::vector<V*> out;
        if (directory.empty()) return out;

        uint64_t h = hash_key(key);
        size_t slot = static_cast<size_t>(h & (directory.size() - 1));
        uint64_t entry = directory[slot];

        if (!bloom_may_contain(h)) return out;  

        uint16_t probe_mask = fingerprint_to_mask(mini_hash(h));    
        uint16_t slot_filter = static_cast<uint16_t>(entry & 0xFFFFu);  
        if ((slot_filter & probe_mask) != probe_mask) return out;   

        produceMatches(key, slot, out); 
        return out;
    }

private:
    void produceMatches(const K& key, size_t slot, std::vector<V*>& out) {  
        size_t dir_n = directory.size();
        uint64_t start = directory[slot] >> 16; 
        uint64_t end = (slot + 1 == dir_n) ? adjacency.size() : (directory[slot + 1] >> 16);    
        for (uint64_t i = start; i < end; ++i) {
            Tuple& t = adjacency[i];
            if (t.key == key) out.push_back(&t.value);
        }
    }
};