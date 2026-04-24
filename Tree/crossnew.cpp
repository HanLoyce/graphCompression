#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>
#include <unordered_set>
#include <unordered_map>
#include <cmath>

using namespace std;

const uint32_t NO_PARENT = UINT32_MAX;

inline void encodeVByte(uint32_t val, vector<uint8_t>& stream) {
	while (val >= 128) {
		stream.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80));
		val >>= 7;
	}
	stream.push_back(static_cast<uint8_t>(val));
}

inline uint32_t decodeVByte(const vector<uint8_t>& stream, size_t& pos) {
	uint32_t val = 0;
	int shift = 0;
	while (true) {
		uint8_t b = stream[pos++];
		val |= (b & 0x7F) << shift;
		if ((b & 0x80) == 0) break;
		shift += 7;
	}
	return val;
}

string getBaseName(const string& path) {
	size_t last_slash = path.find_last_of("/\\");
	size_t last_dot = path.find_last_of(".");
	size_t start = (last_slash == string::npos) ? 0 : last_slash + 1;
	size_t end = (last_dot == string::npos) ? path.length() : last_dot;
	return path.substr(start, end - start);
}

static inline string formatDoubleOrBlank(double x, int precision) {
	if (std::isnan(x)) return "";
	ostringstream oss;
	oss << fixed << setprecision(precision) << x;
	return oss.str();
}

struct BenchResult {
	string dataset;
	string name;
	uint64_t tc_count;
	double tc_time_ms;
	int correct;
};

class GraphLoader {
public:
	uint32_t num_nodes = 0, num_edges = 0;
	vector<uint32_t> row_ptr, col_idx;

	void reorderGraphByDegree() {
		vector<pair<uint32_t, uint32_t>> deg_node(num_nodes);
		for (uint32_t i = 0; i < num_nodes; ++i) {
			deg_node[i] = {row_ptr[i + 1] - row_ptr[i], i};
		}

		sort(deg_node.begin(), deg_node.end(), [](const pair<uint32_t, uint32_t>& a, const pair<uint32_t, uint32_t>& b) {
			if (a.first != b.first) return a.first > b.first;
			return a.second < b.second;
		});

		vector<uint32_t> old_to_new(num_nodes);
		for (uint32_t new_id = 0; new_id < num_nodes; ++new_id) {
			old_to_new[deg_node[new_id].second] = new_id;
		}

		vector<vector<uint32_t>> new_adj(num_nodes);
		for (uint32_t old_id = 0; old_id < num_nodes; ++old_id) {
			uint32_t new_id = old_to_new[old_id];
			for (uint32_t j = row_ptr[old_id]; j < row_ptr[old_id + 1]; ++j) {
				new_adj[new_id].push_back(old_to_new[col_idx[j]]);
			}
		}

		uint32_t edge_cnt = 0;
		for (uint32_t i = 0; i < num_nodes; ++i) {
			row_ptr[i] = edge_cnt;
			sort(new_adj[i].begin(), new_adj[i].end());
			for (uint32_t nei : new_adj[i]) col_idx[edge_cnt++] = nei;
		}
		row_ptr[num_nodes] = edge_cnt;
	}

	bool loadFromText(const string& filename) {
		ifstream infile(filename);
		if (!infile.is_open()) return false;

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
		if (!infile.is_open()) return false;
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

		reorderGraphByDegree();
		return true;
	}
};

BenchResult runUncompressedBaseline(const string& dataset, uint32_t num_nodes, const vector<uint32_t>& row_ptr, const vector<uint32_t>& col_idx) {
	uint64_t tc = 0;
	auto start = chrono::high_resolution_clock::now();

	for (uint32_t u = 0; u < num_nodes; ++u) {
		uint32_t start_u = row_ptr[u], end_u = row_ptr[u + 1];
		for (uint32_t j = start_u; j < end_u; ++j) {
			uint32_t v = col_idx[j];
			if (v <= u) continue;

			uint32_t p_u = start_u, p_v = row_ptr[v], end_v = row_ptr[v + 1];
			while (p_u < end_u && p_v < end_v) {
				if (col_idx[p_u] < col_idx[p_v]) p_u++;
				else if (col_idx[p_u] > col_idx[p_v]) p_v++;
				else {
					if (col_idx[p_u] > v) tc++;
					p_u++;
					p_v++;
				}
			}
		}
	}

	auto end = chrono::high_resolution_clock::now();
	double time_ms = chrono::duration<double, std::milli>(end - start).count();
	return {dataset, "Uncompressed (32-bit)", tc, time_ms, 1};
}

class IndustrialGraphCompressor {
	struct CmdSeg {
		uint32_t bottom;
		uint32_t steps;
		bool is_path;
	};

	uint32_t num_nodes, num_edges;
	const vector<uint32_t>& row_ptr;
	vector<uint32_t> col_idx;
	vector<uint32_t> freq, parent, byte_ptr;
	vector<bool> in_list, is_packed;
	uint32_t max_depth;
	vector<uint8_t> byte_stream;
	string dataset;

	inline void parseRowCommands(uint32_t u, vector<CmdSeg>& cmds) const {
		cmds.clear();
		size_t pos = byte_ptr[u], end_pos = byte_ptr[u + 1];
		while (pos < end_pos) {
			uint32_t val = decodeVByte(byte_stream, pos);
			bool is_path = (val & 1) != 0;
			uint32_t bottom = val >> 1;
			uint32_t steps = 0;
			if (is_path) steps = decodeVByte(byte_stream, pos);
			cmds.push_back({bottom, steps, is_path});
		}
	}

	inline bool pointInPath(uint32_t point, uint32_t bottom, uint32_t steps) const {
		uint32_t cur = bottom;
		if (cur == point) return true;
		for (uint32_t s = 0; s < steps; ++s) {
			if (parent[cur] == NO_PARENT) break;
			cur = parent[cur];
			if (cur == point) return true;
		}
		return false;
	}

	template <typename Fn>
	inline void visitSegmentNodes(const CmdSeg& seg, Fn fn) const {
		uint32_t cur = seg.bottom;
		fn(cur);
		if (!seg.is_path) return;
		for (uint32_t s = 0; s < seg.steps; ++s) {
			if (parent[cur] == NO_PARENT) break;
			cur = parent[cur];
			fn(cur);
		}
	}

	uint64_t triangleCountingDirectOnCompressed(double& out_time_ms) {
		uint64_t tc = 0;
		vector<vector<CmdSeg>> all_cmds(num_nodes);
		for (uint32_t u = 0; u < num_nodes; ++u) {
			parseRowCommands(u, all_cmds[u]);
		}

		vector<uint32_t> mark(num_nodes, 0);
		uint32_t stamp = 1;
		vector<uint32_t> neigh_u;
		neigh_u.reserve(1024);

		auto start = chrono::high_resolution_clock::now();
		for (uint32_t u = 0; u < num_nodes; ++u) {
			if (++stamp == 0) {
				fill(mark.begin(), mark.end(), 0);
				stamp = 1;
			}

			neigh_u.clear();
			for (const auto& seg_u : all_cmds[u]) {
				visitSegmentNodes(seg_u, [&](uint32_t x) {
					mark[x] = stamp;
					if (x > u) neigh_u.push_back(x);
				});
			}

			for (uint32_t v : neigh_u) {
				for (const auto& seg_v : all_cmds[v]) {
					visitSegmentNodes(seg_v, [&](uint32_t x) {
						if (x > v && mark[x] == stamp) tc++;
					});
				}
			}
		}

		auto end = chrono::high_resolution_clock::now();
		out_time_ms = chrono::duration<double, std::milli>(end - start).count();
		return tc;
	}

public:
	IndustrialGraphCompressor(string ds, uint32_t n, uint32_t e, const vector<uint32_t>& r, const vector<uint32_t>& c)
		: num_nodes(n), num_edges(e), row_ptr(r), col_idx(c), max_depth(127), dataset(ds) {
		freq.assign(num_nodes, 0);
		parent.assign(num_nodes, NO_PARENT);
		in_list.assign(num_nodes, false);
		is_packed.assign(num_nodes, false);
		byte_ptr.assign(num_nodes + 1, 0);
	}

	BenchResult run() {
		// 保持压缩逻辑不变
		for (uint32_t i = 0; i < num_edges; ++i) freq[col_idx[i]]++;
		for (uint32_t i = 0; i < num_nodes; ++i) {
			uint32_t start = row_ptr[i], end = row_ptr[i + 1];
			if (end - start < 2) continue;
			sort(col_idx.begin() + start, col_idx.begin() + end, [&](uint32_t a, uint32_t b) {
				if (freq[a] != freq[b]) return freq[a] > freq[b];
				return a < b;
			});
		}

		for (uint32_t i = 0; i < num_nodes; ++i) {
			uint32_t start = row_ptr[i], end = row_ptr[i + 1];
			if (end - start < 2) continue;
			for (uint32_t j = start; j < end - 1; ++j) {
				if (parent[col_idx[j + 1]] == NO_PARENT) parent[col_idx[j + 1]] = col_idx[j];
			}
		}

		for (uint32_t i = 0; i < num_nodes; ++i) {
			byte_ptr[i] = static_cast<uint32_t>(byte_stream.size());
			uint32_t start = row_ptr[i], end = row_ptr[i + 1];
			if (start == end) continue;

			for (uint32_t j = start; j < end; ++j) {
				in_list[col_idx[j]] = true;
				is_packed[col_idx[j]] = false;
			}

			struct TempCmd {
				uint32_t bottom;
				uint32_t steps;
				bool is_path;
			};
			vector<TempCmd> temp_cmds;

			for (int j = static_cast<int>(end) - 1; j >= static_cast<int>(start); --j) {
				uint32_t bottom = col_idx[j];
				if (is_packed[bottom]) continue;

				uint32_t curr = bottom, steps = 0;
				vector<uint32_t> path_nodes = {curr};
				while (parent[curr] != NO_PARENT && in_list[parent[curr]] && !is_packed[parent[curr]] && steps < max_depth) {
					curr = parent[curr];
					path_nodes.push_back(curr);
					steps++;
				}

				if (steps >= 1) {
					for (uint32_t node : path_nodes) is_packed[node] = true;
					temp_cmds.push_back({bottom, steps, true});
				} else {
					is_packed[bottom] = true;
					temp_cmds.push_back({bottom, 0, false});
				}
			}

			for (auto it = temp_cmds.rbegin(); it != temp_cmds.rend(); ++it) {
				encodeVByte((it->bottom << 1) | (it->is_path ? 1 : 0), byte_stream);
				if (it->is_path) encodeVByte(it->steps, byte_stream);
			}

			for (uint32_t j = start; j < end; ++j) in_list[col_idx[j]] = false;
		}

		byte_ptr[num_nodes] = static_cast<uint32_t>(byte_stream.size());

		double tc_time_ms = 0.0;
		uint64_t tc = triangleCountingDirectOnCompressed(tc_time_ms);
		return {dataset, "Your Algorithm (Tree+VB DirectIntersect)", tc, tc_time_ms, 1};
	}
};

void printSummary(const vector<BenchResult>& rows) {
	unordered_map<string, double> baseline_time;
	for (const auto& r : rows) {
		if (r.name == "Uncompressed (32-bit)") {
			baseline_time[r.dataset] = r.tc_time_ms;
		}
	}

	cout << "\n========================================================================================\n";
	cout << left << setw(22) << "Dataset"
		 << setw(42) << "Algorithm"
		 << right << setw(16) << "TC_Count"
		 << setw(14) << "TC_Time(ms)"
		 << setw(12) << "Speedup"
		 << setw(10) << "Correct" << "\n";
	cout << "--------------------------------------------------------------------------------------------\n";

	for (size_t i = 0; i < rows.size(); ++i) {
		if (i > 0 && rows[i].dataset != rows[i - 1].dataset) {
			cout << "--------------------------------------------------------------------------------------------\n";
		}

		double speedup = numeric_limits<double>::quiet_NaN();
		auto it = baseline_time.find(rows[i].dataset);
		if (it != baseline_time.end() && rows[i].tc_time_ms > 0.0 && rows[i].name != "Uncompressed (32-bit)") {
			speedup = it->second / rows[i].tc_time_ms;
		}

		cout << left << setw(22) << rows[i].dataset
			 << setw(42) << rows[i].name
			 << right << setw(16) << rows[i].tc_count
			 << setw(14) << fixed << setprecision(2) << rows[i].tc_time_ms
			 << setw(12) << (std::isnan(speedup) ? string("-") : (formatDoubleOrBlank(speedup, 2) + "x"))
			 << setw(10) << (rows[i].correct ? "YES" : "NO") << "\n";
	}
	cout << "============================================================================================\n";
}

int main(int argc, char* argv[]) {
	if (argc < 2) {
		cout << "Usage: ./crossnew graph1.edges graph2.edges ...\n";
		return -1;
	}

	vector<BenchResult> all;
	uint32_t total_checked = 0, total_correct = 0;

	for (int i = 1; i < argc; ++i) {
		string filepath = argv[i];
		string ds = getBaseName(filepath);
		cout << "\n[Processing] " << ds << "\n";

		GraphLoader loader;
		if (!loader.loadFromText(filepath)) {
			cerr << "[ERROR] failed to load " << filepath << "\n";
			continue;
		}

		BenchResult base = runUncompressedBaseline(ds, loader.num_nodes, loader.row_ptr, loader.col_idx);
		IndustrialGraphCompressor comp(ds, loader.num_nodes, loader.num_edges, loader.row_ptr, loader.col_idx);
		BenchResult mine = comp.run();

		total_checked++;
		bool ok = (base.tc_count == mine.tc_count);
		base.correct = ok ? 1 : 0;
		mine.correct = ok ? 1 : 0;
		if (ok) total_correct++;
		else {
			cerr << "[ERROR] TC mismatch on " << ds << ": raw=" << base.tc_count << ", compressed=" << mine.tc_count << "\n";
		}

		all.push_back(base);
		all.push_back(mine);
	}

	printSummary(all);

	cout << "\nVerification Summary\n";
	cout << "Checked datasets: " << total_checked << "\n";
	cout << "Correct datasets: " << total_correct << "\n";
	double acc = (total_checked == 0) ? 0.0 : (100.0 * static_cast<double>(total_correct) / static_cast<double>(total_checked));
	cout << "TC accuracy: " << fixed << setprecision(2) << acc << "%\n";

	return 0;
}

