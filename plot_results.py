import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# 1. 读取 C++ 导出的 CSV 文件
try:
    df = pd.read_csv('benchmark_results.csv')
except FileNotFoundError:
    print("错误: 找不到 benchmark_results.csv，请先运行 C++ 编译好的程序。")
    exit()

# 获取数据集中所有的图名字
datasets = df['Dataset'].unique()

# 按“预期顺序优先 + 其余算法自动追加”生成算法列表
preferred_algos = [
    'Uncompressed (32-bit)',
    'Delta + VByte',
    'Your Algorithm (Tree+VB)',
    'Your Algorithm (Tree+VB SACT)'
]
all_algos = list(df['Algorithm'].dropna().unique())
algos = [a for a in preferred_algos if a in all_algos]
algos += [a for a in all_algos if a not in algos]

# 图例展示名称（不影响数据筛选键）
display_names = {
    'Uncompressed (32-bit)': 'Uncompressed (32-bit)',
    'Delta + VByte': 'Delta + VByte',
    'Your Algorithm (Tree+VB)': 'Tree+VB',
    'Your Algorithm (Tree+VB SACT)': 'Tree+VB v1'
}

# 颜色和标记：已知算法固定风格，新增算法自动分配
base_colors = {
    'Uncompressed (32-bit)': '#95a5a6',
    'Delta + VByte': '#e74c3c',
    'Your Algorithm (Tree+VB)': '#2980b9',
    'Your Algorithm (Tree+VB SACT)': '#16a085'
}
base_markers = {
    'Uncompressed (32-bit)': 's',
    'Delta + VByte': '^',
    'Your Algorithm (Tree+VB)': 'o',
    'Your Algorithm (Tree+VB SACT)': 'D'
}
base_linestyles = {
    'Uncompressed (32-bit)': '-',
    'Delta + VByte': '-',
    'Your Algorithm (Tree+VB)': '--',
    'Your Algorithm (Tree+VB SACT)': '-'
}
fallback_colors = ['#34495e', '#d35400', '#27ae60', '#8e44ad', '#c0392b']
fallback_markers = ['x', 'v', 'P', '*', 'h']

colors = {}
markers = {}
linestyles = {}
fc_idx = 0
fm_idx = 0
for algo in algos:
    if algo in base_colors:
        colors[algo] = base_colors[algo]
    else:
        colors[algo] = fallback_colors[fc_idx % len(fallback_colors)]
        fc_idx += 1

    if algo in base_markers:
        markers[algo] = base_markers[algo]
    else:
        markers[algo] = fallback_markers[fm_idx % len(fallback_markers)]
        fm_idx += 1

    linestyles[algo] = base_linestyles.get(algo, '-')


def build_draw_order(algorithms):
    # 让迭代版先画、Tree+VB 后画，避免两条几乎重合时 Tree+VB 被遮盖。
    order = [a for a in algorithms if a != 'Your Algorithm (Tree+VB)']
    if 'Your Algorithm (Tree+VB)' in algorithms:
        order.append('Your Algorithm (Tree+VB)')
    return order


draw_order = build_draw_order(algos)

# ==========================================
# 图 1: 压缩率比较 (BPE - Bits Per Edge)
# ==========================================
plt.figure(figsize=(10, 6))

for algo in draw_order:
    algo_data = df[df['Algorithm'] == algo]
    if algo_data.empty: continue
    
    # 按 datasets 的顺序对齐数据
    algo_data = algo_data.set_index('Dataset').reindex(datasets)

    # 防御性处理：inf/-inf 转为 NaN，避免折线断点产生误解
    bpe_values = algo_data['BPE'].replace([np.inf, -np.inf], np.nan)

    marker_face = 'white' if algo == 'Your Algorithm (Tree+VB)' else colors[algo]
    z = 4 if algo == 'Your Algorithm (Tree+VB)' else 3
    
    plt.plot(algo_data.index, bpe_values,
             marker=markers[algo], markersize=9, linewidth=2.5,
             linestyle=linestyles[algo], markerfacecolor=marker_face,
             markeredgewidth=1.8, zorder=z,
             color=colors[algo], label=display_names.get(algo, algo))

plt.title('Compression Ratio Comparison (Lower is Better)', fontsize=16, fontweight='bold')
plt.xlabel('Graph Datasets', fontsize=14)
plt.ylabel('Bits Per Edge (BPE)', fontsize=14)
plt.xticks(fontsize=12, rotation=15) # 稍微倾斜数据集名字，防止重叠
plt.yticks(fontsize=12)
plt.grid(True, linestyle='--', alpha=0.6)
plt.legend(fontsize=12)
plt.tight_layout()

# 保存高质量图表
plt.savefig('bpe_comparison.png', dpi=300)
print("✅ 成功生成压缩率折线图: bpe_comparison.png")

# ==========================================
# 图 2: 解压吞吐量比较 (MEPS - Millions of Edges Per Second)
# ==========================================
plt.figure(figsize=(10, 6))

for algo in draw_order:
    algo_data = df[df['Algorithm'] == algo]
    if algo_data.empty: continue
    
    algo_data = algo_data.set_index('Dataset').reindex(datasets)

    # 防御性处理：inf/-inf 转为 NaN，避免图上出现“空洞但不知原因”
    meps_values = algo_data['MEPS'].replace([np.inf, -np.inf], np.nan)

    marker_face = 'white' if algo == 'Your Algorithm (Tree+VB)' else colors[algo]
    z = 4 if algo == 'Your Algorithm (Tree+VB)' else 3
    
    plt.plot(algo_data.index, meps_values,
             marker=markers[algo], markersize=9, linewidth=2.5,
             linestyle=linestyles[algo], markerfacecolor=marker_face,
             markeredgewidth=1.8, zorder=z,
             color=colors[algo], label=display_names.get(algo, algo))

plt.title('Decompression Throughput (Higher is Better)', fontsize=16, fontweight='bold')
plt.xlabel('Graph Datasets', fontsize=14)
plt.ylabel('Throughput (MEPS)', fontsize=14)
plt.xticks(fontsize=12, rotation=15)
plt.yticks(fontsize=12)
plt.grid(True, linestyle='--', alpha=0.6)
plt.legend(fontsize=12)
plt.tight_layout()

# 保存高质量图表
plt.savefig('meps_comparison.png', dpi=300)
print("✅ 成功生成解压速度折线图: meps_comparison.png")

plt.show() # 如果在带界面的系统运行，直接弹出预览窗口