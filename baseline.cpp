#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>
#include <limits>

using namespace std;

const uint32_t NO_PARENT = UINT32_MAX;

// ============================================================================
// 共享工具库：变长字节编码 (VByte)
// ============================================================================
inline void encodeVByte(uint32_t val, vector<uint8_t> &stream)
{
    while (val >= 128)
    {
        stream.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80));
        val >>= 7;
    }
    stream.push_back(static_cast<uint8_t>(val));
}
inline uint32_t decodeVByte(const vector<uint8_t> &stream, size_t &pos)
{
    uint32_t val = 0;
    int shift = 0;
    while (true)
    {
        uint8_t b = stream[pos++];
        val |= (b & 0x7F) << shift;
        if ((b & 0x80) == 0)
            break;
        shift += 7;
    }
    return val;
}

// 只计算一个数用 VByte 编码后占多少字节（用于收益估计）
inline uint32_t vbyteLen(uint32_t val)
{
    uint32_t len = 1;
    while (val >= 128)
    {
        val >>= 7;
        ++len;
    }
    return len;
}

// 为了避免小图测速出现 0ms 导致 inf，这里统一采用最短测量窗口。
template <typename DecodeRoundFn>
double benchmarkDecodeMEPS(uint64_t edges_per_round, DecodeRoundFn decode_round, double min_ms = 20.0)
{
    if (edges_per_round == 0)
        return 0.0;
    uint64_t rounds = 0;
    auto start = chrono::high_resolution_clock::now();
    double elapsed_ms = 0.0;
    do
    {
        decode_round();
        ++rounds;
        auto now = chrono::high_resolution_clock::now();
        elapsed_ms = chrono::duration<double, std::milli>(now - start).count();
    } while (elapsed_ms < min_ms);

    double seconds = elapsed_ms / 1000.0;
    if (seconds <= 0.0)
        return 0.0;
    return (static_cast<double>(edges_per_round) * static_cast<double>(rounds) / seconds) / 1000000.0;
}

// 提取文件名
string getBaseName(const string &path)
{
    size_t last_slash = path.find_last_of("/\\");
    size_t last_dot = path.find_last_of(".");
    size_t start = (last_slash == string::npos) ? 0 : last_slash + 1;
    size_t end = (last_dot == string::npos) ? path.length() : last_dot;
    return path.substr(start, end - start);
}

// 评估结果结构体
struct BenchResult
{
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
class GraphLoader
{
public:
    uint32_t num_nodes = 0, num_edges = 0;
    vector<uint32_t> row_ptr, col_idx;

    static inline bool parseEdgeLine(const string &line, uint32_t &u, uint32_t &v)
    {
        if (line.empty() || line[0] == '#' || line[0] == '%')
            return false;
        string s = line;
        replace(s.begin(), s.end(), ',', ' ');
        stringstream ss(s);
        if (!(ss >> u >> v))
            return false;
        if (u == v)
            return false;
        return true;
    }

    bool loadFromText(const string &filename)
    {
        // Pass-1: 流式统计节点范围与每行度数（不保留全量边）。
        ifstream infile1(filename);
        if (!infile1.is_open())
        {
            cerr << "[ERROR] File not found: " << filename << "\n";
            return false;
        }

        string line;
        uint32_t max_node_id = 0;
        vector<uint32_t> degree;
        uint64_t directed_edges = 0;

        while (getline(infile1, line))
        {
            uint32_t u, v;
            if (!parseEdgeLine(line, u, v))
                continue;

            max_node_id = max(max_node_id, max(u, v));
            if (degree.size() <= max_node_id)
                degree.resize(static_cast<size_t>(max_node_id) + 1, 0);

            degree[u]++;
            degree[v]++;
            directed_edges += 2;
        }
        infile1.close();

        if (degree.empty())
        {
            num_nodes = 0;
            num_edges = 0;
            row_ptr.assign(1, 0);
            col_idx.clear();
            return true;
        }

        num_nodes = max_node_id + 1;
        row_ptr.assign(static_cast<size_t>(num_nodes) + 1, 0);
        for (uint32_t i = 0; i < num_nodes; ++i)
            row_ptr[i + 1] = row_ptr[i] + degree[i];

        col_idx.assign(static_cast<size_t>(row_ptr[num_nodes]), 0);
        vector<uint32_t> cursor = row_ptr;

        // Pass-2: 流式填充 CSR 邻接数组。
        ifstream infile2(filename);
        if (!infile2.is_open())
        {
            cerr << "[ERROR] File not found: " << filename << "\n";
            return false;
        }
        while (getline(infile2, line))
        {
            uint32_t u, v;
            if (!parseEdgeLine(line, u, v))
                continue;

            col_idx[cursor[u]++] = v;
            col_idx[cursor[v]++] = u;
        }
        infile2.close();

        // 行内排序去重，压紧 CSR。
        vector<uint32_t> compact_col;
        compact_col.reserve(static_cast<size_t>(directed_edges));
        vector<uint32_t> new_row_ptr(static_cast<size_t>(num_nodes) + 1, 0);

        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (start == end)
            {
                new_row_ptr[i + 1] = static_cast<uint32_t>(compact_col.size());
                continue;
            }

            sort(col_idx.begin() + start, col_idx.begin() + end);
            uint32_t prev = col_idx[start];
            compact_col.push_back(prev);
            for (uint32_t j = start + 1; j < end; ++j)
            {
                uint32_t x = col_idx[j];
                if (x != prev)
                {
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

BenchResult runUncompressedBaseline(const string &dataset, uint32_t num_nodes, uint32_t num_edges,
                                    const vector<uint32_t> &row_ptr, const vector<uint32_t> &col_idx)
{
    auto decode_round = [&]()
    {
        volatile uint32_t sink = 0;
        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            for (uint32_t j = row_ptr[i]; j < row_ptr[i + 1]; ++j)
                sink += col_idx[j];
        }
        (void)sink;
    };
    double meps = benchmarkDecodeMEPS(num_edges, decode_round);
    return {dataset, "Uncompressed (32-bit)", (num_edges * 4.0) / 1048576.0, 32.0, meps, true};
}

class DeltaVByteCompressor
{
    uint32_t num_nodes, num_edges;
    const vector<uint32_t> &row_ptr;
    const vector<uint32_t> &col_idx;
    vector<uint32_t> byte_ptr;
    vector<uint8_t> byte_stream;
    string dataset;

public:
    DeltaVByteCompressor(string ds, uint32_t n, uint32_t e, const vector<uint32_t> &r, const vector<uint32_t> &c)
        : dataset(ds), num_nodes(n), num_edges(e), row_ptr(r), col_idx(c) {}
    BenchResult run()
    {
        byte_ptr.assign(num_nodes + 1, 0);
        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            byte_ptr[i] = byte_stream.size();
            uint32_t prev = 0;
            for (uint32_t j = row_ptr[i]; j < row_ptr[i + 1]; ++j)
            {
                uint32_t curr = col_idx[j];
                encodeVByte(curr - prev, byte_stream);
                prev = curr;
            }
        }
        byte_ptr[num_nodes] = byte_stream.size();

        // 先做一次严格边数校验
        uint32_t decoded_edges = 0;
        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            size_t pos = byte_ptr[i], end_pos = byte_ptr[i + 1];
            uint32_t prev = 0;
            while (pos < end_pos)
            {
                prev = prev + decodeVByte(byte_stream, pos);
                decoded_edges++;
            }
        }

        auto decode_round = [&]()
        {
            volatile uint32_t sink = 0;
            for (uint32_t i = 0; i < num_nodes; ++i)
            {
                size_t pos = byte_ptr[i], end_pos = byte_ptr[i + 1];
                uint32_t prev = 0;
                while (pos < end_pos)
                {
                    prev = prev + decodeVByte(byte_stream, pos);
                    sink ^= prev;
                }
            }
            (void)sink;
        };
        double meps = benchmarkDecodeMEPS(num_edges, decode_round);
        return {dataset, "Delta + VByte", byte_stream.size() / 1048576.0, (byte_stream.size() * 8.0) / num_edges, meps, decoded_edges == num_edges};
    }
};

class IndustrialGraphCompressor
{
    uint32_t num_nodes, num_edges;
    const vector<uint32_t> &row_ptr;
    vector<uint32_t> col_idx;
    vector<uint32_t> freq, parent, byte_ptr;
    vector<bool> in_list, is_packed;
    uint32_t max_depth;
    vector<uint8_t> byte_stream;
    string dataset;

    struct TempCmd
    {
        uint32_t bottom, steps;
        bool is_path;
    };

public:
    IndustrialGraphCompressor(string ds, uint32_t n, uint32_t e, const vector<uint32_t> &r, const vector<uint32_t> &c)
        : dataset(ds), num_nodes(n), num_edges(e), row_ptr(r), col_idx(c), max_depth(127)
    {
        freq.assign(num_nodes, 0);
        parent.assign(num_nodes, NO_PARENT);
        in_list.assign(num_nodes, false);
        is_packed.assign(num_nodes, false);
        byte_ptr.assign(num_nodes + 1, 0);
    }

    BenchResult run()
    {
        for (uint32_t i = 0; i < num_edges; ++i)
            freq[col_idx[i]]++;

        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (end - start < 2)
                continue;
            sort(col_idx.begin() + start, col_idx.begin() + end, [&](uint32_t a, uint32_t b)
                 {
                if (freq[a] != freq[b]) return freq[a] > freq[b];
                return a < b; });
        }

        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (end - start < 2)
                continue;
            for (uint32_t j = start; j < end - 1; ++j)
            {
                if (parent[col_idx[j + 1]] == NO_PARENT)
                    parent[col_idx[j + 1]] = col_idx[j];
            }
        }

        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            byte_ptr[i] = byte_stream.size();
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (start == end)
                continue;

            for (uint32_t j = start; j < end; ++j)
            {
                in_list[col_idx[j]] = true;
                is_packed[col_idx[j]] = false;
            }

            vector<TempCmd> temp_cmds;
            for (int j = static_cast<int>(end) - 1; j >= static_cast<int>(start); --j)
            {
                uint32_t bottom = col_idx[j];
                if (is_packed[bottom])
                    continue;

                uint32_t curr = bottom, steps = 0;
                vector<uint32_t> path_nodes = {curr};
                while (parent[curr] != NO_PARENT &&
                       in_list[parent[curr]] &&
                       !is_packed[parent[curr]] &&
                       steps < max_depth)
                {
                    curr = parent[curr];
                    path_nodes.push_back(curr);
                    steps++;
                }

                if (steps >= 1)
                {
                    for (uint32_t node : path_nodes)
                        is_packed[node] = true;
                    temp_cmds.push_back({bottom, steps, true});
                }
                else
                {
                    is_packed[bottom] = true;
                    temp_cmds.push_back({bottom, 0, false});
                }
            }

            for (auto it = temp_cmds.rbegin(); it != temp_cmds.rend(); ++it)
            {
                encodeVByte((it->bottom << 1) | (it->is_path ? 1 : 0), byte_stream);
                if (it->is_path)
                    encodeVByte(it->steps, byte_stream);
            }

            for (uint32_t j = start; j < end; ++j)
                in_list[col_idx[j]] = false;
        }

        byte_ptr[num_nodes] = byte_stream.size();

        uint32_t decoded_edges = 0;
        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            size_t pos = byte_ptr[i], end_pos = byte_ptr[i + 1];
            while (pos < end_pos)
            {
                uint32_t val = decodeVByte(byte_stream, pos);
                uint8_t flag = val & 1;
                uint32_t curr = val >> 1;
                decoded_edges++;
                if (flag == 1)
                {
                    uint32_t steps = decodeVByte(byte_stream, pos);
                    for (uint32_t s = 0; s < steps; ++s)
                    {
                        curr = parent[curr];
                        decoded_edges++;
                    }
                }
            }
        }

        auto decode_round = [&]()
        {
            volatile uint32_t sink = 0;
            for (uint32_t i = 0; i < num_nodes; ++i)
            {
                size_t pos = byte_ptr[i], end_pos = byte_ptr[i + 1];
                while (pos < end_pos)
                {
                    uint32_t val = decodeVByte(byte_stream, pos);
                    uint8_t flag = val & 1;
                    uint32_t curr = val >> 1;
                    sink ^= curr;
                    if (flag == 1)
                    {
                        uint32_t steps = decodeVByte(byte_stream, pos);
                        for (uint32_t s = 0; s < steps; ++s)
                        {
                            curr = parent[curr];
                            sink ^= curr;
                        }
                    }
                }
            }
            (void)sink;
        };

        double meps = benchmarkDecodeMEPS(num_edges, decode_round);
        return {dataset,
                "Your Algorithm (Tree+VB)",
                byte_stream.size() / 1048576.0,
                (byte_stream.size() * 8.0) / num_edges,
                meps,
                decoded_edges == num_edges};
    }
};

class IndustrialGraphCompressorSACT
{
    uint32_t num_nodes, num_edges;
    const vector<uint32_t> &row_ptr;
    vector<uint32_t> col_idx;
    vector<uint32_t> freq, parent, byte_ptr;
    vector<uint32_t> row_mark, packed_mark, bottom_mark, bottom_steps, greedy_mark, owner_mark;
    vector<int32_t> owner_idx;
    vector<uint8_t> byte_stream;
    uint32_t max_depth;
    string dataset;

    struct TempCmd
    {
        uint32_t bottom;
        uint32_t steps;
        bool is_path;
    };

    inline uint32_t singleCmdBytes(uint32_t node) const
    {
        return vbyteLen((node << 1) | 0);
    }

    inline uint32_t pathCmdBytes(uint32_t bottom, uint32_t steps) const
    {
        return vbyteLen((bottom << 1) | 1) + vbyteLen(steps);
    }

    // 阶段 B：码长感知重编号。
    // 用频次 + 父候选贡献 + 底节点贡献构造分数，让高贡献节点优先分配更短 VByte ID。
    void applyCodeLengthAwareRenumbering()
    {
        vector<uint32_t> base_freq(num_nodes, 0);
        vector<uint32_t> parent_potential(num_nodes, 0);
        vector<uint32_t> bottom_potential(num_nodes, 0);

        for (uint32_t i = 0; i < num_edges; ++i)
            base_freq[col_idx[i]]++;

        vector<uint32_t> local;
        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (end - start < 2)
                continue;

            local.assign(col_idx.begin() + start, col_idx.begin() + end);
            sort(local.begin(), local.end(), [&](uint32_t a, uint32_t b)
                 {
                if (base_freq[a] != base_freq[b]) return base_freq[a] > base_freq[b];
                return a < b; });

            for (uint32_t j = 0; j + 1 < local.size(); ++j)
            {
                parent_potential[local[j]]++;
                bottom_potential[local[j + 1]]++;
            }
        }

        const double alpha = 1.00;
        const double beta = 0.75;
        const double gamma = 0.50;

        vector<double> score(num_nodes, 0.0);
        vector<uint32_t> order(num_nodes, 0);
        for (uint32_t u = 0; u < num_nodes; ++u)
        {
            score[u] = alpha * static_cast<double>(base_freq[u]) +
                       beta * static_cast<double>(parent_potential[u]) +
                       gamma * static_cast<double>(bottom_potential[u]);
            order[u] = u;
        }

        sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b)
             {
            if (score[a] != score[b]) return score[a] > score[b];
            if (base_freq[a] != base_freq[b]) return base_freq[a] > base_freq[b];
            return a < b; });

        vector<uint32_t> old_to_new(num_nodes, 0);
        for (uint32_t new_id = 0; new_id < num_nodes; ++new_id)
            old_to_new[order[new_id]] = new_id;

        for (uint32_t i = 0; i < num_edges; ++i)
            col_idx[i] = old_to_new[col_idx[i]];
    }

    void buildFrequency()
    {
        for (uint32_t i = 0; i < num_edges; ++i)
            freq[col_idx[i]]++;
    }

    void sortRowsByFrequency()
    {
        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (end - start < 2)
                continue;

            sort(col_idx.begin() + start, col_idx.begin() + end, [&](uint32_t a, uint32_t b)
                 {
                if (freq[a] != freq[b]) return freq[a] > freq[b];
                return a < b; });
        }
    }

    void buildParentForestFirstFit()
    {
        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (end - start < 2)
                continue;

            for (uint32_t j = start; j < end - 1; ++j)
            {
                uint32_t p = col_idx[j];
                uint32_t c = col_idx[j + 1];
                if (parent[c] == NO_PARENT)
                    parent[c] = p;
            }
        }
    }

    bool buildParentForestGainDriven()
    {
        const uint32_t TOP_K = 4;
        vector<uint32_t> first_fit_parent(num_nodes, NO_PARENT);
        vector<uint32_t> cand_parent(num_nodes * TOP_K, NO_PARENT);
        vector<double> cand_score(num_nodes * TOP_K, 0.0);
        vector<uint32_t> cand_support(num_nodes * TOP_K, 0);

        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (end - start < 2)
                continue;

            for (uint32_t j = start; j + 1 < end; ++j)
            {
                uint32_t p = col_idx[j];
                uint32_t c = col_idx[j + 1];

                if (first_fit_parent[c] == NO_PARENT)
                    first_fit_parent[c] = p;

                uint32_t single_cost = singleCmdBytes(c) + singleCmdBytes(p);
                uint32_t path_cost = pathCmdBytes(c, 1);
                int32_t unit_gain = static_cast<int32_t>(single_cost) - static_cast<int32_t>(path_cost);
                if (unit_gain <= 0)
                    continue;

                double stability = 0.0;
                if (freq[c] != 0)
                    stability = static_cast<double>(freq[p]) / static_cast<double>(freq[c]);

                double id_penalty = static_cast<double>(vbyteLen((c << 1) | 1) - 1);
                double local_score = static_cast<double>(unit_gain) + 0.20 * stability - 0.05 * id_penalty;

                uint32_t base = c * TOP_K;
                int32_t slot = -1;
                int32_t empty_slot = -1;
                int32_t worst_slot = 0;

                for (uint32_t k = 0; k < TOP_K; ++k)
                {
                    uint32_t idx = base + k;
                    if (cand_parent[idx] == p)
                    {
                        slot = static_cast<int32_t>(idx);
                        break;
                    }
                    if (cand_parent[idx] == NO_PARENT && empty_slot < 0)
                        empty_slot = static_cast<int32_t>(idx);

                    if (cand_score[idx] < cand_score[base + static_cast<uint32_t>(worst_slot)])
                        worst_slot = static_cast<int32_t>(k);
                }

                if (slot < 0)
                {
                    if (empty_slot >= 0)
                    {
                        slot = empty_slot;
                    }
                    else
                    {
                        int32_t replace = static_cast<int32_t>(base + static_cast<uint32_t>(worst_slot));
                        if (local_score <= cand_score[replace])
                            continue;
                        slot = replace;
                        cand_parent[slot] = NO_PARENT;
                        cand_score[slot] = 0.0;
                        cand_support[slot] = 0;
                    }

                    cand_parent[slot] = p;
                }

                cand_score[slot] += local_score;
                cand_support[slot] += 1;
            }
        }

        uint32_t switched = 0;
        for (uint32_t c = 0; c < num_nodes; ++c)
        {
            parent[c] = first_fit_parent[c];

            uint32_t base = c * TOP_K;
            int32_t best_slot = -1;
            for (uint32_t k = 0; k < TOP_K; ++k)
            {
                uint32_t idx = base + k;
                if (cand_parent[idx] == NO_PARENT)
                    continue;

                if (best_slot < 0)
                {
                    best_slot = static_cast<int32_t>(idx);
                    continue;
                }

                uint32_t best_idx = static_cast<uint32_t>(best_slot);
                if (cand_score[idx] > cand_score[best_idx] ||
                    (cand_score[idx] == cand_score[best_idx] && cand_support[idx] > cand_support[best_idx]) ||
                    (cand_score[idx] == cand_score[best_idx] && cand_support[idx] == cand_support[best_idx] && cand_parent[idx] < cand_parent[best_idx]))
                {
                    best_slot = static_cast<int32_t>(idx);
                }
            }

            if (best_slot >= 0)
            {
                uint32_t idx = static_cast<uint32_t>(best_slot);
                // 只在高置信候选上覆盖 first-fit，避免小图/弱信号回退。
                if (cand_support[idx] >= 2 && cand_score[idx] >= 2.0)
                {
                    if (parent[c] != cand_parent[idx])
                    {
                        parent[c] = cand_parent[idx];
                        ++switched;
                    }
                }
            }
        }

        return switched > 0;
    }

    struct PathCandidate
    {
        uint32_t bottom;
        uint32_t steps;
        int32_t gain;
        vector<uint32_t> nodes;
    };

    void encodeRowsBottomUp()
    {
        fill(row_mark.begin(), row_mark.end(), 0);
        fill(packed_mark.begin(), packed_mark.end(), 0);
        fill(bottom_mark.begin(), bottom_mark.end(), 0);
        fill(bottom_steps.begin(), bottom_steps.end(), 0);
        fill(greedy_mark.begin(), greedy_mark.end(), 0);
        fill(owner_mark.begin(), owner_mark.end(), 0);

        uint32_t marker = 1;

        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            byte_ptr[i] = byte_stream.size();
            uint32_t start = row_ptr[i], end = row_ptr[i + 1];
            if (start == end)
                continue;

            if (marker == 0)
            {
                fill(row_mark.begin(), row_mark.end(), 0);
                fill(packed_mark.begin(), packed_mark.end(), 0);
                fill(bottom_mark.begin(), bottom_mark.end(), 0);
                fill(greedy_mark.begin(), greedy_mark.end(), 0);
                fill(owner_mark.begin(), owner_mark.end(), 0);
                marker = 1;
            }

            for (uint32_t j = start; j < end; ++j)
            {
                uint32_t node = col_idx[j];
                row_mark[node] = marker;
            }

            vector<PathCandidate> candidates;
            candidates.reserve(end - start);
            for (int j = static_cast<int>(end) - 1; j >= static_cast<int>(start); --j)
            {
                uint32_t bottom = col_idx[j];

                vector<uint32_t> chain;
                chain.reserve(16);
                chain.push_back(bottom);

                uint32_t curr = bottom;
                uint32_t steps = 0;
                uint32_t single_cost = singleCmdBytes(bottom);
                int32_t best_gain = 0;
                uint32_t best_steps = 0;
                uint32_t negative_streak = 0;

                while (steps < max_depth &&
                       parent[curr] != NO_PARENT &&
                       row_mark[parent[curr]] == marker &&
                       packed_mark[parent[curr]] != marker)
                {
                    curr = parent[curr];
                    chain.push_back(curr);
                    ++steps;

                    single_cost += singleCmdBytes(curr);
                    uint32_t path_cost = pathCmdBytes(bottom, steps);
                    int32_t gain = static_cast<int32_t>(single_cost) - static_cast<int32_t>(path_cost);

                    if (gain > best_gain)
                    {
                        best_gain = gain;
                        best_steps = steps;
                        negative_streak = 0;
                    }
                    else if (gain < 0)
                    {
                        ++negative_streak;
                        if (best_gain > 0 && negative_streak >= 3)
                            break;
                    }
                    else
                    {
                        negative_streak = 0;
                    }
                }

                if (best_steps > 0 && best_gain > 0)
                {
                    vector<uint32_t> nodes(chain.begin(), chain.begin() + best_steps + 1);
                    candidates.push_back({bottom, best_steps, best_gain, std::move(nodes)});
                }
            }

            sort(candidates.begin(), candidates.end(), [](const PathCandidate &a, const PathCandidate &b)
                 {
                if (a.gain != b.gain) return a.gain > b.gain;
                if (a.steps != b.steps) return a.steps > b.steps;
                return a.bottom < b.bottom; });

            struct SelectedPath
            {
                uint32_t bottom;
                uint32_t steps;
                int32_t gain;
                vector<uint32_t> nodes;
            };

            vector<SelectedPath> selected;
            selected.reserve(candidates.size());

            for (const auto &cand : candidates)
            {
                bool conflict = false;
                for (uint32_t node : cand.nodes)
                {
                    if (packed_mark[node] == marker)
                    {
                        conflict = true;
                        break;
                    }
                }
                if (conflict)
                    continue;

                int32_t sel_idx = static_cast<int32_t>(selected.size());
                selected.push_back({cand.bottom, cand.steps, cand.gain, cand.nodes});
                for (uint32_t node : cand.nodes)
                {
                    packed_mark[node] = marker;
                    owner_mark[node] = marker;
                    owner_idx[node] = sel_idx;
                }
                bottom_mark[cand.bottom] = marker;
                bottom_steps[cand.bottom] = cand.steps;
            }

            auto rebuild_selected_state = [&]()
            {
                for (uint32_t j = start; j < end; ++j)
                {
                    uint32_t node = col_idx[j];
                    packed_mark[node] = 0;
                    bottom_mark[node] = 0;
                    bottom_steps[node] = 0;
                    owner_mark[node] = 0;
                }

                for (int32_t idx = 0; idx < static_cast<int32_t>(selected.size()); ++idx)
                {
                    const auto &sel = selected[idx];
                    for (uint32_t node : sel.nodes)
                    {
                        packed_mark[node] = marker;
                        owner_mark[node] = marker;
                        owner_idx[node] = idx;
                    }
                    bottom_mark[sel.bottom] = marker;
                    bottom_steps[sel.bottom] = sel.steps;
                }
            };

            // 受限 1-exchange：若一个未选候选仅与一个已选路径冲突，且收益更高，则替换。
            for (const auto &cand : candidates)
            {
                if (bottom_mark[cand.bottom] == marker && bottom_steps[cand.bottom] == cand.steps)
                    continue;

                vector<int32_t> conflicts;
                conflicts.reserve(2);
                for (uint32_t node : cand.nodes)
                {
                    if (owner_mark[node] != marker)
                        continue;

                    int32_t idx = owner_idx[node];
                    bool seen = false;
                    for (int32_t cidx : conflicts)
                    {
                        if (cidx == idx)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                        conflicts.push_back(idx);

                    if (conflicts.size() > 1)
                        break;
                }

                if (conflicts.size() == 1)
                {
                    int32_t idx = conflicts[0];
                    if (idx >= 0 && idx < static_cast<int32_t>(selected.size()) && cand.gain > selected[idx].gain)
                    {
                        selected[idx] = {cand.bottom, cand.steps, cand.gain, cand.nodes};
                        rebuild_selected_state();
                    }
                }
            }

            // 受限 2-exchange：两个未选候选替换两个已选路径，仅在净收益提升时执行。
            // 为控制复杂度，仅在中等行长上启用且最多做两轮替换。
            // A/B 结果显示 2-exchange 对 BPE 无增益且拖慢速度，默认关闭。
            const bool enable_two_exchange = false;
            const uint32_t two_exchange_max_row_len = 2048;
            if (enable_two_exchange &&
                selected.size() >= 2 &&
                (end - start) <= two_exchange_max_row_len)
            {
                vector<int32_t> cand_conf_a(candidates.size(), -1);
                vector<int32_t> cand_conf_b(candidates.size(), -1);
                vector<uint8_t> cand_valid(candidates.size(), 0);

                for (uint32_t ci = 0; ci < candidates.size(); ++ci)
                {
                    const auto &cand = candidates[ci];
                    if (bottom_mark[cand.bottom] == marker && bottom_steps[cand.bottom] == cand.steps)
                        continue;

                    int32_t c0 = -1, c1 = -1;
                    bool overflow = false;
                    for (uint32_t node : cand.nodes)
                    {
                        if (owner_mark[node] != marker)
                            continue;

                        int32_t idx = owner_idx[node];
                        if (idx < 0 || idx >= static_cast<int32_t>(selected.size()))
                        {
                            overflow = true;
                            break;
                        }

                        if (idx == c0 || idx == c1)
                            continue;

                        if (c0 < 0)
                            c0 = idx;
                        else if (c1 < 0)
                            c1 = idx;
                        else
                        {
                            overflow = true;
                            break;
                        }
                    }

                    if (overflow || c0 < 0 || c1 < 0)
                        continue;

                    if (c0 > c1)
                        std::swap(c0, c1);
                    cand_conf_a[ci] = c0;
                    cand_conf_b[ci] = c1;
                    cand_valid[ci] = 1;
                }

                for (uint32_t round = 0; round < 2; ++round)
                {
                    int32_t best_i = -1, best_j = -1;
                    int32_t best_s0 = -1, best_s1 = -1;
                    int64_t best_delta = 0;

                    for (uint32_t i_cand = 0; i_cand < candidates.size(); ++i_cand)
                    {
                        if (!cand_valid[i_cand])
                            continue;
                        const auto &a = candidates[i_cand];
                        if (bottom_mark[a.bottom] == marker && bottom_steps[a.bottom] == a.steps)
                            continue;

                        for (uint32_t j_cand = i_cand + 1; j_cand < candidates.size(); ++j_cand)
                        {
                            if (!cand_valid[j_cand])
                                continue;
                            const auto &b = candidates[j_cand];
                            if (bottom_mark[b.bottom] == marker && bottom_steps[b.bottom] == b.steps)
                                continue;

                            int32_t a0 = cand_conf_a[i_cand], a1 = cand_conf_b[i_cand];
                            int32_t b0 = cand_conf_a[j_cand], b1 = cand_conf_b[j_cand];

                            int32_t s0 = -1, s1 = -1;
                            auto add_idx = [&](int32_t x)
                            {
                                if (s0 < 0)
                                {
                                    s0 = x;
                                    return true;
                                }
                                if (x == s0)
                                    return true;
                                if (s1 < 0)
                                {
                                    s1 = x;
                                    return true;
                                }
                                return x == s1;
                            };

                            if (!add_idx(a0) || !add_idx(a1) || !add_idx(b0) || !add_idx(b1))
                                continue;
                            if (s0 < 0 || s1 < 0)
                                continue;

                            // 候选路径之间不得重叠。
                            bool overlap = false;
                            for (uint32_t na : a.nodes)
                            {
                                for (uint32_t nb : b.nodes)
                                {
                                    if (na == nb)
                                    {
                                        overlap = true;
                                        break;
                                    }
                                }
                                if (overlap)
                                    break;
                            }
                            if (overlap)
                                continue;

                            int64_t old_gain = static_cast<int64_t>(selected[s0].gain) + static_cast<int64_t>(selected[s1].gain);
                            int64_t new_gain = static_cast<int64_t>(a.gain) + static_cast<int64_t>(b.gain);
                            int64_t delta = new_gain - old_gain;
                            if (delta > best_delta)
                            {
                                best_delta = delta;
                                best_i = static_cast<int32_t>(i_cand);
                                best_j = static_cast<int32_t>(j_cand);
                                if (s0 > s1)
                                    std::swap(s0, s1);
                                best_s0 = s0;
                                best_s1 = s1;
                            }
                        }
                    }

                    if (best_delta <= 0 || best_i < 0 || best_j < 0 || best_s0 < 0 || best_s1 < 0)
                        break;

                    selected[best_s0] = {candidates[best_i].bottom, candidates[best_i].steps, candidates[best_i].gain, candidates[best_i].nodes};
                    selected[best_s1] = {candidates[best_j].bottom, candidates[best_j].steps, candidates[best_j].gain, candidates[best_j].nodes};
                    rebuild_selected_state();
                }
            }

            vector<TempCmd> candidate_cmds;
            candidate_cmds.reserve(end - start);
            for (int j = static_cast<int>(end) - 1; j >= static_cast<int>(start); --j)
            {
                uint32_t node = col_idx[j];
                if (bottom_mark[node] == marker)
                {
                    candidate_cmds.push_back({node, bottom_steps[node], true});
                }
                else if (packed_mark[node] != marker)
                {
                    packed_mark[node] = marker;
                    candidate_cmds.push_back({node, 0, false});
                }
            }

            vector<TempCmd> greedy_cmds;
            greedy_cmds.reserve(end - start);
            for (int j = static_cast<int>(end) - 1; j >= static_cast<int>(start); --j)
            {
                uint32_t bottom = col_idx[j];
                if (greedy_mark[bottom] == marker)
                    continue;

                uint32_t curr = bottom;
                uint32_t steps = 0;
                vector<uint32_t> chain;
                chain.reserve(16);
                chain.push_back(curr);

                while (steps < max_depth &&
                       parent[curr] != NO_PARENT &&
                       row_mark[parent[curr]] == marker &&
                       greedy_mark[parent[curr]] != marker)
                {
                    curr = parent[curr];
                    chain.push_back(curr);
                    ++steps;
                }

                if (steps >= 1)
                {
                    for (uint32_t node : chain)
                        greedy_mark[node] = marker;
                    greedy_cmds.push_back({bottom, steps, true});
                }
                else
                {
                    greedy_mark[bottom] = marker;
                    greedy_cmds.push_back({bottom, 0, false});
                }
            }

            auto estimate_bytes = [&](const vector<TempCmd> &cmds) -> uint32_t
            {
                uint32_t total = 0;
                for (const auto &cmd : cmds)
                {
                    total += vbyteLen((cmd.bottom << 1) | (cmd.is_path ? 1 : 0));
                    if (cmd.is_path)
                        total += vbyteLen(cmd.steps);
                }
                return total;
            };

            const vector<TempCmd> &temp_cmds =
                (estimate_bytes(candidate_cmds) < estimate_bytes(greedy_cmds)) ? candidate_cmds : greedy_cmds;

            for (auto it = temp_cmds.rbegin(); it != temp_cmds.rend(); ++it)
            {
                encodeVByte((it->bottom << 1) | (it->is_path ? 1 : 0), byte_stream);
                if (it->is_path)
                    encodeVByte(it->steps, byte_stream);
            }

            ++marker;
        }

        byte_ptr[num_nodes] = byte_stream.size();
    }

public:
    IndustrialGraphCompressorSACT(string ds, uint32_t n, uint32_t e, const vector<uint32_t> &r, const vector<uint32_t> &c)
        : num_nodes(n), num_edges(e), row_ptr(r), col_idx(c), max_depth(127), dataset(ds)
    {
        freq.assign(num_nodes, 0);
        parent.assign(num_nodes, NO_PARENT);
        byte_ptr.assign(num_nodes + 1, 0);
        row_mark.assign(num_nodes, 0);
        packed_mark.assign(num_nodes, 0);
        bottom_mark.assign(num_nodes, 0);
        bottom_steps.assign(num_nodes, 0);
        greedy_mark.assign(num_nodes, 0);
        owner_mark.assign(num_nodes, 0);
        owner_idx.assign(num_nodes, -1);
    }

    BenchResult run()
    {
        applyCodeLengthAwareRenumbering();
        fill(freq.begin(), freq.end(), 0);
        fill(parent.begin(), parent.end(), NO_PARENT);

        buildFrequency();
        sortRowsByFrequency();
        if (!buildParentForestGainDriven())
            buildParentForestFirstFit();
        encodeRowsBottomUp();

        uint32_t decoded_edges = 0;
        for (uint32_t i = 0; i < num_nodes; ++i)
        {
            size_t pos = byte_ptr[i], end_pos = byte_ptr[i + 1];
            while (pos < end_pos)
            {
                uint32_t val = decodeVByte(byte_stream, pos);
                uint8_t flag = val & 1;
                uint32_t curr = val >> 1;
                decoded_edges++;
                if (flag == 1)
                {
                    uint32_t steps = decodeVByte(byte_stream, pos);
                    for (uint32_t s = 0; s < steps; ++s)
                    {
                        if (parent[curr] == NO_PARENT)
                            break;
                        curr = parent[curr];
                        decoded_edges++;
                    }
                }
            }
        }

        auto decode_round = [&]()
        {
            volatile uint32_t sink = 0;
            for (uint32_t i = 0; i < num_nodes; ++i)
            {
                size_t pos = byte_ptr[i], end_pos = byte_ptr[i + 1];
                while (pos < end_pos)
                {
                    uint32_t val = decodeVByte(byte_stream, pos);
                    uint8_t flag = val & 1;
                    uint32_t curr = val >> 1;
                    sink ^= curr;
                    if (flag == 1)
                    {
                        uint32_t steps = decodeVByte(byte_stream, pos);
                        for (uint32_t s = 0; s < steps; ++s)
                        {
                            if (parent[curr] == NO_PARENT)
                                break;
                            curr = parent[curr];
                            sink ^= curr;
                        }
                    }
                }
            }
            (void)sink;
        };

        double meps = benchmarkDecodeMEPS(num_edges, decode_round);
        return {dataset,
                "Your Algorithm (Tree+VB SACT)",
                byte_stream.size() / 1048576.0,
                (byte_stream.size() * 8.0) / num_edges,
                meps,
                decoded_edges == num_edges};
    }
};

// ============================================================================
// Format and print a per-dataset table with compression summary.
// ============================================================================
void printTableForDataset(const string &dataset_name, const vector<BenchResult> &results)
{
    cout << "\n============================== Performance Report: " << left << setw(20) << dataset_name << " ==============================\n";
    cout << left << setw(40) << "Algorithm"
         << right << setw(14) << "Data Size(MB)"
         << setw(16) << "BPE(bits/edge)"
         << setw(14) << "Ratio (%)"
         << setw(18) << "Speed (MEPS)"
         << setw(10) << "Correct" << "\n";
    cout << "----------------------------------------------------------------------------------------------------\n";

    for (const auto &r : results)
    {
        double ratio = (r.bpe / 32.0) * 100.0;
        cout << left << setw(40) << r.name
             << right << setw(14) << fixed << setprecision(2) << r.data_mb
             << setw(16) << fixed << setprecision(2) << r.bpe
             << setw(13) << fixed << setprecision(2) << ratio << "%"
             << setw(18) << fixed << setprecision(2) << r.meps
             << setw(10) << (r.correct ? "YES" : "NO") << "\n";
    }
    cout << "====================================================================================================\n";
}

// ============================================================================
// Main program flow
// ============================================================================
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        cout << "Usage: ./benchmark graph1.edges graph2.edges graph3.edges ...\n";
        return -1;
    }

    vector<BenchResult> all_results;

    for (int i = 1; i < argc; ++i)
    {
        string filepath = argv[i];
        string dataset_name = getBaseName(filepath);
        cout << "\n[Processing] Dataset: " << dataset_name << " ...\n";

        GraphLoader loader;
        if (!loader.loadFromText(filepath))
            continue;

        vector<BenchResult> current_dataset_results;

        current_dataset_results.push_back(runUncompressedBaseline(dataset_name, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx));

        DeltaVByteCompressor delta_comp(dataset_name, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx);
        current_dataset_results.push_back(delta_comp.run());

        IndustrialGraphCompressor treevb_comp(dataset_name, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx);
        current_dataset_results.push_back(treevb_comp.run());

        IndustrialGraphCompressorSACT sact_comp(dataset_name, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx);
        current_dataset_results.push_back(sact_comp.run());

        printTableForDataset(dataset_name, current_dataset_results);

        all_results.insert(all_results.end(), current_dataset_results.begin(), current_dataset_results.end());
    }

    ofstream csv("benchmark_results.csv");
    csv << "Dataset,Algorithm,Data_MB,BPE,MEPS,Correct\n";
    for (const auto &r : all_results)
    {
        csv << r.dataset << "," << r.name << "," << r.data_mb << "," << r.bpe << "," << r.meps << "," << (r.correct ? 1 : 0) << "\n";
    }
    csv.close();

    cout << "All benchmarks completed. Results have been exported to 'benchmark_results.csv'. Run the Python script to generate plots.\n";
    return 0;
}