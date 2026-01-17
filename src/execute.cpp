#include <hardware.h>
#include <plan.h>
#include <table.h>
#include "macro.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <variant>
#include <cmath>
#include <memory>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <array>
#include <nmmintrin.h> 

struct StringIndex {
    uint64_t table_id : 6;
    uint64_t col_id   : 6;
    uint64_t page_id  : 16; 
    uint64_t offset   : 20; 
    uint64_t length   : 16; 
};

struct value_t {
    enum Type : uint8_t { INT32, VARCHAR, NULL_VAL } type;
    union {
        int32_t     int_val;
        StringIndex str_index;
    };

    value_t() : type(NULL_VAL), int_val(0) {}
    value_t(int32_t v) : type(INT32), int_val(v) {}
    value_t(StringIndex s) : type(VARCHAR), str_index(s) {}

    bool is_null() const { return type == NULL_VAL; }
};

namespace Contest {

struct PageHeader {
    uint16_t num_rows;
    uint16_t val_count;
};

constexpr size_t CHUNK_SIZE = 1024;

struct PagedColumn {
    std::vector<std::unique_ptr<value_t[]>> pages;
    size_t total_size = 0;
    size_t capacity = 0;

    bool is_view = false;
    std::vector<const int32_t*> view_chunks;
    uint32_t view_rows_per_page = 0;

    void append(value_t val) {
        if (total_size == capacity) {
            pages.emplace_back(std::make_unique<value_t[]>(CHUNK_SIZE));
            capacity += CHUNK_SIZE;
        }
        size_t page_idx = pages.size() - 1;
        size_t offset = total_size % CHUNK_SIZE;
        pages[page_idx][offset] = val;
        total_size++;
    }

    inline value_t get(size_t idx) const {
        if (is_view) {
            size_t page_idx = idx / view_rows_per_page;
            size_t offset   = idx % view_rows_per_page;
            return value_t(view_chunks[page_idx][offset]);
        }
        size_t page_idx = idx / CHUNK_SIZE; 
        size_t offset   = idx % CHUNK_SIZE; 
        return pages[page_idx][offset];
    }

    size_t size() const { return total_size; }
};

using ExecuteResult = std::vector<PagedColumn>;

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

inline bool get_bitmap_at(const uint8_t* bitmap, uint16_t idx) {
    return bitmap[idx / 8] & (1u << (idx % 8));
}

constexpr size_t GLOBAL_CHUNK_SIZE = 2 * 1024 * 1024; 
constexpr size_t PARTITION_BITS = 10;
constexpr size_t NUM_PARTITIONS = 1 << PARTITION_BITS;
constexpr size_t JOB_BATCH_SIZE = 4096; // Work Stealing Batch Size

struct BuildTuple {
    uint64_t hash;
    int32_t  key;
    uint32_t row_id;
};

class GlobalAllocator {
    std::vector<void*> pools;
    std::mutex mtx;
public:
    ~GlobalAllocator() { for (void* p : pools) std::free(p); }
    void* allocate() {
        void* ptr = std::malloc(GLOBAL_CHUNK_SIZE);
        std::lock_guard<std::mutex> lock(mtx);
        pools.push_back(ptr);
        return ptr;
    }
};

struct PartitionBuffer {
    struct Chunk {
        Chunk* next = nullptr;
        uint8_t data[]; 
    };
    Chunk* head = nullptr;
    Chunk* active = nullptr;
    size_t offset = 0;
    
    void add_tuple(const BuildTuple& t, GlobalAllocator& global) {
        constexpr size_t T_SIZE = sizeof(BuildTuple);
        constexpr size_t HEADER_SIZE = sizeof(Chunk);

        if (!active || offset + T_SIZE > GLOBAL_CHUNK_SIZE - HEADER_SIZE) {
            Chunk* new_chunk = static_cast<Chunk*>(global.allocate());
            new_chunk->next = head;
            head = new_chunk;
            active = new_chunk;
            offset = 0;
        }
        std::memcpy(active->data + offset, &t, T_SIZE);
        offset += T_SIZE;
    }
};

struct ThreadLocalAllocator {
    std::vector<PartitionBuffer> partitions;
    ThreadLocalAllocator() : partitions(NUM_PARTITIONS) {}
};

inline uint64_t next_pow2(uint64_t x) {
    if (x == 0) return 1;
    x--; x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; x |= x >> 32;
    return ++x;
}

inline uint64_t hash_key(int32_t key) {
    uint64_t h = _mm_crc32_u32(0, (uint32_t)key);
    h ^= (h << 32); 
    return h;
}

struct JoinAlgorithm {
    bool                                             build_left;
    ExecuteResult&                                   left;
    ExecuteResult&                                   right;
    ExecuteResult&                                   results;
    size_t                                           left_col, right_col;
    const std::vector<std::tuple<size_t, DataType>>& output_attrs;

    std::unique_ptr<BuildTuple[]> tuple_storage;
    std::vector<uint64_t> directory; 
    int directory_shift;

    void run() {
        auto& build_rel = build_left ? left : right;
        auto& probe_rel = build_left ? right : left;
        size_t build_col_idx = build_left ? left_col : right_col;
        size_t probe_col_idx = build_left ? right_col : left_col;

        results.resize(output_attrs.size());
        size_t build_rows = build_rel.empty() ? 0 : build_rel[0].size();
        if (build_rows == 0) return;

        uint64_t dir_size = next_pow2(build_rows); 
        if (dir_size < NUM_PARTITIONS) dir_size = NUM_PARTITIONS;
        directory.resize(dir_size + 1, 0); 
        directory_shift = 64 - __builtin_ctzll(dir_size);

        size_t num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
        std::vector<std::thread> threads;
        GlobalAllocator global_alloc;
        std::vector<std::unique_ptr<ThreadLocalAllocator>> thread_allocs(num_threads);

        // --- PHASE 1: PARTITIONING (Work Stealing) ---
        std::atomic<size_t> partition_counter{0};
        
        auto partition_task = [&](size_t t_id) {
            thread_allocs[t_id] = std::make_unique<ThreadLocalAllocator>();
            auto& local_parts = thread_allocs[t_id]->partitions;
            
            while (true) {
                size_t start = partition_counter.fetch_add(JOB_BATCH_SIZE);
                if (start >= build_rows) break;
                
                size_t end = std::min(start + JOB_BATCH_SIZE, build_rows);
                
                for (size_t i = start; i < end; ++i) {
                    value_t val = build_rel[build_col_idx].get(i);
                    if (!val.is_null()) {
                        int32_t key = val.int_val;
                        uint64_t hash = hash_key(key);
                        size_t part_id = hash >> (64 - PARTITION_BITS);
                        local_parts[part_id].add_tuple({hash, key, (uint32_t)i}, global_alloc);
                    }
                }
            }
        };

        for (size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back(partition_task, i);
        }
        for (auto& t : threads) t.join();
        threads.clear();

        // --- PHASE 2: HISTOGRAM (Work Stealing on Partitions) ---
        std::atomic<size_t> hist_counter{0};
        
        auto count_task = [&](size_t t_id) {
            while (true) {
                size_t p = hist_counter.fetch_add(1);
                if (p >= NUM_PARTITIONS) break;

                for (size_t t = 0; t < num_threads; ++t) {
                    auto& buffer = thread_allocs[t]->partitions[p];
                    auto* chunk = buffer.head;
                    while (chunk) {
                        size_t bytes_valid = (chunk == buffer.head) ? buffer.offset : GLOBAL_CHUNK_SIZE - sizeof(PartitionBuffer::Chunk);
                        size_t count = bytes_valid / sizeof(BuildTuple);
                        BuildTuple* tuples = reinterpret_cast<BuildTuple*>(chunk->data);
                        for (size_t k = 0; k < count; ++k) {
                            size_t slot = tuples[k].hash >> directory_shift;
                            directory[slot] += (1ULL << 32); 
                        }
                        chunk = chunk->next;
                    }
                }
            }
        };
        for (size_t i = 0; i < num_threads; ++i) threads.emplace_back(count_task, i);
        for (auto& t : threads) t.join();
        threads.clear();

        // --- PHASE 3: PREFIX SUM ---
        uint32_t total_count = 0;
        for (size_t i = 0; i < directory.size(); ++i) {
            uint32_t count = directory[i] >> 32;
            directory[i] = (uint64_t)total_count << 32; 
            total_count += count;
        }
        tuple_storage = std::make_unique<BuildTuple[]>(total_count);

        // --- PHASE 4: SCATTER (Work Stealing on Partitions) ---
        std::atomic<size_t> scatter_counter{0};
        
        auto scatter_task = [&](size_t t_id) {
            while (true) {
                size_t p = scatter_counter.fetch_add(1);
                if (p >= NUM_PARTITIONS) break;

                for (size_t t = 0; t < num_threads; ++t) {
                    auto& buffer = thread_allocs[t]->partitions[p];
                    auto* chunk = buffer.head;
                    while (chunk) {
                        size_t bytes_valid = (chunk == buffer.head) ? buffer.offset : GLOBAL_CHUNK_SIZE - sizeof(PartitionBuffer::Chunk);
                        size_t count = bytes_valid / sizeof(BuildTuple);
                        BuildTuple* tuples = reinterpret_cast<BuildTuple*>(chunk->data);
                        for (size_t k = 0; k < count; ++k) {
                            size_t slot = tuples[k].hash >> directory_shift;
                            uint64_t dir_entry = directory[slot];
                            
                            uint32_t write_pos = dir_entry >> 32;
                            uint32_t tags = dir_entry & 0xFFFFFFFF;
                            
                            tuple_storage[write_pos] = tuples[k];
                            uint32_t new_tag_mask = (1U << (tuples[k].hash & 31)); 
                            directory[slot] = ((uint64_t)(write_pos + 1) << 32) | (tags | new_tag_mask);
                        }
                        chunk = chunk->next;
                    }
                }
            }
        };
        for (size_t i = 0; i < num_threads; ++i) threads.emplace_back(scatter_task, i);
        for (auto& t : threads) t.join();
        threads.clear();

        // Restore Offsets
        uint64_t prev_entry = 0; 
        for (size_t i = 0; i < directory.size(); ++i) {
            uint64_t current_val = directory[i];
            uint32_t start_offset = prev_entry >> 32;
            uint32_t current_tags = current_val & 0xFFFFFFFF;
            directory[i] = ((uint64_t)start_offset << 32) | current_tags;
            prev_entry = current_val; 
        }

        // --- PHASE 5: PARALLEL PROBE (Work Stealing) ---
        std::vector<std::vector<std::vector<value_t>>> thread_results(num_threads);
        for(auto& res : thread_results) res.resize(output_attrs.size());
        
        size_t probe_rows = probe_rel.empty() ? 0 : probe_rel[0].size();
        std::atomic<size_t> probe_counter{0};

        auto probe_task = [&](size_t t_id) {
            auto& local_res = thread_results[t_id];
            // Pre-allocation heuristic
            for(auto& vec : local_res) vec.reserve(JOB_BATCH_SIZE); 

            while (true) {
                size_t start = probe_counter.fetch_add(JOB_BATCH_SIZE);
                if (start >= probe_rows) break;
                
                size_t end = std::min(start + JOB_BATCH_SIZE, probe_rows);
                
                for (size_t i = start; i < end; ++i) {
                    value_t val = probe_rel[probe_col_idx].get(i);
                    if (val.is_null()) continue;

                    int32_t key = val.int_val;
                    uint64_t hash = hash_key(key);
                    size_t slot = hash >> directory_shift;
                    
                    uint64_t dir_entry = directory[slot];
                    uint32_t start_idx = dir_entry >> 32;
                    uint32_t end_idx = directory[slot + 1] >> 32;
                    uint32_t tags = dir_entry & 0xFFFFFFFF;

                    uint32_t probe_tag_mask = (1U << (hash & 31));
                    if (!(tags & probe_tag_mask)) continue; 

                    for (uint32_t k = start_idx; k < end_idx; ++k) {
                        if (tuple_storage[k].key == key) {
                            size_t build_row_id = tuple_storage[k].row_id;
                            size_t probe_row_id = i;
                            size_t left_row_idx  = build_left ? build_row_id : probe_row_id;
                            size_t right_row_idx = build_left ? probe_row_id : build_row_id;

                            for (size_t out_idx = 0; out_idx < output_attrs.size(); ++out_idx) {
                                auto [src_col_idx, _] = output_attrs[out_idx];
                                size_t left_cols_count = left.size();
                                if (src_col_idx < left_cols_count) {
                                    local_res[out_idx].push_back(left[src_col_idx].get(left_row_idx));
                                } else {
                                    local_res[out_idx].push_back(right[src_col_idx - left_cols_count].get(right_row_idx));
                                }
                            }
                        }
                    }
                }
            }
        };

        for (size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back(probe_task, i);
        }
        for (auto& t : threads) t.join();

        // --- PHASE 6: AGGREGATE ---
        for (size_t t = 0; t < num_threads; ++t) {
            for (size_t col = 0; col < output_attrs.size(); ++col) {
                for (const auto& val : thread_results[t][col]) {
                    results[col].append(val);
                }
            }
        }
    }
};

ExecuteResult execute_hash_join(const Plan& plan,
    const JoinNode& join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto left_res  = execute_impl(plan, join.left);
    auto right_res = execute_impl(plan, join.right);
    ExecuteResult results; 
    JoinAlgorithm algo{join.build_left, left_res, right_res, results, join.left_attr, join.right_attr, output_attrs};
    algo.run();
    return results;
}

struct ColumnCursor {
    const Column* column_ptr;
    size_t         page_idx;
    const uint8_t* page_data;
    uint16_t       num_rows_in_page;
    uint16_t       current_row_in_page; 
    uint16_t       non_null_idx; 
    
    uint16_t       table_id;
    uint16_t       col_id;
    DataType       type;
    size_t         num_pages;

    const uint8_t* bitmap_ptr;
    const uint8_t* data_start_ptr; 
    const uint16_t* offsets_ptr;    
    const char* chars_base_ptr; 

    ColumnCursor(const ColumnarTable& table, size_t t_id, size_t c_id, DataType t) 
        : column_ptr(&table.columns[c_id]), page_idx(0), 
          table_id(t_id), col_id(c_id), type(t) {
        
        num_pages = column_ptr->pages.size();
        load_page(0);
    }

    void load_page(size_t idx) {
        page_idx = idx;
        current_row_in_page = 0;
        non_null_idx = 0;

        if (page_idx < num_pages) {
            page_data = reinterpret_cast<const uint8_t*>(column_ptr->pages[page_idx]->data);
            num_rows_in_page = *reinterpret_cast<const uint16_t*>(page_data);
            
            if (type == DataType::VARCHAR && (num_rows_in_page == 0xFFFF || num_rows_in_page == 0xFFFE)) {
                bitmap_ptr = nullptr;
                offsets_ptr = nullptr;
                return; 
            }

            size_t bitmap_size = (num_rows_in_page + 7) / 8;
            bitmap_ptr = page_data + PAGE_SIZE - bitmap_size;

            if (type == DataType::INT32) {
                data_start_ptr = page_data + 4;
            } else if (type == DataType::VARCHAR) {
                uint16_t num_offsets = *reinterpret_cast<const uint16_t*>(page_data + 2);
                offsets_ptr = reinterpret_cast<const uint16_t*>(page_data + 4);
                chars_base_ptr = reinterpret_cast<const char*>(page_data + 4 + num_offsets * 2);
            }
        } else {
            page_data = nullptr;
        }
    }

    void advance_page() {
        load_page(page_idx + 1);
    }

    value_t next() {
        while (true) { 
            if (!page_data) return value_t(); 

            if (type == DataType::VARCHAR && num_rows_in_page == 0xFFFE) {
                advance_page();
                continue;
            }

            if (type == DataType::VARCHAR && num_rows_in_page == 0xFFFF) {
                uint16_t chunk_len = *reinterpret_cast<const uint16_t*>(page_data + 2);
                StringIndex idx;
                idx.table_id = table_id;
                idx.col_id   = col_id;
                idx.page_id  = page_idx; 
                idx.offset   = 4; 
                idx.length   = 0; 

                advance_page(); 
                return value_t(idx);
            }

            if (current_row_in_page >= num_rows_in_page) {
                advance_page();
                continue; 
            }

            bool is_valid = false;
            if (bitmap_ptr) {
                is_valid = get_bitmap_at(bitmap_ptr, current_row_in_page);
            }
            current_row_in_page++;

            if (!is_valid) {
                return value_t(); 
            }

            if (type == DataType::INT32) {
                const int32_t* arr = reinterpret_cast<const int32_t*>(data_start_ptr);
                int32_t val = arr[non_null_idx++];
                return value_t(val);

            } else if (type == DataType::VARCHAR) {
                uint16_t end_offset = offsets_ptr[non_null_idx];
                uint16_t start_offset = (non_null_idx == 0) ? 0 : offsets_ptr[non_null_idx - 1];
                uint16_t len = end_offset - start_offset;
                
                uint64_t base_delta = reinterpret_cast<const uint8_t*>(chars_base_ptr) - page_data;
                uint32_t final_offset = static_cast<uint32_t>(base_delta) + static_cast<uint32_t>(start_offset);

                if (final_offset + len > PAGE_SIZE) {
                    advance_page();
                    continue; 
                }

                non_null_idx++;

                StringIndex idx;
                idx.table_id = table_id;
                idx.col_id   = col_id;
                idx.page_id  = page_idx;
                idx.offset   = final_offset;
                idx.length   = len;

                return value_t(idx);
            }
        }
    }
};

ExecuteResult execute_scan(const Plan& plan,
                           const ScanNode& scan,
                           const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    
    auto table_id = scan.base_table_id;
    const auto& input_table = plan.inputs[table_id];
    size_t total_rows = input_table.num_rows;
    
    ExecuteResult results;
    results.resize(output_attrs.size());

    for (size_t i = 0; i < output_attrs.size(); ++i) {
        auto [col_in_idx, type] = output_attrs[i];
        
        bool can_optimize = (type == DataType::INT32);
        
        if (can_optimize) {
            for (auto* page : input_table.columns[col_in_idx].pages) {
                auto* header = reinterpret_cast<const PageHeader*>(page->data);
                if (header->num_rows != header->val_count) {
                    can_optimize = false;
                    break;
                }
            }
        }

        if (can_optimize) {
            auto& col_res = results[i];
            col_res.is_view = true;
            col_res.total_size = total_rows;
            
            if (!input_table.columns[col_in_idx].pages.empty()) {
                auto* header0 = reinterpret_cast<const PageHeader*>(input_table.columns[col_in_idx].pages[0]->data);
                col_res.view_rows_per_page = header0->num_rows;
            }

            for (auto* page : input_table.columns[col_in_idx].pages) {
                const int32_t* raw_data = reinterpret_cast<const int32_t*>(
                    reinterpret_cast<const uint8_t*>(page->data) + 4
                );
                col_res.view_chunks.push_back(raw_data);
            }
        } else {
            ColumnCursor cur(input_table, table_id, col_in_idx, type);
            for (size_t r = 0; r < total_rows; ++r) {
                results[i].append(cur.next());
            }
        }
    }
    
    return results;
}

ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>) {
                return execute_hash_join(plan, value, node.output_attrs);
            } else {
                return execute_scan(plan, value, node.output_attrs);
            }
        },
        node.data);
}

std::string materialize_string(const value_t& val, const Plan& plan) {
    StringIndex idx = val.str_index;
    if (idx.table_id >= plan.inputs.size()) return "";
    const auto& col = plan.inputs[idx.table_id].columns[idx.col_id];
    if (idx.page_id >= col.pages.size()) return "";
    
    const uint8_t* page_data = reinterpret_cast<const uint8_t*>(col.pages[idx.page_id]->data);
    uint16_t header = *reinterpret_cast<const uint16_t*>(page_data);

    if (header == 0xFFFF) {
        std::string full_string;
        uint16_t chunk_len = *reinterpret_cast<const uint16_t*>(page_data + 2);
        if (chunk_len > PAGE_SIZE) chunk_len = 0;
        full_string.append(reinterpret_cast<const char*>(page_data + 4), chunk_len);
        
        size_t next_page_idx = idx.page_id + 1;
        while (next_page_idx < col.pages.size()) {
            const uint8_t* next_page_data = reinterpret_cast<const uint8_t*>(col.pages[next_page_idx]->data);
            uint16_t next_header = *reinterpret_cast<const uint16_t*>(next_page_data);
            if (next_header != 0xFFFE) break; 
            uint16_t next_chunk_len = *reinterpret_cast<const uint16_t*>(next_page_data + 2);
            if (next_chunk_len > PAGE_SIZE) break;
            full_string.append(reinterpret_cast<const char*>(next_page_data + 4), next_chunk_len);
            next_page_idx++;
        }
        if (!full_string.empty() && full_string.back() == '\0') full_string.pop_back();
        return full_string;
    }
    
    if (idx.offset + idx.length > PAGE_SIZE) return "";
    const char* ptr = reinterpret_cast<const char*>(page_data) + idx.offset;
    size_t len = idx.length;
    if (len > 0 && ptr[len - 1] == '\0') return std::string(ptr, len - 1);
    return std::string(ptr, len);
}

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    auto columns = execute_impl(plan, plan.root);
    
    size_t num_rows = columns.empty() ? 0 : columns[0].size();
    size_t num_cols = columns.size();

    ColumnarTable output_table;
    output_table.num_rows = num_rows;

    for(size_t j = 0; j < num_cols; ++j) {
        auto type = std::get<1>(plan.nodes[plan.root].output_attrs[j]);
        Column out_col(type);

        if (type == DataType::INT32) {
            ColumnInserter<int32_t> inserter(out_col);
            for (size_t i = 0; i < num_rows; ++i) {
                value_t val = columns[j].get(i);
                if (val.is_null()) inserter.insert_null();
                else inserter.insert(val.int_val);
            }
            inserter.finalize();
        } 
        else if (type == DataType::VARCHAR) {
            ColumnInserter<std::string> inserter(out_col);
            for (size_t i = 0; i < num_rows; ++i) {
                value_t val = columns[j].get(i);
                if (val.is_null()) inserter.insert_null();
                else {
                    std::string s = materialize_string(val, plan);
                    inserter.insert(s);
                }
            }
            inserter.finalize();
        } else if (type == DataType::INT64) {
             ColumnInserter<int64_t> inserter(out_col);
             inserter.finalize();
        } else if (type == DataType::FP64) {
             ColumnInserter<double> inserter(out_col);
             inserter.finalize();
        }
        
        output_table.columns.push_back(std::move(out_col));
    }

    return output_table;
}

void* build_context() { return nullptr; }
void destroy_context([[maybe_unused]] void* context) {}

}