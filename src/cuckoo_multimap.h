#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>  

namespace cuckoo {

template <class K, class V = std::size_t>
class CuckooMultiMap {
public:
    //start with a power of two capacity (at least 8)
    explicit CuckooMultiMap(std::size_t expected = 0) {
        const std::size_t cap = next_pow2(expected > 0 ? expected * 2 : 8);
        init_capacity(cap);
    }

    //pre-grow if needed, rebuild into a larger capacity
    void reserve(std::size_t expected) {
        std::size_t want = next_pow2(expected > 0 ? expected * 2 : 8);
        if (want <= capacity_) return;  //already big enough
        auto elems = collect_all(); //gather all buckets
        rebuild_internal(want, elems); // offline rebuild and commit
    }

    //insert (key, value)
    //  If key already exists: append value to its vector
    //  Else: try cuckoo insert. If it loops too long, rehash/expand
    void insert(const K& key, const V& value) {
        //fast path: key already present in one of its two homes?
        {
            std::size_t i1 = h1(key);
            if (table1_[i1].occupied && table1_[i1].key == key) {
                table1_[i1].vals.push_back(value);
                return;
            }
            std::size_t i2 = h2(key);
            if (table2_[i2].occupied && table2_[i2].key == key) {
                table2_[i2].vals.push_back(value);
                return;
            }
        }

        //new distinct key = keep load factor low before inserting
        ensure_capacity_for_insert();

        //after a potential rebuild, check again...
        {
            std::size_t i1 = h1(key);
            if (table1_[i1].occupied && table1_[i1].key == key) {
                table1_[i1].vals.push_back(value);
                return;
            }
            std::size_t i2 = h2(key);
            if (table2_[i2].occupied && table2_[i2].key == key) {
                table2_[i2].vals.push_back(value);
                return;
            }
        }

        //prepare a fresh bucket with the first value
        Bucket fresh;
        fresh.key       = key;
        fresh.vals      = { value };
        fresh.occupied  = true;

        //try with no kicking
        {
            std::size_t i1 = h1(key);
            if (!table1_[i1].occupied) {
                table1_[i1] = std::move(fresh);
                ++size_; //new distinct key
                return;
            }
            std::size_t i2 = h2(key);
            if (!table2_[i2].occupied) {
                table2_[i2] = std::move(fresh);
                ++size_;
                return;
            }
        }

        //do real cuckoo insertion (kick-out loop)
        Bucket cur = std::move(fresh);
        if (cuckoo_kick_insert(cur)) {
            ++size_;
            return;
        }

        //still no luck = grow and rebuild including this leftover bucket
        rehash_with_extra(cur);
        //rebuild recomputes size_, so no ++size_ here
    }

    //call fn(value) for every value stored under "key"
    template <class Fn>
    void for_each(const K& key, Fn&& fn) const {
        std::size_t i1 = h1(key);
        if (table1_[i1].occupied && table1_[i1].key == key) {
            for (const V& v : table1_[i1].vals) fn(v);
            return;
        }
        std::size_t i2 = h2(key);
        if (table2_[i2].occupied && table2_[i2].key == key) {
            for (const V& v : table2_[i2].vals) fn(v);
            return;
        }
        //if not found no calls
    }

    std::size_t size()     const { return size_;     } //number of distinct keys (not total values)
    std::size_t capacity() const { return capacity_; } //slots per table

private:
    
    //one hash bucket: key + all its values and occupied flag
    struct Bucket {
        K                key;
        std::vector<V>   vals;
        bool             occupied = false;
    };

    //2 cuckoo tables.
    std::vector<Bucket> table1_;
    std::vector<Bucket> table2_;

    std::size_t         capacity_ = 0;   //capacity(power of two)
    std::size_t         mask_     = 0;   //mask
    std::size_t         size_     = 0;   //different keys i have

    //helpers

    //next power of two (min 8)
    static std::size_t next_pow2(std::size_t x) {
        if (x < 8) x = 8;
        --x;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
        if constexpr (sizeof(std::size_t) >= 8) {
            x |= x >> 32;
        }
        ++x;
        return x;
    }

    //simple 64-bit mix to spread std::hash results better
    static inline std::uint64_t mix64_u(std::uint64_t x) {        
        x ^= x >> 33u;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33u;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33u;
        return x;
    }

    //static hashes used during offline rebuilds
    static std::size_t h1_static(const K& k, std::size_t mask) {
        std::uint64_t h = (std::uint64_t)std::hash<K>{}(k);
        return (std::size_t)(mix64_u(h)) & mask;
    }

    static std::size_t h2_static(const K& k, std::size_t mask) {
        std::uint64_t h = ((std::uint64_t)std::hash<K>{}(k)) ^ 0x9e3779b97f4a7c15ULL;
        return (std::size_t)(mix64_u(h)) & mask;
    }
    //instance hashes with current mask
    std::size_t h1(const K& k) const { return h1_static(k, mask_); }
    std::size_t h2(const K& k) const { return h2_static(k, mask_); }


    
    //capacity management

    //set up tables for given capacity and clear size
    void init_capacity(std::size_t cap) {
        capacity_ = next_pow2(cap);
        mask_     = capacity_ - 1;
        table1_.assign(capacity_, Bucket{});
        table2_.assign(capacity_, Bucket{});
        size_     = 0;
    }

    //before inserting a new key keep load factor = 0.5 (per table)
    void ensure_capacity_for_insert() {
        // If (size_ + 1) * 2 > capacity_  = grow
        if ((size_ + 1) * 2 <= capacity_) return;

        auto elems = collect_all();
        rebuild_internal(capacity_ * 2, elems);
    }

    //collect all currently occupied buckets from both tables
    std::vector<Bucket> collect_all() const {
        std::vector<Bucket> elems;
        elems.reserve(size_);
        for (const auto& b : table1_) {
            if (b.occupied) elems.push_back(b);
        }
        for (const auto& b : table2_) {
            if (b.occupied) elems.push_back(b);
        }
        return elems;
    }

   
    //live cuckoo insert with kick-outs
    
    bool cuckoo_kick_insert(Bucket& cur) {
        constexpr std::size_t MAX_KICKS = 256; //emergency brake

        bool in_first = true;
        std::size_t idx = h1(cur.key);

        for (std::size_t kick = 0; kick < MAX_KICKS; ++kick) {
            if (in_first) {
                //empty? take it
                if (!table1_[idx].occupied) {
                    table1_[idx] = std::move(cur);
                    table1_[idx].occupied = true;
                    return true;
                }
                //same key? merge values
                if (table1_[idx].key == cur.key) {
                    auto& dest = table1_[idx].vals;
                    dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                    return true;
                }

                //kick resident and move to other table
                std::swap(cur, table1_[idx]);
                in_first = false;
                idx      = h2(cur.key);

                //quick try on table2
                if (!table2_[idx].occupied) {
                    table2_[idx] = std::move(cur);
                    table2_[idx].occupied = true;
                    return true;
                }
                if (table2_[idx].occupied && table2_[idx].key == cur.key) {
                    auto& dest = table2_[idx].vals;
                    dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                    return true;
                }

            } else {
               
                if (!table2_[idx].occupied) {
                    table2_[idx] = std::move(cur);
                    table2_[idx].occupied = true;
                    return true;
                }
                if (table2_[idx].key == cur.key) {
                    auto& dest = table2_[idx].vals;
                    dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                    return true;
                }

               
                std::swap(cur, table2_[idx]);
                in_first = true;
                idx      = h1(cur.key);

                //quick try on table1
                if (!table1_[idx].occupied) {
                    table1_[idx] = std::move(cur);
                    table1_[idx].occupied = true;
                    return true;
                }
                if (table1_[idx].occupied && table1_[idx].key == cur.key) {
                    auto& dest = table1_[idx].vals;
                    dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                    return true;
                }
            }
        }

        //too many kicks = likely a cycle, rehash needed
        return false;
    }

   
    // Rehash / Expand
   

    //rebuild including an extra bucket that couldn't be placed
    void rehash_with_extra(const Bucket& extra) {
        auto elems = collect_all();
        elems.push_back(extra);
        rebuild_internal(capacity_ * 2, elems);
    }

    // Offline build: try to place everything into new tables of size start_cap
    // If it fails (too many kicks) = double capacity and retry
    void rebuild_internal(std::size_t start_cap, std::vector<Bucket>& elems) {
        std::size_t cap = next_pow2(start_cap);
        if (cap < 8) cap = 8;

        for (;;) {
            std::vector<Bucket> new_t1(cap);
            std::vector<Bucket> new_t2(cap);
            const std::size_t   new_mask = cap - 1;

            if (try_build_tables(elems, new_t1, new_t2, new_mask)) {
                // commit the new tables
                table1_.swap(new_t1);
                table2_.swap(new_t2);
                capacity_ = cap;
                mask_     = new_mask;

                //recount distinct keys
                size_ = 0;
                for (const auto& b : table1_) if (b.occupied) ++size_;
                for (const auto& b : table2_) if (b.occupied) ++size_;
                return;
            }

            cap *= 2; //still stuck? grow and try again
        }
    }

    //try to build tables offline with cuckoo insert
    //return false if any bucket gets stuck (too many kicks) = caller will grow
    static bool try_build_tables(
        const std::vector<Bucket>& elems,
        std::vector<Bucket>&       T1,
        std::vector<Bucket>&       T2,
        std::size_t                mask
    ) {
        constexpr std::size_t MAX_KICKS = 512;

        for (const Bucket& b0 : elems) {
            Bucket cur = b0; //local copy to kick around

            //first try: two homes without kicks (merge if same key)
            std::size_t i1 = h1_static(cur.key, mask);
            if (!T1[i1].occupied) {
                T1[i1] = std::move(cur);
                T1[i1].occupied = true;
                continue;
            }
            if (T1[i1].key == cur.key) {
                //same key = append values
                auto& dest = T1[i1].vals;
                dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                continue;
            }

            std::size_t i2 = h2_static(cur.key, mask);
            if (!T2[i2].occupied) {
                T2[i2] = std::move(cur);
                T2[i2].occupied = true;
                continue;
            }
            if (T2[i2].key == cur.key) {
                auto& dest = T2[i2].vals;
                dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                continue;
            }

            //offline cuckoo loop (same idea as live insert)
            bool in_first = true;
            std::size_t idx = i1;

            std::size_t kick;
            for (kick = 0; kick < MAX_KICKS; ++kick) {
                if (in_first) {
                    if (!T1[idx].occupied) {
                        T1[idx] = std::move(cur);
                        T1[idx].occupied = true;
                        break;
                    }
                    if (T1[idx].key == cur.key) {
                        auto& dest = T1[idx].vals;
                        dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                        break;
                    }

             
                    std::swap(cur, T1[idx]);
                    in_first = false;
                    idx      = h2_static(cur.key, mask);

                   
                    if (!T2[idx].occupied) {
                        T2[idx] = std::move(cur);
                        T2[idx].occupied = true;
                        break;
                    }
                    if (T2[idx].key == cur.key) {
                        auto& dest = T2[idx].vals;
                        dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                        break;
                    }

                } else {
                   
                    if (!T2[idx].occupied) {
                        T2[idx] = std::move(cur);
                        T2[idx].occupied = true;
                        break;
                    }
                    if (T2[idx].key == cur.key) {
                        auto& dest = T2[idx].vals;
                        dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                        break;
                    }

                
                    std::swap(cur, T2[idx]);
                    in_first = true;
                    idx      = h1_static(cur.key, mask);

                 
                    if (!T1[idx].occupied) {
                        T1[idx] = std::move(cur);
                        T1[idx].occupied = true;
                        break;
                    }
                    if (T1[idx].key == cur.key) {
                        auto& dest = T1[idx].vals;
                        dest.insert(dest.end(), cur.vals.begin(), cur.vals.end());
                        break;
                    }
                }
            }

            if (kick == MAX_KICKS) {
                //stuck with this capacity = let caller grow and retry
                return false;
            }
        }

        return true;
    }
};

} // namespace cuckoo
