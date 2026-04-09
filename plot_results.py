import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import csv

EXPECTED_COLUMNS = ['Dataset', 'Algorithm', 'Data_MB', 'BPE', 'MEPS', 'Correct']
TARGET_ALGOS = [
    'Uncompressed (32-bit)',
    'SOTA',
    'Your Algorithm (Tree+VB SACT)'
]


def load_benchmark_csv(csv_path='benchmark_results.csv'):
    """Read benchmark CSV and repair malformed rows split by numeric commas."""
    rows = []
    bad_rows = 0

    with open(csv_path, 'r', encoding='utf-8', newline='') as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            return pd.DataFrame(columns=EXPECTED_COLUMNS)

        for row in reader:
            if not row:
                continue

            if len(row) == 6:
                rows.append(row)
                continue

            if len(row) > 6:
                # e.g. Data_MB was written like 1,367.31 without quoting
                dataset = row[0]
                algorithm = row[1]
                correct = row[-1]
                meps = row[-2]
                bpe = row[-3]
                data_mb = ''.join(part.strip() for part in row[2:-3])
                rows.append([dataset, algorithm, data_mb, bpe, meps, correct])
                bad_rows += 1
                continue

            bad_rows += 1

    if bad_rows > 0:
        print(f"警告: 已自动修复/跳过 {bad_rows} 行异常 CSV 记录。")

    return pd.DataFrame(rows, columns=EXPECTED_COLUMNS)

# 1. 读取 C++ 导出的 CSV 文件
try:
    df = load_benchmark_csv('benchmark_results.csv')
except FileNotFoundError:
    print("错误: 找不到 benchmark_results.csv，请先运行 C++ 编译好的程序。")
    exit()

if df.empty:
    print("错误: benchmark_results.csv 没有可用数据，无法绘图。")
    exit()

for col in ['Data_MB', 'BPE', 'MEPS', 'Correct']:
    df[col] = pd.to_numeric(df[col], errors='coerce')

df = df.dropna(subset=['Dataset', 'Algorithm', 'BPE'])
if df.empty:
    print("错误: 清洗后无有效记录，无法绘图。")
    exit()

# 获取数据集中所有的图名字
datasets = df['Dataset'].unique()

# 只画三个目标算法（若缺失则自动跳过并提示）
all_algos = set(df['Algorithm'].dropna().unique())
algos = [a for a in TARGET_ALGOS if a in all_algos]
missing_algos = [a for a in TARGET_ALGOS if a not in all_algos]
if missing_algos:
    print("警告: 以下算法在 CSV 中不存在，将不绘制: " + ", ".join(missing_algos))

if not algos:
    print("错误: 三个目标算法都不存在，无法绘图。")
    exit()

# 图例展示名称（不影响数据筛选键）
display_names = {
    'Uncompressed (32-bit)': 'Uncompressed (32-bit)',
    'SOTA': 'SOTA',
    'Your Algorithm (Tree+VB SACT)': 'Tree+VB v1'
}

# 颜色和标记
base_colors = {
    'Uncompressed (32-bit)': '#95a5a6',
    'SOTA': '#e74c3c',
    'Your Algorithm (Tree+VB SACT)': '#16a085'
}
base_markers = {
    'Uncompressed (32-bit)': 's',
    'SOTA': '^',
    'Your Algorithm (Tree+VB SACT)': 'D'
}
base_linestyles = {
    'Uncompressed (32-bit)': '-',
    'SOTA': '--',
    'Your Algorithm (Tree+VB SACT)': '-'
}
colors = {a: base_colors[a] for a in algos}
markers = {a: base_markers[a] for a in algos}
linestyles = {a: base_linestyles[a] for a in algos}

# ==========================================
# 图 1: 压缩比比较 (相对未压缩基线, %)
# ==========================================
plt.figure(figsize=(10, 6))

# 以每个数据集的未压缩 BPE 作为基线，计算压缩比 = 当前BPE/基线BPE * 100
baseline_bpe = (
    df[df['Algorithm'] == 'Uncompressed (32-bit)'][['Dataset', 'BPE']]
    .drop_duplicates('Dataset')
    .set_index('Dataset')['BPE']
)

for algo in algos:
    algo_data = df[df['Algorithm'] == algo]
    if algo_data.empty:
        continue
    
    # 按 datasets 的顺序对齐数据
    algo_data = algo_data.set_index('Dataset').reindex(datasets)

    # 防御性处理：用基线换算压缩比，并处理 inf/-inf 与除零
    ratio_values = (algo_data['BPE'] / baseline_bpe.reindex(algo_data.index) * 100.0)
    ratio_values = ratio_values.replace([np.inf, -np.inf], np.nan)

    marker_face = colors[algo]
    z = 3
    
    plt.plot(algo_data.index, ratio_values,
             marker=markers[algo], markersize=9, linewidth=2.5,
             linestyle=linestyles[algo], markerfacecolor=marker_face,
             markeredgewidth=1.8, zorder=z,
             color=colors[algo], label=display_names.get(algo, algo))

plt.title('Compression Ratio Comparison (Lower is Better)', fontsize=16, fontweight='bold')
plt.xlabel('Graph Datasets', fontsize=14)
plt.ylabel('Compression Ratio (% of Uncompressed)', fontsize=14)
plt.xticks(fontsize=12, rotation=15) # 稍微倾斜数据集名字，防止重叠
plt.yticks(fontsize=12)
plt.grid(True, linestyle='--', alpha=0.6)
plt.legend(fontsize=12)
plt.tight_layout()

# 保存高质量图表
plt.savefig('compression_ratio_comparison.png', dpi=300)
print("✅ 成功生成压缩比折线图: compression_ratio_comparison.png")

plt.show() # 如果在带界面的系统运行，直接弹出预览窗口