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

const uint32_t NO_PARENT = UINT32_MAX;

// ============================================================================
// 共享工具库：变长字节编码 (VByte)
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

// 提取文件名
string getBaseName(const string& path) {
    size_t last_slash = path.find_last_of("/\\");
    size_t last_dot = path.find_last_of(".");
    size_t start = (last_slash == string::npos) ? 0 : last_slash + 1;
    size_t end = (last_dot == string::npos) ? path.length() : last_dot;
    return path.substr(start, end - start);
}

// 评估结果结构体
struct BenchResult {
    string dataset;
    string name;
    double data_mb;
    double bpe;
    double meps;
    bool correct;
};

// ============================================================================
// 模块定义区：保持核心算法完全不变
// ============================================================================
class GraphLoader {
public:
    uint32_t num_nodes = 0, num_edges = 0;
    vector<uint32_t> row_ptr, col_idx;
    bool loadFromText(const string& filename) {
        ifstream infile(filename);
        if (!infile.is_open()) { cerr << "[ERROR] File not found: " << filename << "\n"; return false; }
        string line; vector<pair<uint32_t, uint32_t>> edges; uint32_t max_node_id = 0;
        while (getline(infile, line)) {
            if (line.empty() || line[0] == '#' || line[0] == '%') continue; 
            replace(line.begin(), line.end(), ',', ' ');
            stringstream ss(line); uint32_t u, v;
            if (ss >> u >> v) {
                if (u == v) continue; 
                edges.push_back({u, v}); edges.push_back({v, u}); 
                max_node_id = max({max_node_id, u, v});
            }
        }
        infile.close();
        num_nodes = max_node_id + 1; 
        vector<vector<uint32_t>> adj(num_nodes);
        for (const auto& e : edges) adj[e.first].push_back(e.second);
        row_ptr.assign(num_nodes + 1, 0);
        for (uint32_t i = 0; i < num_nodes; ++i) {
            sort(adj[i].begin(), adj[i].end());
            adj[i].erase(unique(adj[i].begin(), adj[i].end()), adj[i].end());
            row_ptr[i] = col_idx.size();
            for (uint32_t neighbor : adj[i]) col_idx.push_back(neighbor);
        }
        row_ptr[num_nodes] = col_idx.size(); num_edges = col_idx.size();
        return true;
    }
};

BenchResult runUncompressedBaseline(const string& dataset, uint32_t num_nodes, uint32_t num_edges, 
                                    const vector<uint32_t>& row_ptr, const vector<uint32_t>& col_idx) {
    volatile uint32_t dummy_sum = 0; 
    auto start_time = chrono::high_resolution_clock::now();
    for (uint32_t i = 0; i < num_nodes; ++i) {
        for (uint32_t j = row_ptr[i]; j < row_ptr[i+1]; ++j) dummy_sum += col_idx[j]; 
    }
    auto end_time = chrono::high_resolution_clock::now();
    chrono::duration<double, std::milli> elapsed = end_time - start_time;
    return {dataset, "Uncompressed (32-bit)", (num_edges * 4.0) / 1048576.0, 32.0, (num_edges / (elapsed.count() / 1000.0)) / 1000000.0, true};
}

class DeltaVByteCompressor {
    uint32_t num_nodes, num_edges; const vector<uint32_t>& row_ptr; const vector<uint32_t>& col_idx;
    vector<uint32_t> byte_ptr; vector<uint8_t> byte_stream; string dataset;
public:
    DeltaVByteCompressor(string ds, uint32_t n, uint32_t e, const vector<uint32_t>& r, const vector<uint32_t>& c)
        : dataset(ds), num_nodes(n), num_edges(e), row_ptr(r), col_idx(c) {}
    BenchResult run() {
        byte_ptr.assign(num_nodes + 1, 0);
        for (uint32_t i = 0; i < num_nodes; ++i) {
            byte_ptr[i] = byte_stream.size(); uint32_t prev = 0;
            for (uint32_t j = row_ptr[i]; j < row_ptr[i+1]; ++j) {
                uint32_t curr = col_idx[j]; encodeVByte(curr - prev, byte_stream); prev = curr;
            }
        }
        byte_ptr[num_nodes] = byte_stream.size();
        bool correct = true; uint32_t decoded_edges = 0;
        auto start_time = chrono::high_resolution_clock::now();
        for (uint32_t i = 0; i < num_nodes; ++i) {
            size_t pos = byte_ptr[i], end_pos = byte_ptr[i+1]; uint32_t prev = 0;
            while (pos < end_pos) { prev = prev + decodeVByte(byte_stream, pos); decoded_edges++; }
        }
        auto end_time = chrono::high_resolution_clock::now();
        chrono::duration<double, std::milli> elapsed = end_time - start_time;
        return {dataset, "Delta + VByte", byte_stream.size() / 1048576.0, (byte_stream.size() * 8.0) / num_edges, (num_edges / (elapsed.count() / 1000.0)) / 1000000.0, decoded_edges == num_edges};
    }
};

class IndustrialGraphCompressor {
    uint32_t num_nodes, num_edges; const vector<uint32_t>& row_ptr; vector<uint32_t> col_idx; 
    vector<uint32_t> freq, parent, byte_ptr; vector<bool> in_list, is_packed; uint32_t max_depth;
    vector<uint8_t> byte_stream; string dataset; struct TempCmd { uint32_t bottom, steps; bool is_path; };
public:
    IndustrialGraphCompressor(string ds, uint32_t n, uint32_t e, const vector<uint32_t>& r, const vector<uint32_t>& c) 
        : dataset(ds), num_nodes(n), num_edges(e), row_ptr(r), col_idx(c), max_depth(127) {
        freq.assign(num_nodes, 0); parent.assign(num_nodes, NO_PARENT);
        in_list.assign(num_nodes, false); is_packed.assign(num_nodes, false); byte_ptr.assign(num_nodes + 1, 0);
    }
    BenchResult run() {
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

        uint32_t decoded_edges = 0;
        auto start_time = chrono::high_resolution_clock::now();
        for (uint32_t i = 0; i < num_nodes; ++i) {
            size_t pos = byte_ptr[i], end_pos = byte_ptr[i+1];
            while (pos < end_pos) {
                uint32_t val = decodeVByte(byte_stream, pos);
                uint8_t flag = val & 1; uint32_t curr = val >> 1; decoded_edges++;
                if (flag == 1) {
                    uint32_t steps = decodeVByte(byte_stream, pos);
                    for (uint32_t s = 0; s < steps; ++s) { curr = parent[curr]; decoded_edges++; }
                }
            }
        }
        auto end_time = chrono::high_resolution_clock::now();
        chrono::duration<double, std::milli> elapsed = end_time - start_time;
        return {dataset, "Your Algorithm (Tree+VB)", byte_stream.size() / 1048576.0, (byte_stream.size() * 8.0) / num_edges, (num_edges / (elapsed.count() / 1000.0)) / 1000000.0, decoded_edges == num_edges};
    }
};

// ============================================================================
// Format and print a per-dataset table with compression summary.
// ============================================================================
void printTableForDataset(const string& dataset_name, const vector<BenchResult>& results) {
    cout << "\n============================== Performance Report: " << left << setw(20) << dataset_name << " ==============================\n";
    cout << left << setw(28) << "Algorithm" 
         << right << setw(14) << "Data Size(MB)" 
         << setw(16) << "BPE(bits/edge)" 
         << setw(14) << "Ratio (%)" 
         << setw(18) << "Speed (MEPS)" 
         << setw(10) << "Correct" << "\n";
    cout << "----------------------------------------------------------------------------------------------------\n";
    
    double uncomp_bpe = 32.0;
    double delta_bpe = 32.0;
    double your_bpe = 32.0;

    for (const auto& r : results) {
        double ratio = (r.bpe / 32.0) * 100.0;
        cout << left << setw(28) << r.name 
             << right << setw(14) << fixed << setprecision(2) << r.data_mb 
             << setw(16) << fixed << setprecision(2) << r.bpe 
             << setw(13) << fixed << setprecision(2) << ratio << "%"
             << setw(18) << fixed << setprecision(2) << r.meps 
             << setw(10) << (r.correct ? "YES" : "NO") << "\n";
             
        if (r.name == "Delta + VByte") delta_bpe = r.bpe;
        if (r.name == "Your Algorithm (Tree+VB)") your_bpe = r.bpe;
    }
    cout << "====================================================================================================\n";
    
    double Compression_ratio_from_raw = (your_bpe / uncomp_bpe) * 100.0;
    double Compression_ratio_from_delta = (your_bpe / delta_bpe) * 100.0;
    
}

// ============================================================================
// Main program flow
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: ./benchmark graph1.edges graph2.edges graph3.edges ...\n";
        return -1;
    }

    vector<BenchResult> all_results;

    for (int i = 1; i < argc; ++i) {
        string filepath = argv[i];
        string dataset_name = getBaseName(filepath);
        cout << "\n[Processing] Dataset: " << dataset_name << " ...\n";
        
        GraphLoader loader;
        if (!loader.loadFromText(filepath)) continue;

        vector<BenchResult> current_dataset_results;

        current_dataset_results.push_back(runUncompressedBaseline(dataset_name, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx));
        
        DeltaVByteCompressor delta_comp(dataset_name, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx);
        current_dataset_results.push_back(delta_comp.run());
        
        IndustrialGraphCompressor your_comp(dataset_name, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx);
        current_dataset_results.push_back(your_comp.run());

        printTableForDataset(dataset_name, current_dataset_results);

        all_results.insert(all_results.end(), current_dataset_results.begin(), current_dataset_results.end());
    }

    ofstream csv("benchmark_results.csv");
    csv << "Dataset,Algorithm,Data_MB,BPE,MEPS,Correct\n";
    for (const auto& r : all_results) {
        csv << r.dataset << "," << r.name << "," << r.data_mb << "," << r.bpe << "," << r.meps << "," << (r.correct?1:0) << "\n";
    }
    csv.close();

    cout << "All benchmarks completed. Results have been exported to 'benchmark_results.csv'. Run the Python script to generate plots.\n";
    return 0;
}