
import csv
from pathlib import Path

raw_dir = Path("dataset/raw")
processed_dir = Path("dataset/processed")
processed_dir.mkdir(parents=True, exist_ok=True)

traffic_levels = ["low", "medium", "high"]

def combine_files(file_type, output_name):
    combined_rows = []

    for level in traffic_levels:
        filename = raw_dir / f"dataset_{level}_{file_type}.csv"

        if not filename.exists():
            print(f"Missing file: {filename}")
            continue

        with open(filename, "r", newline="") as file:
            reader = csv.DictReader(file)

            for row in reader:
                row["traffic_level"] = level
                combined_rows.append(row)

    if not combined_rows:
        print(f"No data found for {file_type}")
        return

    output_file = processed_dir / output_name
    fieldnames = list(combined_rows[0].keys())

    with open(output_file, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(combined_rows)

    print(f"\nCreated: {output_file}")
    print(f"Total data rows: {len(combined_rows)}")
    print(f"Columns: {', '.join(fieldnames)}")

combine_files("network", "all_flows.csv")
combine_files("monitoring", "all_monitoring.csv")
