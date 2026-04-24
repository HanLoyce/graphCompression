#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>
#include <unordered_map>
#include <set>
#include <limits>
#include <cmath>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

using namespace std;

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

// Extract base filename without path and suffix (for chart display)
string getBaseName(const string& path) {
    size_t last_slash = path.find_last_of("/\\");
    size_t last_dot = path.find_last_of(".");
    size_t start = (last_slash == string::npos) ? 0 : last_slash + 1;
    size_t end = (last_dot == string::npos) ? path.length() : last_dot;
    return path.substr(start, end - start);
}

// Evaluation result structure
struct BenchResult {
    string dataset;
    string name;
    double data_mb;
    double bpe;
    int correct; // 1: YES, 0: NO, -1: empty/unknown
};

static inline string trim(const string& s) {
    size_t l = 0, r = s.size();
    while (l < r && isspace(static_cast<unsigned char>(s[l]))) ++l;
    while (r > l && isspace(static_cast<unsigned char>(s[r - 1]))) --r;
    return s.substr(l, r - l);
}

static vector<string> splitCsvSimple(const string& line) {
    vector<string> cols;
    string cur;
    stringstream ss(line);
    while (getline(ss, cur, ',')) cols.push_back(trim(cur));
    return cols;
}

static unordered_map<string, BenchResult> loadSotaMap(const string& csv_path) {
    unordered_map<string, BenchResult> mp;
    ifstream in(csv_path);
    if (!in.is_open()) return mp;

    string header_line;
    if (!getline(in, header_line)) return mp;
    vector<string> header = splitCsvSimple(header_line);

    int idx_dataset = -1, idx_algo = -1, idx_data_mb = -1, idx_bpe = -1, idx_correct = -1;
    for (int i = 0; i < static_cast<int>(header.size()); ++i) {
        if (header[i] == "Dataset") idx_dataset = i;
        else if (header[i] == "Algorithm") idx_algo = i;
        else if (header[i] == "Data_MB") idx_data_mb = i;
        else if (header[i] == "BPE") idx_bpe = i;
        else if (header[i] == "Correct") idx_correct = i;
    }

    if (idx_dataset < 0 || idx_bpe < 0) return mp;

    string line;
    while (getline(in, line)) {
        if (line.empty()) continue;
        vector<string> cols = splitCsvSimple(line);
        if (idx_dataset >= static_cast<int>(cols.size())) continue;

        string dataset = cols[idx_dataset];
        string algo = (idx_algo >= 0 && idx_algo < static_cast<int>(cols.size())) ? cols[idx_algo] : "SOTA";
        if (algo.find("SOTA") == string::npos && algo.find("sota") == string::npos) continue;

        double data_mb = numeric_limits<double>::quiet_NaN();
        double bpe = numeric_limits<double>::quiet_NaN();
        int correct = -1;

        if (idx_data_mb >= 0 && idx_data_mb < static_cast<int>(cols.size()) && !cols[idx_data_mb].empty()) {
            try { data_mb = stod(cols[idx_data_mb]); } catch (...) {}
        }
        if (idx_bpe >= 0 && idx_bpe < static_cast<int>(cols.size()) && !cols[idx_bpe].empty()) {
            try { bpe = stod(cols[idx_bpe]); } catch (...) {}
        }
        if (idx_correct >= 0 && idx_correct < static_cast<int>(cols.size()) && !cols[idx_correct].empty()) {
            correct = (cols[idx_correct] == "1" || cols[idx_correct] == "YES" || cols[idx_correct] == "yes") ? 1 : 0;
        }

        mp[dataset] = {dataset, "SOTA", data_mb, bpe, correct};
    }

    return mp;
}

static inline string formatDoubleOrBlank(double x, int precision) {
    if (std::isnan(x)) return "";
    ostringstream oss;
    oss << fixed << setprecision(precision) << x;
    return oss.str();
}

static inline string formatCorrect(int c) {
    if (c == 1) return "YES";
    if (c == 0) return "NO";
    return "";
}

vector<BenchResult> mergeWithSotaRows(const vector<BenchResult>& results, const string& sota_csv_path) {
    vector<BenchResult> merged;
    if (results.empty()) return merged;

    unordered_map<string, BenchResult> sota_map = loadSotaMap(sota_csv_path);
    set<string> seen_dataset;

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        if (r.name == "Uncompressed (32-bit)") {
            merged.push_back(r);
            if (seen_dataset.insert(r.dataset).second) {
                auto it = sota_map.find(r.dataset);
                if (it != sota_map.end()) {
                    merged.push_back(it->second);
                } else {
                    merged.push_back({
                        r.dataset,
                        "SOTA",
                        numeric_limits<double>::quiet_NaN(),
                        numeric_limits<double>::quiet_NaN(),
                        -1
                    });
                }
            }
            continue;
        }

        merged.push_back(r);

        if (r.name == "Your Algorithm (Tree+VB)" && seen_dataset.insert(r.dataset).second) {
            auto it = sota_map.find(r.dataset);
            if (it != sota_map.end()) {
                merged.push_back(it->second);
            } else {
                merged.push_back({
                    r.dataset,
                    "SOTA",
                    numeric_limits<double>::quiet_NaN(),
                    numeric_limits<double>::quiet_NaN(),
                    -1
                });
            }
        }
    }

    return merged;
}

void printSummaryTable(const vector<BenchResult>& results) {
    if (results.empty()) {
        cout << "No benchmark results to display.\n";
        return;
    }

    cout << "\n============================== Benchmark Summary ==============================\n";
    cout << left << setw(26) << "Dataset"
         << setw(32) << "Algorithm"
         << right << setw(14) << "Data_MB"
         << setw(14) << "BPE"
         << setw(12) << "Ratio(%)"
         << setw(10) << "Correct"
         << "\n";
    cout << "--------------------------------------------------------------------------------------------\n";

    vector<double> baseline_mb(results.size(), 0.0);
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].name == "Uncompressed (32-bit)") {
            for (size_t j = 0; j < results.size(); ++j) {
                if (results[j].dataset == results[i].dataset) {
                    baseline_mb[j] = results[i].data_mb;
                }
            }
        }
    }

    vector<double> baseline_bpe(results.size(), 0.0);
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].name == "Uncompressed (32-bit)") {
            for (size_t j = 0; j < results.size(); ++j) {
                if (results[j].dataset == results[i].dataset) {
                    baseline_bpe[j] = results[i].bpe;
                }
            }
        }
    }

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        if (i > 0 && r.dataset != results[i - 1].dataset) {
            cout << "--------------------------------------------------------------------------------------------\n";
        }

        double ratio = numeric_limits<double>::quiet_NaN();
        if (!std::isnan(r.bpe) && baseline_bpe[i] > 0.0) {
            ratio = (r.bpe / baseline_bpe[i]) * 100.0;
        } else if (!std::isnan(r.data_mb) && baseline_mb[i] > 0.0) {
            ratio = (r.data_mb / baseline_mb[i]) * 100.0;
        }

        string data_mb_str = formatDoubleOrBlank(r.data_mb, 4);
        string bpe_str = formatDoubleOrBlank(r.bpe, 4);
        string ratio_str = formatDoubleOrBlank(ratio, 2);
        string correct_str = formatCorrect(r.correct);

        cout << left << setw(26) << r.dataset
             << setw(32) << r.name
             << right << setw(14) << data_mb_str
             << setw(14) << bpe_str
             << setw(12) << ratio_str
             << setw(10) << correct_str
             << "\n";
    }
    cout << "============================================================================================\n";
}

// ============================================================================
// (Your core classes are kept unchanged: GraphLoader, IndustrialGraphCompressor)
// For brevity, the class definitions are folded here.
// Please paste the exact same class definitions from the previous version.
// ============================================================================
class GraphLoader {
public:
    uint32_t num_nodes = 0, num_edges = 0;
    vector<uint32_t> row_ptr, col_idx;
    bool loadFromText(const string& filename) {
        // Pass-1: count max node id and per-node degree without storing all edges.
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

        if (!has_edge) {
            num_nodes = 0;
            num_edges = 0;
            row_ptr.assign(1, 0);
            col_idx.clear();
            return true;
        }

        num_nodes = max_node_id + 1;
        row_ptr.assign(static_cast<size_t>(num_nodes) + 1, 0);
        for (uint32_t i = 0; i < num_nodes; ++i) row_ptr[i + 1] = row_ptr[i] + degree[i];

        col_idx.assign(static_cast<size_t>(row_ptr[num_nodes]), 0);
        vector<uint32_t> cursor = row_ptr;

        // Pass-2: refill CSR using the precomputed offsets.
        infile.open(filename);
        if (!infile.is_open()) { cerr << "[ERROR] File not found: " << filename << "\n"; return false; }
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

        // Sort + deduplicate each row and compact CSR.
        vector<uint32_t> compact_col;
        compact_col.reserve(col_idx.size());
        vector<uint32_t> new_row_ptr(static_cast<size_t>(num_nodes) + 1, 0);

        for (uint32_t i = 0; i < num_nodes; ++i) {
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (start == end) {
                new_row_ptr[i + 1] = static_cast<uint32_t>(compact_col.size());
                continue;
            }

            sort(col_idx.begin() + start, col_idx.begin() + end);
            uint32_t prev = col_idx[start];
            compact_col.push_back(prev);
            for (uint32_t j = start + 1; j < end; ++j) {
                uint32_t x = col_idx[j];
                if (x != prev) {
                    compact_col.push_back(x);
                    prev = x;
                }
            }
            new_row_ptr[i + 1] = static_cast<uint32_t>(compact_col.size());
        }

        row_ptr.swap(new_row_ptr);
        col_idx.swap(compact_col);
        num_edges = static_cast<uint32_t>(col_idx.size());
        return true;
    }
};

BenchResult runUncompressedBaseline(const string& dataset, uint32_t num_nodes, uint32_t num_edges, 
                                    const vector<uint32_t>& row_ptr, const vector<uint32_t>& col_idx) {
    volatile uint32_t dummy_sum = 0;
    for (uint32_t i = 0; i < num_nodes; ++i) {
        for (uint32_t j = row_ptr[i]; j < row_ptr[i+1]; ++j) dummy_sum += col_idx[j]; 
    }
    return {dataset, "Uncompressed (32-bit)", (num_edges * 4.0) / 1048576.0, 32.0, true};
}

class IndustrialGraphCompressor {
    uint32_t num_nodes, num_edges; const vector<uint32_t>& row_ptr; vector<uint32_t> col_idx; // Use a local copy to avoid mutating the original input data
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
        return {dataset, "Your Algorithm (Tree+VB)", byte_stream.size() / 1048576.0, (byte_stream.size() * 8.0) / num_edges, decoded_edges == num_edges};
    }
};

// 按照节点度数（频率）降序进行 ReID 重排序
void reorderGraphByDegree(uint32_t num_nodes, vector<uint32_t>& row_ptr, vector<uint32_t>& col_idx) {
    // 1. 统计每个节点的度数 (Degree)
    vector<pair<uint32_t, uint32_t>> deg_node(num_nodes);
    for (uint32_t i = 0; i < num_nodes; ++i) {
        deg_node[i] = {row_ptr[i+1] - row_ptr[i], i};
    }

    // 2. 按度数从大到小排序
    sort(deg_node.begin(), deg_node.end(), [](const pair<uint32_t, uint32_t>& a, const pair<uint32_t, uint32_t>& b){
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
        for (uint32_t j = row_ptr[old_id]; j < row_ptr[old_id+1]; ++j) {
            new_adj[new_id].push_back(old_to_new[col_idx[j]]);
        }
    }

    // 5. 将重建后的数据写回 row_ptr 和 col_idx，并严格升序排列
    uint32_t edge_cnt = 0;
    for (uint32_t i = 0; i < num_nodes; ++i) {
        row_ptr[i] = edge_cnt;
        // 这里的排序非常关键，保证了新图的邻接表是严格升序的
        sort(new_adj[i].begin(), new_adj[i].end()); 
        for (uint32_t neighbor : new_adj[i]) {
            col_idx[edge_cnt++] = neighbor;
        }
    }
    row_ptr[num_nodes] = edge_cnt;
}


// ============================================================================
// Main function: supports batch processing and CSV export
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: ./benchmark graph1.edges graph2.edges graph3.edges ...\n";
        return -1;
    }

    vector<BenchResult> all_results;

    // Process each input graph file
    for (int i = 1; i < argc; ++i) {
        string filepath = argv[i];
        string dataset_name = getBaseName(filepath);
        cout << "\n========================================================\n";
        cout << "Start benchmarking dataset: " << dataset_name << "\n";
        
        GraphLoader loader;
        if (!loader.loadFromText(filepath)) continue;

        // Run two algorithms and append results into all_results
        all_results.push_back(runUncompressedBaseline(dataset_name, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx));
        
        reorderGraphByDegree(loader.num_nodes, loader.row_ptr, loader.col_idx);
        IndustrialGraphCompressor your_comp(dataset_name, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx);
        all_results.push_back(your_comp.run());
    }

    // Merge optional SOTA rows (one row per dataset, empty if missing).
    const string output_dir = "../result";
    const string sota_csv = output_dir + "/vssota.csv";
    vector<BenchResult> final_results = mergeWithSotaRows(all_results, sota_csv);

    // Print the full summary table in terminal.
    printSummaryTable(final_results);

    // Export CSV into project-level result directory.
#ifdef _WIN32
    _mkdir(output_dir.c_str());
#else
    mkdir(output_dir.c_str(), 0755);
#endif
    const string output_csv = output_dir + "/benchmark_results.csv";

    ofstream csv(output_csv);
    csv << "Dataset,Algorithm,Data_MB,BPE,Correct\n";
    for (const auto& r : final_results) {
        csv << r.dataset << "," << r.name << ","
            << formatDoubleOrBlank(r.data_mb, 4) << ","
            << formatDoubleOrBlank(r.bpe, 4) << ",";
        if (r.correct == -1) csv << "";
        else csv << r.correct;
        csv << "\n";
    }
    csv.close();

    cout << "\nAll datasets finished. Results were exported to: " << output_csv << "\n";
    cout << "Run the provided Python script to generate the line chart.\n";
    return 0;
} 