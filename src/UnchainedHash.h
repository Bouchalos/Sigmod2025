#pragma once
#include <vector>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <nmmintrin.h>  //for crc32 hash function

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

    std::vector<uint64_t> bloom_bits;   //bloom filter map
    size_t bloom_bit_count = 0;     //total bloom bits

    std::vector<Tuple> raw_tuples;

    uint64_t hash_key(const K& key) const {
        if constexpr (sizeof(K) <= 4) {
            return static_cast<uint64_t>(_mm_crc32_u32(0, static_cast<uint32_t>(key))); //crc32 hash function
        } else {
            uint32_t lo = static_cast<uint32_t>(key & 0xFFFFFFFF);
            uint32_t hi = static_cast<uint32_t>((key >> 32) & 0xFFFFFFFF);
            uint64_t h = _mm_crc32_u32(0, lo);
            h = _mm_crc32_u32(static_cast<uint32_t>(h), hi);
            return h;
        }
    }

    static uint64_t bloom_hash1(uint64_t x) {   //Bit mixing hash function 1
        x ^= x >> 16;
        x ^= x << 3;
        return x;
    }

    static uint64_t bloom_hash2(uint64_t x) {   //Bit mixing hash function 2
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    }

    static uint16_t mini_hash(uint64_t hash){ 
        return static_cast<uint16_t>(hash & 0xFFFFu); //takes the last 16 bits 
    }

    static uint16_t fingerprint_to_mask(uint16_t fp) {
        uint16_t mask = 0;
        mask |= (1u << ((fp      ) & 0xF));
        mask |= (1u << ((fp >>  4) & 0xF));
        mask |= (1u << ((fp >>  8) & 0xF));
        mask |= (1u << ((fp >> 12) & 0xF));
        return mask;
    }

    inline void bloom_set_bit(uint64_t bitpos) {    //function that sets the bit into true
        if (bloom_bit_count == 0) return;
        bitpos %= bloom_bit_count;
        bloom_bits[bitpos >> 6] |= (1ULL << (bitpos & 63));
    }

    inline bool bloom_may_contain(uint64_t key) const {
        if (bloom_bit_count == 0)   
            return true;    //empty bloom filter → always pretend "maybe present"
        uint64_t h1 = bloom_hash1(key) % bloom_bit_count;       //first bloom hash → bit position
        uint64_t h2 = bloom_hash2(key) % bloom_bit_count;        //second bloom hash → bit position
        return (bloom_bits[h1 >> 6] & (1ULL << (h1 & 63))) && (bloom_bits[h2 >> 6] & (1ULL << (h2 & 63)));  //check that both bloom-filter bits for this key are set
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

    void add_tuple(const K& key, const V& value) {  //making tuples ov key and value to build the
        raw_tuples.push_back({key, value});
        uint64_t h = hash_key(key);
        bloom_set_bit(bloom_hash1(h));      //sets the bit 1
        bloom_set_bit(bloom_hash2(h));      //sets the bit 1
    }

    void build() {  //building the table
        size_t N = raw_tuples.size();   //the elements we will insert into the table
        adjacency.resize(N);    //contiguous adjacency array that we will insert the elements

        const size_t dir_n = directory.size();
        std::vector<size_t> counts(dir_n, 0);

        for (const auto& t : raw_tuples) {  //counts how many elements goes to every slot
            uint64_t h = hash_key(t.key);   
            size_t slot = static_cast<size_t>(h & (dir_n - 1));
            counts[slot]++;
        }

        size_t offset = 0;
        for (size_t i = 0; i < dir_n; ++i) {    //Compute the starting offset of each slot in the adjacency array (prefix sums).
            uint64_t start_high = static_cast<uint64_t>(offset) << 16;
            uint64_t low16 = directory[i] & 0xFFFFu;
            directory[i] = start_high | low16;
            offset += counts[i];
        }

        std::vector<size_t> write_pos(dir_n);
        for (size_t i = 0; i < dir_n; ++i) 
            write_pos[i] = directory[i] >> 16;

        for (const auto& t : raw_tuples) {  //inserts the elements into adjancy array and updates the fingerprint slot
            uint64_t h = hash_key(t.key);
            size_t slot = static_cast<size_t>(h & (dir_n - 1));
            size_t pos = write_pos[slot]++;
            adjacency[pos] = t;

            uint16_t fp = mini_hash(h);
            uint16_t mask = fingerprint_to_mask(fp);
            directory[slot] |= mask;
        }
    }

    std::vector<V*> find(const K& key) {
        std::vector<V*> out;
        if (directory.empty()) return out;

        uint64_t h = hash_key(key);
        size_t slot = static_cast<size_t>(h & (directory.size() - 1));
        uint64_t entry = directory[slot];

        if (!bloom_may_contain(h)) return out;  //checks if the element might be into the table

        uint16_t probe_mask = fingerprint_to_mask(mini_hash(h));    //takes the fingerprint
        uint16_t slot_filter = static_cast<uint16_t>(entry & 0xFFFFu);  //takes the fingerprint of the slot
        if ((slot_filter & probe_mask) != probe_mask) return out;   //sees if the fingerprint matches to the slot comparing it's bits

        produceMatches(key, slot, out); 
        return out;
    }

private:
    void produceMatches(const K& key, size_t slot, std::vector<V*>& out) {  //finds the key and returns the values
        size_t dir_n = directory.size();
        uint64_t start = directory[slot] >> 16; //takes the 48 bits (offset) start of the adjency list
        uint64_t end = (slot + 1 == dir_n) ? adjacency.size() : (directory[slot + 1] >> 16);    //end of the bucket
        for (uint64_t i = start; i < end; ++i) {
            Tuple& t = adjacency[i];
            if (t.key == key) out.push_back(&t.value);
        }
    }
};
