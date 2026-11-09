# Reproducible build/run environment for the AI-Adaptive-Routing project.
# Contains ns-3.47 (only the modules this project needs) plus Python with
# scikit-learn, so the simulator can call the ML model directly.
#
# Build:  docker build -t ai-routing -f docker/Dockerfile docker
# (run_pipeline.py does this for you)

FROM python:3.12-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir scikit-learn==1.5.2 pandas==2.2.3 joblib==1.4.2 \
        matplotlib==3.9.2

ARG NS3_VERSION=ns-3.47
RUN git clone --depth 1 --branch ${NS3_VERSION} \
        https://gitlab.com/nsnam/ns-3-dev.git /opt/ns3

WORKDIR /opt/ns3
RUN ./ns3 configure --build-profile=optimized --disable-tests --disable-examples \
        --disable-python-bindings \
        --enable-modules="core;network;internet;point-to-point;applications;internet-apps;flow-monitor;traffic-control" \
    && ./ns3 build

# The project is mounted at /project at run time.
WORKDIR /project
