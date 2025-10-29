#pragma once
#include <vector>
#include <optional>
#include <bitset>
#include <functional>
#include <utility>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <iostream>

using namespace std;

inline bool is_prime(size_t n) {        //function to see if a num is prime 
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    for (size_t i = 3; i * i <= n; i += 2)
        if (n % i == 0) return false;
    return true;
}

inline size_t next_prime(size_t n) {       //function to find the next prime number
    while (!is_prime(n)) ++n;
    return n;
}

template <typename Key, typename Value>
class RobinHoodHashMap {
private:
    struct Bucket{
        optional<pair<Key,Value>> kv;
        size_t distance;    //the distance of it's origin bucket
    };

    vector<Bucket> table;   //a table of buckets
    size_t table_size;      //the size of the table
    size_t element_count;   //the total elements I have in the table

    size_t hash(const Key& key) const {         //the hash function
        return std::hash<Key>{}(key) % table_size;
    }

    void rehash(){      //rehash function
        size_t new_size = next_prime(max<size_t>(2,table_size)*2);  //finds the new table size
        vector<pair<Key,Value>> old_pairs;  

        for(auto& b : table){   //takes every element that exists in table 
            if(b.kv)
                old_pairs.push_back(*b.kv);
        }
        table.clear();  //clears the table 
        table.resize(new_size);     //resizes the table 
        table_size = new_size;
        element_count = 0;

        for(auto& [k,v] : old_pairs)    //emplace all the elements again
            emplace(k,v);
    }

public:
    RobinHoodHashMap(size_t expected_elements = 64)     //takes an expected amount of elements
        : table_size(0), element_count(0){   
        if (expected_elements < 1) expected_elements = 1;   //check so the table will be at least 1 size
        table_size = next_prime(expected_elements * 2);     //finds the next prime of the double of elements that going to be inserted
        table.resize(table_size);   //sets the table size
    }

        struct iterator {
        using bucket_iter = typename vector<Bucket>::iterator;
        bucket_iter it; //iterator to cuurent position
        bucket_iter end_it; //iteraton to last position
        iterator(bucket_iter i, bucket_iter e) : it(i), end_it(e) {}
        auto& operator*() { return *it->kv; }   //returns the kv
        auto* operator->() { return &(*it->kv); }   //returns a pointer to the kv
        bool operator==(const iterator& other) const { return it == other.it; } //sees if 2 iterators are equal
        bool operator!=(const iterator& other) const { return it != other.it; } // ^^-
    };

    iterator end() { return iterator(table.end(), table.end()); }       //returns an iterator to end of table

    iterator find(const Key &key){
        if(table_size == 0) return end();
        size_t base = hash(key);
        size_t idx = base;
        size_t dist = 0;
        while(table[idx].distance >= dist && table[idx].kv){    
            if (table[idx].kv && table[idx].kv->first == key) {     //when we find the key
                    return iterator(table.begin() + idx, table.end());  //returns an iterator to keys position
            }
            if(idx == table_size - 1){
                idx = 0;
                dist ++;
                continue;
            }
            idx ++;
            dist ++;
        }
        return end();
    }

    pair<iterator,bool> emplace(const Key& key, const Value& value){
        size_t base = hash(key);    //the bucket that we want to place the kv 
        size_t dist = 0;        //distance from my bucket 
        size_t idx = base;
        while(table[idx].distance >= dist && table[idx].kv){    //tries to find the next bucket that is empty or it's element's distance is sorter than the new ones
            if(idx == table_size - 1){
                idx = 0;
                dist ++;
                continue;
            }
            dist ++;
            idx ++;
        }
        if(table[idx].kv){      //we free the bucket and place the new element
            auto kv = table[idx].kv;
            table[idx].kv.reset();
            table[idx].distance = dist;
            table[idx].kv = make_pair(key, value);
            emplace(kv.value().first, kv.value().second);       //we insert the old element again 
        }
        else{
            table[idx].distance = dist;
            table[idx].kv = make_pair(key, value);
            element_count ++;
            double load_factor = static_cast<double>(element_count) / static_cast<double>(table_size);
            if(load_factor > 0.6){
                rehash();
            }
        }
        return std::make_pair(iterator(table.begin() + idx, table.end()), true);
    }
};
