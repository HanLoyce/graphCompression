#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>

using namespace std;
using namespace std::chrono;

const uint32_t NO_PARENT = UINT32_MAX;

// ============================================================================
// Shared utility library: Variable-byte encoding (VByte)
// ============================================================================
inline void encodeVByte(uint32_t val, vector<uint8_t>& stream) {
    while (val >= 128) { stream.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80)); val >>= 7; }
    stream.push_back(static_cast<uint8_t>(val));
}
inline uint32_t decodeVByte(const vector<uint8_t>& stream, size_t& pos) {
    uint32_t val = 0; int shift = 0;
    while (true) {
        uint8_t b = stream[pos++]; val |= (b & 0x7F) << shift;
        if ((b & 0x80) == 0) break; shift += 7;
    }
    return val;
}

// 提取文件名的基础名称
string getBaseName(const string& path) {
    size_t last_slash = path.find_last_of("/\\");
    size_t last_dot = path.find_last_of(".");
    size_t start = (last_slash == string::npos) ? 0 : last_slash + 1;
    size_t end = (last_dot == string::npos) ? path.length() : last_dot;
    return path.substr(start, end - start);
}

// ============================================================================
// 你的图加载类
// ============================================================================
class GraphLoader {
public:
    uint32_t num_nodes = 0, num_edges = 0;
    vector<uint32_t> row_ptr, col_idx;
    bool loadFromText(const string& filename) {
        ifstream infile(filename);
        if (!infile.is_open()) { cerr << "[ERROR] File not found: " << filename << "\n"; return false; }

        string line;
        uint32_t max_node_id = 0;
        vector<uint32_t> degree;
        bool has_edge = false;

        while (getline(infile, line)) {
            if (line.empty() || line[0] == '#' || line[0] == '%') continue;
            replace(line.begin(), line.end(), ',', ' ');
            stringstream ss(line);
            uint32_t u, v;
            if (!(ss >> u >> v)) continue;
            if (u == v) continue;

            has_edge = true;
            max_node_id = max(max_node_id, max(u, v));
            if (degree.size() <= max_node_id) degree.resize(static_cast<size_t>(max_node_id) + 1, 0);
            degree[u]++;
            degree[v]++;
        }
        infile.close();

        if (!has_edge) return false;

        num_nodes = max_node_id + 1;
        row_ptr.assign(static_cast<size_t>(num_nodes) + 1, 0);
        for (uint32_t i = 0; i < num_nodes; ++i) row_ptr[i + 1] = row_ptr[i] + degree[i];

        col_idx.assign(static_cast<size_t>(row_ptr[num_nodes]), 0);
        vector<uint32_t> cursor = row_ptr;

        infile.open(filename);
        while (getline(infile, line)) {
            if (line.empty() || line[0] == '#' || line[0] == '%') continue;
            replace(line.begin(), line.end(), ',', ' ');
            stringstream ss(line);
            uint32_t u, v;
            if (!(ss >> u >> v)) continue;
            if (u == v) continue;
            col_idx[cursor[u]++] = v;
            col_idx[cursor[v]++] = u;
        }
        infile.close();

        vector<uint32_t> compact_col;
        compact_col.reserve(col_idx.size());
        vector<uint32_t> new_row_ptr(static_cast<size_t>(num_nodes) + 1, 0);

        for (uint32_t i = 0; i < num_nodes; ++i) {
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (start == end) { new_row_ptr[i + 1] = static_cast<uint32_t>(compact_col.size()); continue; }
            sort(col_idx.begin() + start, col_idx.begin() + end);
            uint32_t prev = col_idx[start];
            compact_col.push_back(prev);
            for (uint32_t j = start + 1; j < end; ++j) {
                uint32_t x = col_idx[j];
                if (x != prev) { compact_col.push_back(x); prev = x; }
            }
            new_row_ptr[i + 1] = static_cast<uint32_t>(compact_col.size());
        }

        row_ptr.swap(new_row_ptr);
        col_idx.swap(compact_col);
        num_edges = static_cast<uint32_t>(col_idx.size());
        return true;
    }
};

// ============================================================================
// 你的图压缩类 (将其核心结构设为 public 供下游任务使用)
// ============================================================================
class IndustrialGraphCompressor {
public: // 设为 public，让下游任务可以获取压缩后的数据
    vector<uint32_t> parent;
    vector<uint32_t> byte_ptr;
    vector<uint8_t> byte_stream;

private:
    uint32_t num_nodes, num_edges; 
    const vector<uint32_t>& row_ptr; 
    vector<uint32_t> col_idx; 
    vector<uint32_t> freq; 
    vector<bool> in_list, is_packed; 
    uint32_t max_depth;
    struct TempCmd { uint32_t bottom, steps; bool is_path; };

public:
    IndustrialGraphCompressor(uint32_t n, uint32_t e, const vector<uint32_t>& r, const vector<uint32_t>& c) 
        : num_nodes(n), num_edges(e), row_ptr(r), col_idx(c), max_depth(127) {
        freq.assign(num_nodes, 0); 
        parent.assign(num_nodes, NO_PARENT);
        in_list.assign(num_nodes, false); 
        is_packed.assign(num_nodes, false); 
        byte_ptr.assign(num_nodes + 1, 0);
    }

    void runCompression() {
        for (uint32_t i = 0; i < num_edges; ++i) freq[col_idx[i]]++;
        for (uint32_t i = 0; i < num_nodes; ++i) {
            uint32_t start = row_ptr[i], end = row_ptr[i+1];
            if (end - start < 2) continue;
            sort(col_idx.begin() + start, col_idx.begin() + end, [&](uint32_t a, uint32_t b) {
                if (freq[a] != freq[b]) return freq[a] > freq[b]; return a < b;
            });
        }
        for (uint32_t i = 0; i < num_nodes; ++i) {
            uint32_t start = row_ptr[i], end = row_ptr[i+1];
            if (end - start < 2) continue;
            for (uint32_t j = start; j < end - 1; ++j) {
                if (parent[col_idx[j+1]] == NO_PARENT) parent[col_idx[j+1]] = col_idx[j];
            }
        }
        for (uint32_t i = 0; i < num_nodes; ++i) {
            byte_ptr[i] = byte_stream.size(); 
            uint32_t start = row_ptr[i], end = row_ptr[i+1];
            if (start == end) continue;
            for (uint32_t j = start; j < end; ++j) { in_list[col_idx[j]] = true; is_packed[col_idx[j]] = false; }
            vector<TempCmd> temp_cmds; 
            for (int j = (int)end - 1; j >= (int)start; --j) {
                uint32_t bottom = col_idx[j];
                if (is_packed[bottom]) continue;
                uint32_t curr = bottom, steps = 0; vector<uint32_t> path_nodes = {curr};
                while (parent[curr] != NO_PARENT && in_list[parent[curr]] && !is_packed[parent[curr]] && steps < max_depth) {
                    curr = parent[curr]; path_nodes.push_back(curr); steps++;
                }
                if (steps >= 1) {
                    for (uint32_t node : path_nodes) is_packed[node] = true;
                    temp_cmds.push_back({bottom, steps, true});
                } else { is_packed[bottom] = true; temp_cmds.push_back({bottom, 0, false}); }
            }
            for (auto it = temp_cmds.rbegin(); it != temp_cmds.rend(); ++it) {
                encodeVByte((it->bottom << 1) | (it->is_path ? 1 : 0), byte_stream);
                if (it->is_path) encodeVByte(it->steps, byte_stream);
            }
            for (uint32_t j = start; j < end; ++j) in_list[col_idx[j]] = false;
        }
        byte_ptr[num_nodes] = byte_stream.size(); 
    }
};

// 按照节点度数（频率）降序进行 ReID 重排序
void reorderGraphByDegree(uint32_t num_nodes, vector<uint32_t>& row_ptr, vector<uint32_t>& col_idx) {
    // 1. 统计每个节点的度数 (Degree)
    vector<pair<uint32_t, uint32_t>> deg_node(num_nodes);
    for (uint32_t i = 0; i < num_nodes; ++i) {
        deg_node[i] = {row_ptr[i + 1] - row_ptr[i], i};
    }

    // 2. 按度数从大到小排序
    sort(deg_node.begin(), deg_node.end(), [](const pair<uint32_t, uint32_t>& a, const pair<uint32_t, uint32_t>& b) {
        if (a.first != b.first) return a.first > b.first; // 度数大的排前面 (ID变小)
        return a.second < b.second;
    });

    // 3. 构建新旧 ID 映射表
    vector<uint32_t> old_to_new(num_nodes);
    for (uint32_t new_id = 0; new_id < num_nodes; ++new_id) {
        old_to_new[deg_node[new_id].second] = new_id;
    }

    // 4. 重建邻接表 (使用新 ID)
    vector<vector<uint32_t>> new_adj(num_nodes);
    for (uint32_t old_id = 0; old_id < num_nodes; ++old_id) {
        uint32_t new_id = old_to_new[old_id];
        for (uint32_t j = row_ptr[old_id]; j < row_ptr[old_id + 1]; ++j) {
            new_adj[new_id].push_back(old_to_new[col_idx[j]]);
        }
    }

    // 5. 将重建后的数据写回 row_ptr 和 col_idx，并严格升序排列
    uint32_t edge_cnt = 0;
    for (uint32_t i = 0; i < num_nodes; ++i) {
        row_ptr[i] = edge_cnt;
        sort(new_adj[i].begin(), new_adj[i].end());
        for (uint32_t neighbor : new_adj[i]) {
            col_idx[edge_cnt++] = neighbor;
        }
    }
    row_ptr[num_nodes] = edge_cnt;
}

// ============================================================================
// 标准并查集 (DSU) 数据结构
// ============================================================================
struct DSU {
    vector<uint32_t> parent;
    vector<uint8_t> rank;
    uint32_t components; // 连通分量个数

    DSU(uint32_t n) : components(n) {
        parent.resize(n);
        rank.assign(n, 0);
        for (uint32_t i = 0; i < n; ++i) parent[i] = i;
    }

    uint32_t find(uint32_t i) {
        uint32_t root = i;
        while (parent[root] != root) root = parent[root];
        while (parent[i] != i) {
            uint32_t next = parent[i];
            parent[i] = root;
            i = next;
        }
        return root;
    }

    void unite(uint32_t i, uint32_t j) {
        uint32_t root_i = find(i);
        uint32_t root_j = find(j);
        if (root_i != root_j) {
            if (rank[root_i] < rank[root_j]) {
                parent[root_i] = root_j;
            } else if (rank[root_i] > rank[root_j]) {
                parent[root_j] = root_i;
            } else {
                parent[root_j] = root_i;
                rank[root_i]++;
            }
            components--; // 每次成功合并，分量总数减 1
        }
    }
};

// ============================================================================
// 算法 1: 在未压缩的原图上跑连通分量 (Baseline)
// ============================================================================
uint32_t runUncompressedCC(uint32_t num_nodes, const vector<uint32_t>& row_ptr, const vector<uint32_t>& col_idx) {
    DSU dsu(num_nodes);
    // 遍历 CSR 图的所有边
    for (uint32_t u = 0; u < num_nodes; ++u) {
        for (uint32_t j = row_ptr[u]; j < row_ptr[u+1]; ++j) {
            uint32_t v = col_idx[j];
            dsu.unite(u, v);
        }
    }
    return dsu.components;
}

// ============================================================================
// 算法 2: 在压缩后的数据结构上跑连通分量 (Fast CC)
// 核心魔法：利用树结构进行超光速合并，忽略多余的边遍历！
// ============================================================================
uint32_t runCompressedCC(uint32_t num_nodes, 
                         const vector<uint32_t>& parent_tree, 
                         const vector<uint32_t>& byte_ptr, 
                         const vector<uint8_t>& byte_stream) {
    DSU dsu(num_nodes);

    // 阶段一：神级白嫖！利用你的“二跳树”进行极速合并
    for (uint32_t i = 0; i < num_nodes; ++i) {
        if (parent_tree[i] != NO_PARENT) {
            dsu.unite(i, parent_tree[i]);
        }
    }

    // 阶段二：解析残余边（跳过冗余的解压操作！）
    for (uint32_t u = 0; u < num_nodes; ++u) {
        size_t pos = byte_ptr[u];
        size_t end_pos = byte_ptr[u+1];
        
        while (pos < end_pos) {
            uint32_t val = decodeVByte(byte_stream, pos);
            uint8_t flag = val & 1; 
            uint32_t bottom = val >> 1;

            // 把节点 u 直接和 bottom 连起来
            dsu.unite(u, bottom); 

            if (flag == 1) {
                // 如果是路径，直接解码跳过！绝对不要去循环还原边！
                // 因为阶段一已经把 bottom 顺着树的路都连通了
                decodeVByte(byte_stream, pos); 
            }
        }
    }
    return dsu.components;
}

// ============================================================================
// Main 函数
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: ./benchmark graph1.edges graph2.edges ...\n";
        return -1;
    }

    cout << "==========================================================================\n";
    cout << left << setw(20) << "Dataset" 
         << setw(15) << "Components" 
         << setw(15) << "RawTime(ms)" 
         << setw(15) << "CompTime(ms)" 
         << setw(10) << "Speedup" << "\n";
    cout << "--------------------------------------------------------------------------\n";

        uint32_t total_checked = 0;
        uint32_t total_correct = 0;
        uint32_t total_load_failed = 0;

    for (int i = 1; i < argc; ++i) {
        string filepath = argv[i];
        string dataset_name = getBaseName(filepath);
        
        // 1. 加载原图
        GraphLoader loader;
        if (!loader.loadFromText(filepath)) {
            total_load_failed++;
            continue;
        }

        // ---------------------------------------------------------
        // 测试 A: 跑原图连通分量并计时
        // ---------------------------------------------------------
        auto start_uncomp = high_resolution_clock::now();
        uint32_t cc_uncomp = runUncompressedCC(loader.num_nodes, loader.row_ptr, loader.col_idx);
        auto end_uncomp = high_resolution_clock::now();
        double time_uncomp_ms = duration<double, milli>(end_uncomp - start_uncomp).count();

        // 在压缩前应用 ReID 重排
        reorderGraphByDegree(loader.num_nodes, loader.row_ptr, loader.col_idx);

        // ---------------------------------------------------------
        // 压缩阶段：先压缩好图（注意：这里不计算到下游任务耗时里！）
        // ---------------------------------------------------------
        IndustrialGraphCompressor compressor(loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx);
        compressor.runCompression(); // 压缩完成，核心数据存在 compressor.parent/byte_ptr/byte_stream 中

        // ---------------------------------------------------------
        // 测试 B: 跑压缩图连通分量并计时
        // ---------------------------------------------------------
        auto start_comp = high_resolution_clock::now();
        // 直接把压缩结构丢进去跑
        uint32_t cc_comp = runCompressedCC(loader.num_nodes, compressor.parent, compressor.byte_ptr, compressor.byte_stream);
        auto end_comp = high_resolution_clock::now();
        double time_comp_ms = duration<double, milli>(end_comp - start_comp).count();
        total_checked++;

        // ---------------------------------------------------------
        // 结果验证与输出
        // ---------------------------------------------------------
        if (cc_uncomp != cc_comp) {
            cerr << "[ERROR] Dataset " << dataset_name << " mismatch! raw=" << cc_uncomp << ", compressed=" << cc_comp << "\n";
            continue;
        }

        total_correct++;

        double speedup = time_uncomp_ms / time_comp_ms;

        cout << left << setw(20) << dataset_name 
             << setw(15) << cc_uncomp 
             << fixed << setprecision(3) << setw(15) << time_uncomp_ms 
             << setw(15) << time_comp_ms 
             << setprecision(2) << setw(10) << speedup << "x\n";
    }

    cout << "==========================================================================\n";
    cout << "Verification Summary\n";
    cout << "Checked datasets: " << total_checked << "\n";
    cout << "Correct datasets: " << total_correct << "\n";
    cout << "Load failed: " << total_load_failed << "\n";
    double accuracy = 0.0;
    if (total_checked > 0) {
        accuracy = 100.0 * static_cast<double>(total_correct) / static_cast<double>(total_checked);
    }
    cout << "CC accuracy: " << fixed << setprecision(2) << accuracy << "%\n";
    return 0;
}