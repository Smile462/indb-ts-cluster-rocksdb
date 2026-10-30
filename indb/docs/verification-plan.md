# 功能验证流程

本文件定义"两个平台的实现是否真的没问题"的判定流程。全部步骤可复现，每步都有明确的通过与不通过标准。

适用对象：
- RocksDB 实现：`indb-ts-cluster-rocksdb` 的 `indb/`
- PostgreSQL 实现：`indb-ts-cluster-postgresql` 的 `indb/`

约定：`$RDB = E:\build\indb\Release`，`$PG = E:\pgsql`，`$WORK = E:\tmp\indb-verify`。

配套脚本（均可重复执行）：

| 脚本 | 覆盖阶段 | 前提 |
|---|---|---|
| `indb/scripts/verify.ps1` | 0–3 | 两个平台均已构建 |
| `indb/scripts/verify-oracle.ps1` | 4（与 Java 参考实现对比） | JDK 与部署的 UDF jar |
| `indb/scripts/verify-realdata.ps1` | 5（真实数据集） | 数据源目录 |

---

## 阶段 0：环境自检

```powershell
Test-Path "$RDB\indb_cli.exe", "$RDB\indb_tests.exe"
Test-Path "$PG\bin\psql.exe", "$PG\lib\postgres.lib", "$PG\lib\indb_tsc.dll"
Get-Service pg18indb | Select-Object Status
```

**通过标准**：全部为 `True`，服务为 `Running`。

若 `indb_tsc.dll` 不存在或版本旧，先重建：

```powershell
powershell -ExecutionPolicy Bypass -File indb\scripts\build.ps1 -Install   # PG 仓库
cmake --build E:\build\indb --config Release                              # RocksDB 仓库
```

---

## 阶段 1：单元与端到端测试

```powershell
& "$RDB\indb_tests.exe"
```

**通过标准**：输出 `ALL TESTS PASSED`，退出码 0。

覆盖内容：

| 用例 | 验证点 |
|---|---|
| zscore | 样本标准差（分母 l-1）与期望值一致 |
| fft | 正反变换往返误差 < 1e-12；`fft_size_for` 边界（166→512、96→256） |
| eigen | 2×2 对称矩阵特征值与特征向量 |
| meta round trip | 页元数据编解码后矩阵/中心逐位相同 |
| parameter guard | 用错误的 l 校验元数据必须抛错 |
| store end to end | 导入→8 页 3 块→四路径→关库重开→部分区间修正→参数不匹配拒绝 |

---

## 阶段 2：合成数据四方法一致性（核心）

这一阶段回答两个问题：**实现是否自洽**，以及**两个平台的实现是否等价**。

### 2.1 生成数据

脚本 `indb/scripts/verify.ps1` 会生成 20000 点、三段形状交替、带噪声的确定性序列（固定 seed）。

### 2.2 RocksDB

```powershell
& "$RDB\indb_cli.exe" import --db $WORK\rdb --ds 1 --csv $WORK\series.csv `
    --k 3 --l 50 --page-points 2000 --chunk-pages 4
& "$RDB\indb_cli.exe" info --db $WORK\rdb
& "$RDB\indb_cli.exe" query --db $WORK\rdb --ds 1 --t0 0 --t1 19999 --method kshape
```

期望：`info` 输出 `points=20000 pages=10 chunks=3`。

### 2.3 PostgreSQL

```sql
SELECT indb_tsc_import(1, 'E:/tmp/indb-verify/series.csv', false);
SELECT indb_tsc_build(1, 3, 50, 2000, 4, 30, 24301);
SELECT indb_tsc_kshape(1, 0, 19999, 'page');
```

### 2.4 判定表

| 检查项 | 通过标准 |
|---|---|
| 中心数量 | 四条路径均为 `k × l` 个数（本例 150） |
| 数值有限 | 无 NaN / Inf |
| 两平台逐位一致 | `kshape` page、`kshape` chunk、`kshape_raw`、`mshape_raw` 最大绝对差 = 0 |
| 全区间不修正 | `pages_corrected = 0`，`raw_points = 0` |
| 块级确实用块 | `chunks_meta = 3`，`pages_meta = 0` |

> `mshape`（预计算版）两边抽样流不同，只做统计性比较，不要求逐位相同。

---

## 阶段 3：边界与异常用例

| 用例 | 命令要点 | 通过标准 |
|---|---|---|
| 单页区间 | `--t0 100 --t1 900` | 返回 k×l；`pages_corrected >= 1`；`raw_points` 只等于边界页点数 |
| 跨页不对齐 | `--t0 500 --t1 12000` | `pages_meta + pages_corrected = pages_considered` |
| 空区间 | `--t0 999999 --t1 999999` | 返回空结果，不崩溃、不报错 |
| 区间在数据外 | `--t0 -100 --t1 -1` | 同上 |
| 参数不匹配（RocksDB） | 查询时加 `--l 99` | 报 `query parameters do not match the stored dataset` |
| 未建元数据（PG） | 新 ds 直接查询 | 报 `dataset N has no metadata` |
| 常量序列 | 全 1 的序列 | 不崩溃；中心若为 NaN 需在报告中记录（参考实现同样产生 NaN） |

---

## 阶段 4：Java 参考实现对齐

这是**最强的正确性证据**：与 IoTDB 原实现比，而不只是自己和自己比。

素材：`E:\实验\work\environment\iotdb-0.13.3-clean\ext\udf\library-udf-0.14.0-SNAPSHOT-jar-with-dependencies.jar`
（内含 `org.apache.iotdb.library.dmatch.util.KShape` / `KShapeM` / `KMeans`）。

做法：

1. 用同一份 CSV 准备输入：按 `l` 切好的子序列矩阵。
2. Java 侧调用 `KShape(k, 100).fit(X)` 与 `KShapeM(k, edK, 0.2, 100).fit(X)`，输出中心与 delta。
3. C++ 侧用 `kshape_raw` / `mshape_raw` 跑同一输入。
4. 比较前先对齐：聚类结果按中心与最近邻匹配重排，特征向量按符号对齐。

**通过标准**（统计性判据，不是逐位一致）：

K-Shape 与 Medoid-Shape 都从随机初始分配出发，两个**都正确**的实现不会给出相同数字，给出的是同一个"最优解分布"。所以判据是：

1. 先量参考实现自身的离散度：Java 连跑 5 次，两两之间的最大中心差 `javaSpread`。
2. 再量本实现换种子的离散度：C++ 跑 3 个种子，两两最大差 `cppSpread`。
3. 判定：
   - C++ 与 Java 的**最优配对差** `<= javaSpread`（即落在参考自身的波动范围内）；
   - `cppSpread <= javaSpread`（本实现不比参考更不稳定）。

**为什么不能用 1e-6**：初版判据写的是"中心差 < 1e-6"，实测立刻被推翻——Java 自己两次运行之间就能差 1e-1。1e-6 只适用于**给定同一初始分配**的纯数值比对（那部分已经在阶段 1 的单元测试里覆盖）。

**Medoid-Shape 用结构判据**：medoid 必须是输入子序列之一，绝不能是平均值。所以检查每个返回中心都能在 z-score 后的输入子序列里找到完全相同的行（差 < 1e-9）。这一条与随机采样无关，两边都必须满足。

---

## 阶段 5：真实数据尺度

在 Air（k=3, l=166, 714962 点）上重复阶段 2：

```powershell
& "$RDB\indb_cli.exe" import --db $WORK\air --ds 1 --csv <air.csv> --k 3 --l 166 `
    --page-points 10240 --chunk-pages 8
```

```sql
SELECT indb_tsc_import(1, '<air.csv>', false);
SELECT indb_tsc_build(1, 3, 166, 10240, 8, 30, 24301);
```

**通过标准**：

| 检查项 | 标准 |
|---|---|
| 页数 | `ceil(点数 / 10240)` |
| 全区间查询 | `pages_corrected = 0`，`raw_points = 0` |
| 元数据路径耗时 | 显著低于 `*_raw`（至少 10 倍，本例规模下） |
| 两平台 | 确定性路径逐位一致 |
| 元数据体积 | 与理论值 `8·k·l² + …` 同量级（Air 约 683 KB/页） |

---

## 阶段 6：持久化

| 平台 | 做法 | 通过标准 |
|---|---|---|
| RocksDB | 每次 CLI 调用都是独立进程，导入后再次查询即验证了重开；额外执行 `info` 确认目录结构 | 重开结果与首次一致 |
| PostgreSQL | `Restart-Service pg18indb` 后再查询 | 结果与重启前一致 |

---

## 阶段 7：性能合理性（不判正确性）

记录但不必"更好"：

- `kshape`（元数据）vs `kshape_raw`（从头）的耗时比；
- 元数据读取字节数 / 原始点读取数；
- 合并次数（`units_merged`）。

这些数字是后续 §8.4 实验的基线，出现数量级异常（例如元数据路径反而慢）要先排查，再进入正式计时。

---

## 阶段 4 与阶段 5 的实测结果（2026-10-30）

### 阶段 4：`indb/scripts/verify-oracle.ps1`（30 项全过）

数据：3 种明显分离的形状 × 50 份 + 噪声，l=64，共 150 条子序列。

| 指标 | 数值 |
|---|---:|
| Java 自身 5 次运行的离散度 | 1.086e-1 |
| C++ 3 个种子的离散度 | 2.603e-2 |
| C++ 与 Java 的最优配对差 | 1.374e-2 |
| Medoid 结构检查（cpp / java） | 差 < 1e-9，通过 |

结论：C++ 与参考实现的差距**小于参考自身两次运行的差距**，且本实现的随机性更小（因为种子可控）。这说明残留差异来自初始化，不是移植错误。

### 阶段 5：`indb/scripts/verify-realdata.ps1`（30 项全过）

数据：Air，70 个 CSV，714,962 点 = 4307 × 166；k=3，l=166，页 10240 点，块 8 页。

| 项 | RocksDB | PostgreSQL |
|---|---:|---:|
| 导入 | 45.2 s | 2.1 s |
| 建元数据 | （同上，导入时一并完成） | 42.2 s |
| 页数 / 块数 | 70 / 9 | 70 / 9 |
| `kshape` page | 0.352 s | 255 ms |
| `kshape` chunk | 0.257 s | 233 ms |
| `mshape` page | 0.171 s | 351 ms |
| `kshape_raw` | 7.370 s | 7.186 s |
| `mshape_raw` | 0.595 s | 576 ms |
| 库体积 | 66.1 MB | — |

一致性：

| 路径 | 最大绝对差 |
|---|---:|
| `kshape` page | **0.0** |
| `kshape` chunk | **0.0** |
| `kshape_raw` | **0.0** |
| `mshape_raw` | **0.0** |
| `mshape` page | 0.906（抽样流不同，已知且可接受） |

元数据路径比从头聚类快 **24.8 倍**（RocksDB），与论文"预计算加速查询"的前提一致。

---

## 验收总表

| 阶段 | 判定 | 不通过时的处理 |
|---|---|---|
| 0 环境 | 全部就绪 | 补构建/安装 |
| 1 单测 | `ALL TESTS PASSED` | 停下修代码 |
| 2 一致性 | 确定性路径 maxdiff = 0 | 停下对比两侧内核版本（`vendor/README.md` 哈希） |
| 3 边界 | 逐项通过 | 记录并定位到具体路径 |
| 4 Java 对齐 | 落在参考自身波动内且更稳定 | 检查符号规则、迭代次数、归一化口径 |
| 5 真实数据 | 页数/计数/耗时符合 | 检查页容量配置是否与导入时一致 |
| 6 持久化 | 结果不变 | 检查元数据是否真的落盘 |
| 7 性能 | 无数量级异常 | 记录，进入实验前排查 |

**只有 0–6 全部通过，才允许进入 §8.4 正式计时。**

---

## 失败排查对照表

| 现象 | 首先检查 |
|---|---|
| `metadata magic mismatch` | 元数据列是否被压缩；读取是否 detoast |
| 两平台结果不同 | `vendor/core` 哈希是否与 RocksDB 仓库一致 |
| `pages_corrected` 恒为 0 但区间是部分覆盖 | 页时间范围是否写入正确（`ts_page.t_start/t_end`） |
| `raw_points` 等于全量 | 是否误走了 `*_raw` 路径 |
| 中心全为 NaN | 输入含常量序列；参考实现同样会产生 NaN |
| 块级计数为 0 | 导入时 `chunk_pages` 与查询时是否一致 |
| PG 查询报 `no metadata` | 是否执行过 `indb_tsc_build` |
