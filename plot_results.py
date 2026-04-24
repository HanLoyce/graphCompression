import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

BASE_COLUMNS = ["Dataset", "Algorithm", "Data_MB", "BPE", "Correct"]
ALGO_ORDER = [
    "Uncompressed (32-bit)",
    "SOTA",
    "Your Algorithm (Tree+VB)",
    "Your Algorithm (Tree+NoVB+NewTree)",
]
DISPLAY_NAMES = {
    "Uncompressed (32-bit)": "Uncompressed (32-bit)",
    "SOTA": "SOTA",
    "Your Algorithm (Tree+VB)": "Tree+VB",
    "Your Algorithm (Tree+NoVB+NewTree)": "Tree+NoVB+NewTree",
}


def load_benchmark_csv(csv_path: Path) -> pd.DataFrame:
    """Load benchmark CSV and normalize schema.

    Supports both old format (with MEPS) and new format (without MEPS).
    """
    rows = []
    bad_rows = 0

    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            return pd.DataFrame(columns=BASE_COLUMNS)

        has_meps = "MEPS" in header
        expected_len = 6 if has_meps else 5

        for row in reader:
            if not row:
                continue

            if len(row) == expected_len:
                rows.append(row)
                continue

            if len(row) > expected_len:
                # Repair rows where Data_MB contains thousand separators.
                dataset = row[0]
                algorithm = row[1]
                correct = row[-1]
                bpe = row[-2] if not has_meps else row[-3]
                data_mb = "".join(part.strip() for part in row[2 : -2 if not has_meps else -3])

                if has_meps:
                    meps = row[-2]
                    rows.append([dataset, algorithm, data_mb, bpe, meps, correct])
                else:
                    rows.append([dataset, algorithm, data_mb, bpe, correct])
                bad_rows += 1
                continue

            bad_rows += 1

    if bad_rows > 0:
        print(f"Warning: auto-repaired/skipped {bad_rows} malformed rows in {csv_path.name}.")

    df = pd.DataFrame(rows, columns=header)

    for col in ["Data_MB", "BPE", "Correct"]:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    df = df.dropna(subset=["Dataset", "Algorithm", "BPE"])

    # Keep only normalized columns needed for table/plot.
    for col in BASE_COLUMNS:
        if col not in df.columns:
            df[col] = np.nan

    return df[BASE_COLUMNS]


def load_external_sota_csv(csv_path: Path) -> pd.DataFrame:
    """Load optional external SOTA data.

    Supported schemas:
    1) Full schema like benchmark: Dataset,Algorithm,Data_MB,BPE,(MEPS),Correct
    2) Minimal schema: Dataset,BPE
    """
    if not csv_path.exists():
        print(f"Info: external SOTA file not found, skip merge: {csv_path}")
        return pd.DataFrame(columns=BASE_COLUMNS)

    raw = pd.read_csv(csv_path)
    raw_cols = {c.strip(): c for c in raw.columns}

    # Allow ratio-only external data. Ratio is percentage of uncompressed (32-bit).
    if "BPE" not in raw_cols:
        ratio_keys = ["Ratio", "RatioPercent", "CompressionRatio", "Compression_Ratio", "Ratio_%"]
        ratio_col = next((k for k in ratio_keys if k in raw_cols), None)
        if ratio_col is not None and "Dataset" in raw_cols:
            ratio_series = pd.to_numeric(raw[raw_cols[ratio_col]], errors="coerce")
            bpe_series = ratio_series * 32.0 / 100.0
            df = pd.DataFrame(
                {
                    "Dataset": raw[raw_cols["Dataset"]],
                    "Algorithm": "SOTA",
                    "Data_MB": np.nan,
                    "BPE": bpe_series,
                    "Correct": 1,
                }
            )
            return df.dropna(subset=["Dataset", "BPE"])

    if "Dataset" in raw_cols and "BPE" in raw_cols:
        # Minimal schema: auto-tag as SOTA.
        if "Algorithm" not in raw_cols:
            df = pd.DataFrame(
                {
                    "Dataset": raw[raw_cols["Dataset"]],
                    "Algorithm": "SOTA",
                    "Data_MB": np.nan,
                    "BPE": pd.to_numeric(raw[raw_cols["BPE"]], errors="coerce"),
                    "Correct": 1,
                }
            )
            return df.dropna(subset=["Dataset", "BPE"])

        # Full schema or mixed schema.
        for col in BASE_COLUMNS:
            if col not in raw.columns:
                raw[col] = np.nan

        raw["BPE"] = pd.to_numeric(raw["BPE"], errors="coerce")
        raw["Data_MB"] = pd.to_numeric(raw["Data_MB"], errors="coerce")
        raw["Correct"] = pd.to_numeric(raw["Correct"], errors="coerce")

        df = raw[BASE_COLUMNS].dropna(subset=["Dataset", "Algorithm", "BPE"])
        # Keep only SOTA rows from external file to avoid accidental override of your runs.
        df = df[df["Algorithm"].astype(str).str.upper().str.contains("SOTA")].copy()
        df["Algorithm"] = "SOTA"
        return df

    print(f"Warning: unsupported SOTA schema in {csv_path}. Expected columns include Dataset and BPE.")
    return pd.DataFrame(columns=BASE_COLUMNS)


def merge_results(base_df: pd.DataFrame, sota_df: pd.DataFrame) -> pd.DataFrame:
    # Remove existing SOTA rows from base, then inject external SOTA for same datasets.
    base_no_sota = base_df[base_df["Algorithm"] != "SOTA"].copy()
    merged = pd.concat([base_no_sota, sota_df], ignore_index=True)

    merged["algo_rank"] = merged["Algorithm"].apply(
        lambda x: ALGO_ORDER.index(x) if x in ALGO_ORDER else len(ALGO_ORDER)
    )
    merged = merged.sort_values(["Dataset", "algo_rank", "BPE"], kind="stable").drop(columns=["algo_rank"])

    # Deduplicate by dataset+algorithm, keep last one (external SOTA wins when duplicated).
    merged = merged.drop_duplicates(subset=["Dataset", "Algorithm"], keep="last")
    return merged


def export_tables(df: pd.DataFrame):
    result_dir = Path(__file__).resolve().parent.parent / "result"
    result_dir.mkdir(parents=True, exist_ok=True)

    long_path = result_dir / "final_comparison_table.csv"
    df.to_csv(long_path, index=False)

    wide = df.pivot(index="Dataset", columns="Algorithm", values="BPE").reset_index()
    wide_path = result_dir / "final_comparison_table_wide.csv"
    wide.to_csv(wide_path, index=False)

    print(f"Generated table: {long_path}")
    print(f"Generated table: {wide_path}")


def plot_ratio(df: pd.DataFrame):
    result_dir = Path(__file__).resolve().parent.parent / "result"
    result_dir.mkdir(parents=True, exist_ok=True)

    datasets = df["Dataset"].dropna().unique()
    all_algos = set(df["Algorithm"].dropna().unique())
    algos = [a for a in ALGO_ORDER if a in all_algos]

    if "Uncompressed (32-bit)" not in all_algos:
        print("Warning: no Uncompressed (32-bit) baseline, skip ratio figure.")
        return

    baseline_bpe = (
        df[df["Algorithm"] == "Uncompressed (32-bit)"][ ["Dataset", "BPE"] ]
        .drop_duplicates("Dataset")
        .set_index("Dataset")["BPE"]
    )

    colors = {
        "Uncompressed (32-bit)": "#95a5a6",
        "SOTA": "#e74c3c",
        "Your Algorithm (Tree+VB)": "#2980b9",
        "Your Algorithm (Tree+NoVB+NewTree)": "#16a085",
    }
    markers = {
        "Uncompressed (32-bit)": "s",
        "SOTA": "^",
        "Your Algorithm (Tree+VB)": "o",
        "Your Algorithm (Tree+NoVB+NewTree)": "D",
    }

    plt.figure(figsize=(10, 6))

    for algo in algos:
        algo_data = df[df["Algorithm"] == algo].set_index("Dataset").reindex(datasets)
        ratio_values = algo_data["BPE"] / baseline_bpe.reindex(algo_data.index) * 100.0
        ratio_values = ratio_values.replace([np.inf, -np.inf], np.nan)

        plt.plot(
            algo_data.index,
            ratio_values,
            marker=markers.get(algo, "o"),
            markersize=8,
            linewidth=2.2,
            color=colors.get(algo, "#333333"),
            label=DISPLAY_NAMES.get(algo, algo),
        )

    plt.title("Compression Ratio Comparison (Lower is Better)", fontsize=16, fontweight="bold")
    plt.xlabel("Graph Datasets", fontsize=13)
    plt.ylabel("Compression Ratio (% of Uncompressed)", fontsize=13)
    plt.xticks(rotation=15)
    plt.grid(True, linestyle="--", alpha=0.6)
    plt.legend(fontsize=11)
    plt.tight_layout()
    figure_path = result_dir / "compression_ratio_comparison.png"
    plt.savefig(figure_path, dpi=300)
    print(f"Generated chart: {figure_path}")
    plt.show()


def main():
    workspace_root = Path(__file__).resolve().parent.parent
    base_csv = workspace_root / "result" / "benchmark_results_newtree.csv"
    sota_csv = workspace_root / "result" / "vssota.csv"

    if not base_csv.exists():
        print("Error: benchmark_results_newtree.csv not found. Run benchmark first.")
        return

    base_df = load_benchmark_csv(base_csv)
    if base_df.empty:
        print("Error: benchmark_results_newtree.csv has no usable records.")
        return

    sota_df = load_external_sota_csv(sota_csv)
    merged_df = merge_results(base_df, sota_df)

    export_tables(merged_df)
    plot_ratio(merged_df)


if __name__ == "__main__":
    main()
