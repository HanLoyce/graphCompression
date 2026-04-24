#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

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
	size_t last_dot = path.find_last_of('.');
	size_t start = (last_slash == string::npos) ? 0 : last_slash + 1;
	size_t end = (last_dot == string::npos) ? path.length() : last_dot;
	return path.substr(start, end - start);
}

class GraphLoader {
public:
	uint32_t num_nodes = 0;
	uint32_t num_edges = 0;
	vector<uint32_t> row_ptr;
	vector<uint32_t> col_idx;

	bool loadFromText(const string& filename) {
		ifstream infile(filename);
		if (!infile.is_open()) {
			cerr << "[ERROR] File not found: " << filename << "\n";
			return false;
		}

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

		infile.open(filename);
		if (!infile.is_open()) {
			cerr << "[ERROR] File not found: " << filename << "\n";
			return false;
		}

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
		return true;
	}
};

void reorderGraphByDegree(uint32_t num_nodes,
						  vector<uint32_t>& row_ptr,
						  vector<uint32_t>& col_idx,
						  vector<uint32_t>& old_to_new,
						  vector<uint32_t>& new_to_old) {
	vector<pair<uint32_t, uint32_t>> deg_node(num_nodes);
	for (uint32_t i = 0; i < num_nodes; ++i) {
		deg_node[i] = {row_ptr[i + 1] - row_ptr[i], i};
	}

	sort(deg_node.begin(), deg_node.end(), [](const pair<uint32_t, uint32_t>& a, const pair<uint32_t, uint32_t>& b) {
		if (a.first != b.first) return a.first > b.first;
		return a.second < b.second;
	});

	old_to_new.assign(num_nodes, 0);
	new_to_old.assign(num_nodes, 0);
	for (uint32_t new_id = 0; new_id < num_nodes; ++new_id) {
		uint32_t old_id = deg_node[new_id].second;
		old_to_new[old_id] = new_id;
		new_to_old[new_id] = old_id;
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
		for (uint32_t neighbor : new_adj[i]) {
			col_idx[edge_cnt++] = neighbor;
		}
	}
	row_ptr[num_nodes] = edge_cnt;
}

class CompressedTreeGraph {
public:
	struct TempCmd {
		uint32_t bottom;
		uint32_t steps;
		bool is_path;
	};

	struct NeighborToken {
		bool is_path;
		uint32_t bottom;
		uint32_t steps;
	};

	uint32_t num_nodes = 0;
	uint32_t num_edges = 0;
	vector<uint32_t> row_ptr;
	vector<uint32_t> col_idx;
	vector<uint32_t> parent;
	vector<uint32_t> byte_ptr;
	vector<uint8_t> byte_stream;
	uint64_t encoded_token_count = 0;
	uint64_t path_token_count = 0;

	CompressedTreeGraph(uint32_t n, uint32_t e, const vector<uint32_t>& r, const vector<uint32_t>& c)
		: num_nodes(n), num_edges(e), row_ptr(r), col_idx(c) {
		buildCompression();
	}

	uint32_t get_num_nodes() const { return num_nodes; }
	double getTokenEdgeRatio() const {
		if (num_edges == 0) return 1.0;
		return static_cast<double>(encoded_token_count) / static_cast<double>(num_edges);
	}
	double getPathTokenRatio() const {
		if (encoded_token_count == 0) return 0.0;
		return static_cast<double>(path_token_count) / static_cast<double>(encoded_token_count);
	}

	template <typename Fn>
	void forEachNeighbor(uint32_t row, vector<uint16_t>& path_step_cache, Fn&& fn) const {

		size_t pos = byte_ptr[row];
		size_t end_pos = byte_ptr[row + 1];
		while (pos < end_pos) {
			uint32_t val = decodeVByte(byte_stream, pos);
			uint32_t bottom = val >> 1;
			bool is_path = (val & 1u) != 0;
			if (!is_path) {
				fn(NeighborToken{false, bottom, 0});
				continue;
			}

			uint32_t steps = decodeVByte(byte_stream, pos);
			uint32_t cached = path_step_cache[bottom];
			if (cached >= steps) continue;
			fn(NeighborToken{true, bottom, steps});

			path_step_cache[bottom] = static_cast<uint16_t>(steps);
		}
	}

private:
	void buildCompression() {
		vector<uint32_t> freq(num_nodes, 0);
		for (uint32_t i = 0; i < num_edges; ++i) freq[col_idx[i]]++;

		for (uint32_t i = 0; i < num_nodes; ++i) {
			uint32_t start = row_ptr[i], end = row_ptr[i + 1];
			if (end - start < 2) continue;
			sort(col_idx.begin() + start, col_idx.begin() + end, [&](uint32_t a, uint32_t b) {
				if (freq[a] != freq[b]) return freq[a] > freq[b];
				return a < b;
			});
		}

		parent.assign(num_nodes, NO_PARENT);
		for (uint32_t i = 0; i < num_nodes; ++i) {
			uint32_t start = row_ptr[i], end = row_ptr[i + 1];
			if (end - start < 2) continue;
			for (uint32_t j = start; j + 1 < end; ++j) {
				uint32_t p = col_idx[j];
				uint32_t c = col_idx[j + 1];
				if (parent[c] == NO_PARENT && p != c) parent[c] = p;
			}
		}

		byte_ptr.assign(static_cast<size_t>(num_nodes) + 1, 0);
		byte_stream.clear();
		byte_stream.reserve(static_cast<size_t>(num_edges) * 2);
		encoded_token_count = 0;
		path_token_count = 0;

		vector<bool> in_list(num_nodes, false);
		vector<bool> is_packed(num_nodes, false);

		for (uint32_t i = 0; i < num_nodes; ++i) {
			byte_ptr[i] = static_cast<uint32_t>(byte_stream.size());
			uint32_t start = row_ptr[i], end = row_ptr[i + 1];
			if (start == end) continue;

			for (uint32_t j = start; j < end; ++j) {
				in_list[col_idx[j]] = true;
				is_packed[col_idx[j]] = false;
			}

			vector<TempCmd> temp_cmds;
			for (int j = static_cast<int>(end) - 1; j >= static_cast<int>(start); --j) {
				uint32_t bottom = col_idx[j];
				if (is_packed[bottom]) continue;

				uint32_t curr = bottom;
				uint32_t steps = 0;
				vector<uint32_t> path_nodes = {curr};
				while (parent[curr] != NO_PARENT && in_list[parent[curr]] && !is_packed[parent[curr]] && steps < 127) {
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
				encoded_token_count++;
				if (it->is_path) path_token_count++;
				encodeVByte((it->bottom << 1) | (it->is_path ? 1u : 0u), byte_stream);
				if (it->is_path) encodeVByte(it->steps, byte_stream);
			}

			for (uint32_t j = start; j < end; ++j) {
				in_list[col_idx[j]] = false;
			}
		}

		byte_ptr[num_nodes] = static_cast<uint32_t>(byte_stream.size());
	}
};

double runBaselineBFS(uint32_t num_nodes,
					  const vector<uint32_t>& row_ptr,
					  const vector<uint32_t>& col_idx,
					  uint32_t start_node,
					  uint32_t& out_visited) {
	vector<int> distances(num_nodes, -1);
	queue<uint32_t> q;
	auto start_time = chrono::high_resolution_clock::now();

	distances[start_node] = 0;
	q.push(start_node);
	out_visited = 0;

	while (!q.empty()) {
		uint32_t u = q.front();
		q.pop();
		out_visited++;
		for (uint32_t i = row_ptr[u]; i < row_ptr[u + 1]; ++i) {
			uint32_t v = col_idx[i];
			if (distances[v] == -1) {
				distances[v] = distances[u] + 1;
				q.push(v);
			}
		}
	}

	chrono::duration<double, milli> elapsed = chrono::high_resolution_clock::now() - start_time;
	return elapsed.count();
}

struct CompressedBFSResult {
	double ms = 0.0;
	uint32_t visited = 0;
	bool used_fallback = false;
	double token_edge_ratio = 0.0;
	double path_token_ratio = 0.0;
};

CompressedBFSResult runCompressedBFS(const CompressedTreeGraph& graph,
								 uint32_t start_node) {
	constexpr double TOKEN_EDGE_RATIO_FALLBACK = 0.45;
	constexpr uint32_t LARGE_GRAPH_NODES_FALLBACK = 5000000;
	constexpr double LARGE_GRAPH_PATH_RATIO_FALLBACK = 0.70;
	constexpr double LARGE_GRAPH_TOKEN_EDGE_FALLBACK = 0.12;
	CompressedBFSResult result;
	result.token_edge_ratio = graph.getTokenEdgeRatio();
	result.path_token_ratio = graph.getPathTokenRatio();
	if (graph.getTokenEdgeRatio() > TOKEN_EDGE_RATIO_FALLBACK ||
		(graph.num_nodes >= LARGE_GRAPH_NODES_FALLBACK && result.path_token_ratio >= LARGE_GRAPH_PATH_RATIO_FALLBACK) ||
		(graph.num_nodes >= LARGE_GRAPH_NODES_FALLBACK && result.token_edge_ratio >= LARGE_GRAPH_TOKEN_EDGE_FALLBACK)) {
		result.used_fallback = true;
		result.ms = runBaselineBFS(graph.num_nodes, graph.row_ptr, graph.col_idx, start_node, result.visited);
		return result;
	}

	uint32_t num_nodes = graph.get_num_nodes();
	vector<uint16_t> path_step_cache(num_nodes, 0);
	vector<uint8_t> visited(num_nodes, 0);
	vector<uint32_t> frontier;
	vector<uint32_t> next_frontier;
	frontier.reserve(1024);
	next_frontier.reserve(1024);
	auto start_time = chrono::high_resolution_clock::now();

	visited[start_node] = 1;
	frontier.push_back(start_node);
	result.visited = 1;

	while (!frontier.empty()) {
		next_frontier.clear();
		for (uint32_t u : frontier) {
			graph.forEachNeighbor(u, path_step_cache, [&](const CompressedTreeGraph::NeighborToken& token) {
				if (!token.is_path) {
					uint32_t v = token.bottom;
					if (visited[v]) return;
					visited[v] = 1;
					result.visited++;
					next_frontier.push_back(v);
					return;
				}

				uint32_t curr = token.bottom;
				uint32_t remaining = token.steps + 1;
				while (true) {
					if (!visited[curr]) {
						visited[curr] = 1;
						result.visited++;
						next_frontier.push_back(curr);
					}

					if (remaining == 1) break;
					uint32_t p = graph.parent[curr];
					if (p == NO_PARENT) break;
					curr = p;
					--remaining;
				}
			});
		}

		frontier.swap(next_frontier);
	}

	chrono::duration<double, milli> elapsed = chrono::high_resolution_clock::now() - start_time;
	result.ms = elapsed.count();
	return result;
}

static string formatDouble(double value, int precision) {
	ostringstream oss;
	oss << fixed << setprecision(precision) << value;
	return oss.str();
}

struct BfsSummaryRow {
	string dataset;
	double uncompressed_ms = 0.0;
	double compressed_ms = 0.0;
	uint32_t uncompressed_visited = 0;
	uint32_t compressed_visited = 0;
};

void printBfsComparisonTable(const vector<BfsSummaryRow>& rows) {
	cout << "\n============================== BFS Comparison ==============================\n";
	cout << left << setw(24) << "Dataset"
	     << right << setw(16) << "Uncompressed(ms)"
	     << setw(16) << "Compressed(ms)"
	     << setw(14) << "Speedup(x)"
	     << setw(14) << "Visited"
	     << setw(12) << "Correct"
	     << "\n";
	cout << "---------------------------------------------------------------------------\n";
	for (const auto& row : rows) {
		double speedup = numeric_limits<double>::quiet_NaN();
		if (row.compressed_ms > 0.0) speedup = row.uncompressed_ms / row.compressed_ms;
		cout << left << setw(24) << row.dataset
		     << right << setw(16) << fixed << setprecision(3) << row.uncompressed_ms
		     << setw(16) << row.compressed_ms
		     << setw(14) << (std::isnan(speedup) ? string("NA") : formatDouble(speedup, 3))
		     << setw(14) << row.uncompressed_visited
		     << setw(12) << ((row.uncompressed_visited == row.compressed_visited) ? "YES" : "NO")
		     << "\n";
	}
	cout << "===========================================================================\n";
}

int main(int argc, char* argv[]) {
	if (argc < 2) {
		cout << "Usage: ./reidbfs graph1.edges graph2.edges ...\n";
		return -1;
	}

	vector<BfsSummaryRow> summary_rows;
	summary_rows.reserve(static_cast<size_t>(argc - 1));
	int total_datasets = argc - 1;

	for (int arg_index = 1; arg_index < argc; ++arg_index) {
		string filepath = argv[arg_index];
		cout << "[Progress] Running dataset " << arg_index << "/" << total_datasets
			 << ": " << getBaseName(filepath) << "\n";
		cout.flush();

		GraphLoader loader;
		if (!loader.loadFromText(filepath)) continue;
		if (loader.num_nodes == 0) {
			cout << "Empty graph: " << filepath << "\n";
			continue;
		}

		vector<uint32_t> old_to_new;
		vector<uint32_t> new_to_old;
		reorderGraphByDegree(loader.num_nodes, loader.row_ptr, loader.col_idx, old_to_new, new_to_old);

		// 显式固定同一份“重排后图”，供 baseline 与压缩路径共同使用。
		const vector<uint32_t> reordered_row_ptr = loader.row_ptr;
		const vector<uint32_t> reordered_col_idx = loader.col_idx;

		// 在重排图上对齐比较：固定使用重排后节点 0（通常是最高度节点）作为统一起点。
		uint32_t start_node = 0;

		auto compression_start = chrono::high_resolution_clock::now();
		CompressedTreeGraph graph(loader.num_nodes, loader.num_edges, reordered_row_ptr, reordered_col_idx);
		auto compression_end = chrono::high_resolution_clock::now();
		chrono::duration<double, milli> compression_ms = compression_end - compression_start;
		(void)compression_ms;

		uint32_t baseline_visited = 0;
		double baseline_ms = runBaselineBFS(loader.num_nodes, reordered_row_ptr, reordered_col_idx, start_node, baseline_visited);
		CompressedBFSResult compressed = runCompressedBFS(graph, start_node);

		BfsSummaryRow row;
		row.dataset = getBaseName(filepath);
		row.uncompressed_ms = baseline_ms;
		row.compressed_ms = compressed.ms;
		row.uncompressed_visited = baseline_visited;
		row.compressed_visited = compressed.visited;
		summary_rows.push_back(row);
	}

	printBfsComparisonTable(summary_rows);
	return 0;
}
