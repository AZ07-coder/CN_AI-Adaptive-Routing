# AI-Based Adaptive Routing in Computer Networks Using Machine Learning

## Introduction
AI-Based Adaptive Routing is a network-routing project that explores how Machine Learning can help networks make better routing decisions as traffic conditions change. Traditional routing generally follows configured routing rules, while an adaptive approach can use network-performance information to help select efficient paths.

## Project Description
The project models a computer network in NS-3, a network simulator, and generates TCP and UDP traffic across a topology containing 10 routers and 10 hosts. The simulation collects network-performance information such as packets sent and received, packet loss, delay, throughput, jitter, and queue conditions. Python tools are used to process and analyze the resulting data, and Scikit-learn is used to experiment with Machine Learning models that estimate a routing-cost score from network metrics.

The intended adaptive-routing workflow is to observe network conditions, evaluate available routing choices using measured information, and select a suitable path when conditions change. The system is evaluated against traditional routing using consistent metrics, with the aim of reducing delay and packet loss while improving throughput and overall network efficiency.

## How It Works
1. **Network simulation:** NS-3 represents routers, hosts, communication links, and network traffic.
2. **Traffic generation:** TCP and UDP applications create traffic under different load conditions.
3. **Performance monitoring:** FlowMonitor and monitoring outputs provide measurements such as packet delivery, delay, throughput, and jitter.
4. **Data analysis and Machine Learning:** Python, Pandas, and Scikit-learn are used to prepare the dataset, examine network performance, and experiment with a preliminary routing-cost model.
5. **Adaptive routing evaluation:** Routing decisions can be assessed using network measurements, then compared with the traditional-routing baseline in repeatable simulations.

## Running the complete pipeline

One command runs everything, from simulation through training to evaluation:

```bash
python run_pipeline.py            # full run (~132 simulations)
python run_pipeline.py --quick    # small smoke test
python run_pipeline.py --stages train eval report   # selected stages only
```

If ns-3 is not installed locally, the script automatically builds and uses a
Docker image (`docker/Dockerfile`: ns-3.47 + Python + scikit-learn), so the only
requirement on Windows is Docker Desktop. On Linux/WSL with ns-3 installed, set
`NS3_DIR=/path/to/ns-3` to run natively.

| Stage | What it does | Output |
|-------|--------------|--------|
| build | compiles `ns3/adaptive_topology.cc` | ns-3 binary |
| collect | rule-based adaptive runs on training seeds 1-20 | `results/pipeline/train/` |
| train | compares majority / logistic / tree / forest models, split by run | `ml/route_selector.pkl`, `results/pipeline/training/` |
| eval | baseline vs adaptive-rule vs adaptive-ML on unseen seeds 1001-1008 | `results/pipeline/eval/` |
| report | aggregates metrics and produces a plot | `results/pipeline/evaluation/report.md`, `comparison.png` |

Simulator options (`adaptive_topology --help`): `--routingMode=baseline|adaptive`,
`--decisionMode=rule|ml`, `--trafficLevel=low|medium|high`, `--seed`,
`--outputPrefix`, `--pythonCmd`, `--predictScript`, `--modelPath`,
`--hysteresis`, `--decisionInterval`, `--crossTraffic`.

The ML model is called at each decision via `ml/predict_route.py`. If the call
fails, the simulator logs a warning and falls back to the queue rule
(`decision_source=fallback` in `adaptive_route_log.csv`), so a run never crashes.

**Interpreting the results:** training labels come from the hysteresis queue rule,
so the classifier's accuracy measures how well it imitates the rule. The routing
benefit is measured separately by the evaluation stage on held-out seeds.

## Technologies
- **NS-3 and C++:** network topology and simulation
- **Python:** data processing and model experimentation
- **Pandas:** dataset preparation and analysis
- **Scikit-learn:** Machine Learning
- **Jupyter Notebook / JupyterLab:** experiments and results analysis
- **CSV:** storage of simulation measurements and predictions

## Project Structure
```text
AI-Adaptive-Routing/
├── ns3/
│   ├── topology.cc
│   ├── adaptive_topology.cc
│   ├── routing-table.txt
│   └── topology_day7_backup.bak
├── dataset/
│   ├── raw/
│   └── processed/
│       └── flow_analysis.csv
├── ml/
│   └── route_cost_model.pkl
├── routing/
├── results/
│   └── csv/
│       └── route_cost_predictions.csv
├── dashboard/
├── research/
├── report/
├── 01_Dataset_Analysis_and_ML.ipynb
├── 02_AI_Routing_Engine.ipynb
└── 03_NS3_Adaptive_Routing.ipynb
```

## Expected Outcome
The project is designed to demonstrate how Machine Learning and simulation-based network monitoring can support adaptive routing. Performance is assessed through measurable indicators—packet loss, delay, throughput, and jitter—rather than assumed improvements. A fair comparison uses the same topology and traffic conditions for traditional and adaptive routing.

## Scope and Interpretation
The Machine Learning model currently represents a preliminary experiment with a routing-cost score derived from network metrics. Such a prediction alone does not prove that a route is optimal or that NS-3 is dynamically changing paths. A complete adaptive-routing demonstration requires the model or routing logic to influence actual simulator route selection and requires reproducible baseline-versus-adaptive measurements.

## Conclusion
AI-Based Adaptive Routing combines network simulation, traffic monitoring, data analysis, and Machine Learning to investigate smarter routing decisions. By using observed network conditions and evaluating results with consistent performance metrics, the project aims to explore ways to make computer networks more responsive, efficient, and reliable.
