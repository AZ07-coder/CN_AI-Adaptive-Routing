
import csv
from pathlib import Path

input_file = Path("dataset/processed/all_flows.csv")
output_file = Path("dataset/processed/flow_analysis.csv")

if not input_file.exists():
    print("Error: all_flows.csv not found.")
    print("Run prepare_dataset.py first.")
    raise SystemExit(1)

with open(input_file, "r", newline="") as file:
    rows = list(csv.DictReader(file))

if not rows:
    print("Error: The flow dataset is empty.")
    raise SystemExit(1)

def number(row, column):
    return float(row[column])

delays = [number(row, "delay") for row in rows]
losses = [number(row, "packets_lost") for row in rows]
sent = [number(row, "packets_sent") for row in rows]
received = [number(row, "packets_received") for row in rows]
throughputs = [number(row, "throughput") for row in rows]

with open(output_file, "w", newline="") as file:
    fieldnames = list(rows[0].keys()) + [
        "packet_delivery_ratio_percent",
        "route_cost"
    ]
    writer = csv.DictWriter(file, fieldnames=fieldnames)
    writer.writeheader()

    for row in rows:
        packets_sent = number(row, "packets_sent")
        packets_received = number(row, "packets_received")
        packet_loss = number(row, "packets_lost")
        delay = number(row, "delay")
        throughput = number(row, "throughput")

        if packets_sent > 0:
            pdr = 100.0 * packets_received / packets_sent
            loss_rate = packet_loss / packets_sent
        else:
            pdr = 0.0
            loss_rate = 0.0

        row["packet_delivery_ratio_percent"] = round(pdr, 4)

        # Normalize metrics against fixed reference values.
        # These are experimental scoring assumptions, not measured limits.
        delay_score = min(max(delay / 0.1, 0.0), 1.0)
        loss_score = min(max(loss_rate, 0.0), 1.0)
        throughput_score = min(max(throughput / 10.0, 0.0), 1.0)

        # Lower cost is better. Throughput is converted to a penalty,
        # so higher throughput reduces the total cost.
        cost = (
            0.40 * delay_score
            + 0.30 * loss_score
            + 0.30 * (1.0 - throughput_score)
        )

        row["route_cost"] = round(cost, 6)
        writer.writerow(row)

print("Dataset analysis completed.")
print("Output:", output_file)
print("Total flow rows:", len(rows))
print("Average delay:", round(sum(delays) / len(delays), 6))
print("Total packets sent:", int(sum(sent)))
print("Total packets received:", int(sum(received)))
print("Total packets lost:", int(sum(losses)))
print("Average throughput:", round(sum(throughputs) / len(throughputs), 6))
print("Average PDR (%):", round(
    100.0 * sum(received) / sum(sent), 4
) if sum(sent) else 0.0)
print("Average experimental route cost:", round(
    sum(float(row["route_cost"]) for row in rows) / len(rows), 6
))
