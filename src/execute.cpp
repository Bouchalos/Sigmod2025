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

    bool operator==(const value_t& other) const {
        if (type != other.type) return false;
        if (type == INT32) return int_val == other.int_val;
        if (type == VARCHAR) {
            return std::memcmp(&str_index, &other.str_index, sizeof(StringIndex)) == 0;
        }
        return true;
    }
};

namespace Contest {

constexpr size_t CHUNK_SIZE = 4096;

struct PagedColumn {
    
    std::vector<std::unique_ptr<value_t[]>> pages;
    size_t total_size = 0;
    size_t capacity = 0;

   
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


struct JoinAlgorithm {
    bool                                             build_left;
    ExecuteResult&                                   left;
    ExecuteResult&                                   right;
    ExecuteResult&                                   results;
    size_t                                           left_col, right_col;
    const std::vector<std::tuple<size_t, DataType>>& output_attrs;

    void run() {
        using JoinType = int32_t;
        
        auto& build_rel = build_left ? left : right;
        auto& probe_rel = build_left ? right : left;
        size_t build_col_idx = build_left ? left_col : right_col;
        size_t probe_col_idx = build_left ? right_col : left_col;

      
        results.resize(output_attrs.size());

       
        size_t build_rows = build_rel.empty() ? 0 : build_rel[0].size();
        size_t probe_rows = probe_rel.empty() ? 0 : probe_rel[0].size();

        HashTable<JoinType> hash_table(build_rows);

     
        for (size_t i = 0; i < build_rows; ++i) {
            value_t key_val = build_rel[build_col_idx].get(i);
            
            if (!key_val.is_null()) {
                JoinType key = key_val.int_val;
                auto itr = hash_table.find(key);
                if (itr == hash_table.end()) {
                    hash_table.emplace(key, std::vector<size_t>{i});
                } else {
                    itr->second.push_back(i);
                }
            }
        }

      
        for (size_t i = 0; i < probe_rows; ++i) {
            value_t key_val = probe_rel[probe_col_idx].get(i);

            if (!key_val.is_null()) {
                JoinType key = key_val.int_val;
                auto itr = hash_table.find(key);
                
                if (itr != hash_table.end()) {
                    for (size_t match_idx : itr->second) {
                       
                        
                        size_t left_row_idx  = build_left ? match_idx : i;
                        size_t right_row_idx = build_left ? i : match_idx;

                        
                        for (size_t out_idx = 0; out_idx < output_attrs.size(); ++out_idx) {
                            auto [src_col_idx, _] = output_attrs[out_idx];
                            
                         
                            
                            size_t left_cols_count = left.size();
                            
                            value_t val_to_append;
                            if (src_col_idx < left_cols_count) {
                                
                                val_to_append = left[src_col_idx].get(left_row_idx);
                            } else {
                                
                                val_to_append = right[src_col_idx - left_cols_count].get(right_row_idx);
                            }
                            
                            results[out_idx].append(val_to_append);
                        }
                    }
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

    JoinAlgorithm algo{
        .build_left   = join.build_left,
        .left         = left_res,
        .right        = right_res,
        .results      = results,
        .left_col     = join.left_attr,
        .right_col    = join.right_attr,
        .output_attrs = output_attrs
    };
    algo.run();

    return results;
}

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
        
        
        ColumnCursor cur(input_table, table_id, col_in_idx, type);
        
        
        for (size_t r = 0; r < total_rows; ++r) {
            results[i].append(cur.next());
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


Data materialize(const value_t& val, const Plan& plan) {
    if (val.type == value_t::INT32) {
        return val.int_val;
    }
    if (val.is_null()) {
        return std::monostate{};
    }
    if (val.type == value_t::VARCHAR) {
        StringIndex idx = val.str_index;
        
        if (idx.table_id >= plan.inputs.size()) return std::monostate{};
        const auto& table = plan.inputs[idx.table_id];
        const auto& col = table.columns[idx.col_id];
        if (idx.page_id >= col.pages.size()) return std::monostate{};
        
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

        
        if (idx.offset + idx.length > PAGE_SIZE) return std::string("");
        const char* ptr = reinterpret_cast<const char*>(page_data) + idx.offset;
        size_t len = idx.length;
        if (len > 0 && ptr[len - 1] == '\0') return std::string(ptr, len - 1);
        return std::string(ptr, len);
    }
    return std::monostate{};
}

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    
    auto columns = execute_impl(plan, plan.root);

    
    size_t num_rows = columns.empty() ? 0 : columns[0].size();
    size_t num_cols = columns.size();

    std::vector<std::vector<Data>> old_style_ret;
    old_style_ret.reserve(num_rows);

    for(size_t i = 0; i < num_rows; ++i) {
        std::vector<Data> row;
        row.reserve(num_cols);
        
        for(size_t j = 0; j < num_cols; ++j) {
           
            value_t val = columns[j].get(i);
            row.push_back(materialize(val, plan));
        }
        old_style_ret.push_back(std::move(row));
    }

    namespace views = ranges::views;
    auto ret_types  = plan.nodes[plan.root].output_attrs
                   | views::transform([](const auto& v) { return std::get<1>(v); })
                   | ranges::to<std::vector<DataType>>();
    
    Table table{std::move(old_style_ret), std::move(ret_types)};
    return table.to_columnar();
}

void* build_context() { return nullptr; }
void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest
