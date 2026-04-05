# graphCompression
基于树结构的图压缩算法
# 代码部分介绍
- baseline.cpp是包含树结构算法、vbyte+Delta算法和未压缩的总体代码，运行会将每个算法跑一遍，并生成csv文件保存数据，同时会在终端输出可视化表格。
- plot_results.py可将csv文件的数据可视化
- vb-dir.cpp是原始代码，只包含树结构算法

# 运行过程
- 先用O3编译baseline.cpp
  
`g++ baseline.cpp -O3 -std=c++14 -o benchmark`

- 再进行cpp运行
  
`./benchmark datasets/web-BerkStan-dir.edges  datasets/web-arabic-2005.mtx datasets/web-baidu-baike-related.edges datasets/web-it-2004.mtx datasets/web-Stanford.mtx  datasets/web-NotreDame.edges datasets/web-google.mtx datasets/web-uk-2005.mtx datasets/web-edu.mtx datasets/web-italycnr-2000.mtx datasets/web-EPA.edges datasets/web-hudong.edges datasets/web-wiki-ch-internal.edges`

- 得到csv文件后，打开plot_results.py文件进行运行生成可视化图

# 注意
datasets数据集太大，要去 "https://networkrepository.com/web.php" 中自行下载，或者找我要压缩包
