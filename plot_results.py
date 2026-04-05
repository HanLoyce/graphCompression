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

# 定义我们想要的算法顺序和颜色 (学术风调色)
algos = ['Uncompressed (32-bit)', 'Delta + VByte', 'Your Algorithm (Tree+VB)']
colors = {'Uncompressed (32-bit)': '#95a5a6',  # 灰色
          'Delta + VByte': '#e74c3c',         # 红色
          'Your Algorithm (Tree+VB)': '#2980b9'} # 蓝色
markers = {'Uncompressed (32-bit)': 's', 
           'Delta + VByte': '^', 
           'Your Algorithm (Tree+VB)': 'o'}

# ==========================================
# 图 1: 压缩率比较 (BPE - Bits Per Edge)
# ==========================================
plt.figure(figsize=(10, 6))

for algo in algos:
    algo_data = df[df['Algorithm'] == algo]
    if algo_data.empty: continue
    
    # 按 datasets 的顺序对齐数据
    algo_data = algo_data.set_index('Dataset').reindex(datasets)
    
    plt.plot(algo_data.index, algo_data['BPE'], 
             marker=markers[algo], markersize=9, linewidth=2.5, 
             color=colors[algo], label=algo)

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

for algo in algos:
    algo_data = df[df['Algorithm'] == algo]
    if algo_data.empty: continue
    
    algo_data = algo_data.set_index('Dataset').reindex(datasets)
    
    plt.plot(algo_data.index, algo_data['MEPS'], 
             marker=markers[algo], markersize=9, linewidth=2.5, 
             color=colors[algo], label=algo)

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