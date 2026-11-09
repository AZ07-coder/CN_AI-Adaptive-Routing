// AI-Based Adaptive Routing - NS-3 simulation
//
// Topology: 10 routers (R0..R9), 10 hosts (H1..H10, Hk attached to R(k-1)).
// Main flows: UDP H1 -> H10 and TCP H2 -> H9.
// Cross traffic: random bursty UDP on R1->R3 (Path A branch) and on
// R2->R3 (Path B branch), so the two candidate paths are congested at
// different times and route choice actually matters.
//
// routingMode=baseline : ns-3 global (shortest-path) routing only.
// routingMode=adaptive : every decisionInterval seconds, measure the two
//                        candidate paths and install static host routes
//                        for H10 along the chosen one.
//   decisionMode=rule  : queue rule with hysteresis (used to label data).
//   decisionMode=ml    : ask the trained Python model (ml/predict_route.py);
//                        falls back to the rule if the model call fails.
//
// Path A: R0-R1-R3-R5-R7-R9     Path B: R0-R2-R3-R5-R7-R9

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/ipv4-static-routing.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

using namespace ns3;

// ---------------------------------------------------------------- settings
static std::string g_outputPrefix = "new_run_";
static std::string g_decisionMode = "ml";
static std::string g_pythonCmd = "python3";
static std::string g_predictScript = "ml/predict_route.py";
static std::string g_modelPath = "ml/route_selector.pkl";
static uint32_t g_hysteresis = 20; // packets
static double g_decisionInterval = 1.0;
static double g_decisionStart = 3.0;
static double g_decisionStop = 19.0;

static std::string OutputFile(const std::string& filename)
{
    return g_outputPrefix + filename;
}

// ------------------------------------------------- link byte counters
// Counters 0..7: both directions of d1 (R0-R1), d2 (R0-R2),
// d3 (R1-R3, Path A branch) and d4 (R2-R3, Path B branch).
static constexpr uint64_t LINK_CAPACITY_BPS = 10000000ULL;
static constexpr uint32_t N_COUNTERS = 8;
static uint64_t g_linkTxBytes[N_COUNTERS] = {};
static uint64_t g_monitorSnapshot[N_COUNTERS] = {};
static uint64_t g_decisionSnapshot[N_COUNTERS] = {};
static double g_lastMonitorTime = 2.0;
static double g_lastDecisionTime = 0.0;

static void CountLinkTxBytes(uint32_t counterIndex, Ptr<const Packet> packet)
{
    if (counterIndex < N_COUNTERS && packet)
    {
        g_linkTxBytes[counterIndex] += packet->GetSize();
    }
}

// Utilisation (%) of a duplex link (two counters) since a snapshot.
static double LinkUtilization(uint32_t firstCounter, const uint64_t* snapshot, double seconds)
{
    if (seconds <= 0.0)
    {
        return 0.0;
    }
    const uint64_t bytes = (g_linkTxBytes[firstCounter] - snapshot[firstCounter]) +
                           (g_linkTxBytes[firstCounter + 1] - snapshot[firstCounter + 1]);
    const double percent = bytes * 8.0 / (2.0 * LINK_CAPACITY_BPS * seconds) * 100.0;
    return percent > 100.0 ? 100.0 : percent;
}

// ------------------------------------------------------------- topology
struct Topology
{
    NodeContainer routers;
    std::vector<NetDeviceContainer> links;
    std::vector<Ipv4InterfaceContainer> interfaces;
};

static Topology g_topo;
static std::string g_currentPath = "None";

static uint32_t LinkQueue(size_t linkIndex)
{
    uint32_t total = 0;
    for (uint32_t side = 0; side < 2; ++side)
    {
        Ptr<PointToPointNetDevice> dev =
            DynamicCast<PointToPointNetDevice>(g_topo.links[linkIndex].Get(side));
        if (!dev)
        {
            continue;
        }
        if (dev->GetQueue())
        {
            total += dev->GetQueue()->GetNPackets();
        }
        // Include the traffic-control (qdisc) backlog as well.
        Ptr<TrafficControlLayer> tc = dev->GetNode()->GetObject<TrafficControlLayer>();
        if (tc)
        {
            Ptr<QueueDisc> qd = tc->GetRootQueueDiscOnDevice(dev);
            if (qd)
            {
                total += qd->GetNPackets();
            }
        }
    }
    return total;
}

// ------------------------------------------------------- monitoring
void MonitorQueues()
{
    std::ofstream file(OutputFile("monitoring_data.csv"), std::ios::app);
    const double time = Simulator::Now().GetSeconds();

    uint32_t totalQueuePackets = 0;
    for (size_t k = 0; k < g_topo.links.size(); ++k)
    {
        totalQueuePackets += LinkQueue(k);
    }
    const double averageQueue =
        static_cast<double>(totalQueuePackets) / (2.0 * g_topo.links.size());

    const double interval = time - g_lastMonitorTime;
    const double d1 = LinkUtilization(0, g_monitorSnapshot, interval);
    const double d2 = LinkUtilization(2, g_monitorSnapshot, interval);

    file << std::fixed << std::setprecision(6) << time << "," << totalQueuePackets << ","
         << averageQueue << "," << d1 << "," << d2 << "," << (d1 + d2) / 2.0 << "\n";

    for (uint32_t i = 0; i < N_COUNTERS; ++i)
    {
        g_monitorSnapshot[i] = g_linkTxBytes[i];
    }
    g_lastMonitorTime = time;

    Simulator::Schedule(Seconds(1), &MonitorQueues);
}

// ------------------------------------------------------- decisions
static std::string RuleDecision(uint32_t queueA, uint32_t queueB)
{
    if (g_currentPath == "Path_A")
    {
        return (queueB + g_hysteresis < queueA) ? "Path_B" : "Path_A";
    }
    if (g_currentPath == "Path_B")
    {
        return (queueA + g_hysteresis < queueB) ? "Path_A" : "Path_B";
    }
    return (queueA <= queueB) ? "Path_A" : "Path_B";
}

// Returns "Path_A"/"Path_B" from the Python model, or "" on any failure.
static std::string MlDecision(uint32_t queueA,
                              uint32_t queueB,
                              double utilA,
                              double utilB,
                              std::string& rawOutput)
{
    std::ostringstream cmd;
    cmd << g_pythonCmd << " \"" << g_predictScript << "\" --model \"" << g_modelPath << "\" "
        << queueA << " " << queueB << " " << std::fixed << std::setprecision(4) << utilA << " "
        << utilB << " " << g_currentPath << " 2>&1";

    FILE* pipe = POPEN(cmd.str().c_str(), "r");
    if (pipe == nullptr)
    {
        rawOutput = "could not start python";
        return "";
    }
    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    {
        rawOutput += buffer.data();
    }
    const int exitCode = PCLOSE(pipe);

    const bool a = rawOutput.find("\"selected_path\": \"Path_A\"") != std::string::npos;
    const bool b = rawOutput.find("\"selected_path\": \"Path_B\"") != std::string::npos;
    if (exitCode != 0 || a == b)
    {
        return "";
    }
    return a ? "Path_A" : "Path_B";
}

static bool InstallPath(const std::vector<uint32_t>& path, const Ipv4Address& destination)
{
    Ipv4StaticRoutingHelper staticHelper;

    // Remove previous static routes to the destination.
    for (uint32_t r = 0; r < g_topo.routers.GetN(); ++r)
    {
        Ptr<Ipv4StaticRouting> routing =
            staticHelper.GetStaticRouting(g_topo.routers.Get(r)->GetObject<Ipv4>());
        for (uint32_t i = routing->GetNRoutes(); i > 0;)
        {
            --i;
            if (routing->GetRoute(i).GetDest() == destination)
            {
                routing->RemoveRoute(i);
            }
        }
    }

    for (size_t i = 0; i + 1 < path.size(); ++i)
    {
        const uint32_t currentId = g_topo.routers.Get(path[i])->GetId();
        const uint32_t nextId = g_topo.routers.Get(path[i + 1])->GetId();
        bool found = false;

        for (size_t k = 0; k < g_topo.links.size() && !found; ++k)
        {
            Ptr<NetDevice> first = g_topo.links[k].Get(0);
            Ptr<NetDevice> second = g_topo.links[k].Get(1);
            Ptr<NetDevice> localDevice;
            Ipv4Address nextHop;

            if (first->GetNode()->GetId() == currentId && second->GetNode()->GetId() == nextId)
            {
                localDevice = first;
                nextHop = g_topo.interfaces[k].GetAddress(1);
            }
            else if (second->GetNode()->GetId() == currentId &&
                     first->GetNode()->GetId() == nextId)
            {
                localDevice = second;
                nextHop = g_topo.interfaces[k].GetAddress(0);
            }
            else
            {
                continue;
            }

            Ptr<Ipv4> ipv4 = g_topo.routers.Get(path[i])->GetObject<Ipv4>();
            const int32_t ifIndex = ipv4->GetInterfaceForDevice(localDevice);
            if (ifIndex < 0)
            {
                std::cerr << "ERROR: interface not found on router " << path[i] << std::endl;
                return false;
            }
            staticHelper.GetStaticRouting(ipv4)->AddHostRouteTo(destination,
                                                                nextHop,
                                                                static_cast<uint32_t>(ifIndex));
            found = true;
        }

        if (!found)
        {
            std::cerr << "ERROR: no link between routers " << path[i] << " and " << path[i + 1]
                      << std::endl;
            return false;
        }
    }
    return true;
}

void ApplyAdaptiveRoute()
{
    const double now = Simulator::Now().GetSeconds();

    // Branch links that differ between the paths: d3 (R1-R3) vs d4 (R2-R3).
    const uint32_t queueA = LinkQueue(2);
    const uint32_t queueB = LinkQueue(3);
    const double interval = now - g_lastDecisionTime;
    const double utilA = LinkUtilization(4, g_decisionSnapshot, interval);
    const double utilB = LinkUtilization(6, g_decisionSnapshot, interval);
    for (uint32_t i = 0; i < N_COUNTERS; ++i)
    {
        g_decisionSnapshot[i] = g_linkTxBytes[i];
    }
    g_lastDecisionTime = now;

    const std::string ruleChoice = RuleDecision(queueA, queueB);
    std::string selected = ruleChoice;
    std::string source = "rule";

    if (g_decisionMode == "ml")
    {
        std::string raw;
        const std::string mlChoice = MlDecision(queueA, queueB, utilA, utilB, raw);
        if (!mlChoice.empty())
        {
            selected = mlChoice;
            source = "ml";
        }
        else
        {
            source = "fallback";
            std::cerr << "WARNING: ML call failed at t=" << now << " (" << raw
                      << "). Using rule: " << ruleChoice << std::endl;
        }
    }

    const std::vector<uint32_t> path = (selected == "Path_A")
                                           ? std::vector<uint32_t>{0, 1, 3, 5, 7, 9}
                                           : std::vector<uint32_t>{0, 2, 3, 5, 7, 9};

    if (InstallPath(path, Ipv4Address("10.1.10.2")))
    {
        std::ofstream log(OutputFile("adaptive_route_log.csv"), std::ios::app);
        log << std::fixed << std::setprecision(3) << now << "," << queueA << "," << queueB << ","
            << utilA << "," << utilB << "," << g_currentPath << "," << selected << "," << source
            << "\n";
        g_currentPath = selected;
    }

    if (now + g_decisionInterval <= g_decisionStop + 1e-9)
    {
        Simulator::Schedule(Seconds(g_decisionInterval), &ApplyAdaptiveRoute);
    }
}

// ------------------------------------------------------------- traffic
// Optional per-packet trace of the main UDP flow (--packetTrace=1).
static std::ofstream g_packetTrace;

static void TracePacket(Ptr<const Packet>, const Address&, const Address&, const SeqTsSizeHeader& h)
{
    const double now = Simulator::Now().GetSeconds();
    g_packetTrace << std::fixed << std::setprecision(6) << h.GetSeq() << ","
                  << h.GetTs().GetSeconds() << "," << now << "," << now - h.GetTs().GetSeconds()
                  << "\n";
}

static void InstallUdpFlow(Ptr<Node> src,
                           Ptr<Node> dst,
                           Ipv4Address dstAddr,
                           uint16_t port,
                           const std::string& rate,
                           const std::string& onTime,
                           const std::string& offTime,
                           double start,
                           double stop,
                           bool trace = false)
{
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    if (trace)
    {
        sink.SetAttribute("EnableSeqTsSizeHeader", BooleanValue(true));
    }
    ApplicationContainer s = sink.Install(dst);
    if (trace)
    {
        s.Get(0)->TraceConnectWithoutContext("RxWithSeqTsSize", MakeCallback(&TracePacket));
    }
    s.Start(Seconds(1));
    s.Stop(Seconds(stop));

    OnOffHelper client("ns3::UdpSocketFactory", InetSocketAddress(dstAddr, port));
    client.SetAttribute("DataRate", DataRateValue(DataRate(rate)));
    client.SetAttribute("PacketSize", UintegerValue(1024));
    client.SetAttribute("OnTime", StringValue(onTime));
    client.SetAttribute("OffTime", StringValue(offTime));
    if (trace)
    {
        client.SetAttribute("EnableSeqTsSizeHeader", BooleanValue(true));
    }
    ApplicationContainer c = client.Install(src);
    c.Start(Seconds(start));
    c.Stop(Seconds(stop));
}

int main(int argc, char* argv[])
{
    std::string trafficLevel = "low";
    std::string routingMode = "adaptive";
    uint32_t seed = 1;
    bool crossTraffic = true;
    bool packetTrace = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("trafficLevel", "Traffic level: low, medium, high", trafficLevel);
    cmd.AddValue("routingMode", "baseline or adaptive", routingMode);
    cmd.AddValue("decisionMode", "Adaptive decision maker: rule or ml", g_decisionMode);
    cmd.AddValue("seed", "Run number for the random streams", seed);
    cmd.AddValue("outputPrefix", "Output path/prefix (directory must exist)", g_outputPrefix);
    cmd.AddValue("pythonCmd", "Python interpreter used for ML decisions", g_pythonCmd);
    cmd.AddValue("predictScript", "Path to ml/predict_route.py", g_predictScript);
    cmd.AddValue("modelPath", "Path to the trained route selector .pkl", g_modelPath);
    cmd.AddValue("hysteresis", "Rule: queue margin (packets) needed to switch", g_hysteresis);
    cmd.AddValue("decisionInterval", "Seconds between adaptive decisions", g_decisionInterval);
    cmd.AddValue("crossTraffic", "Enable bursty cross traffic on the path branches", crossTraffic);
    cmd.AddValue("packetTrace", "Write per-packet trace of the main UDP flow", packetTrace);
    cmd.Parse(argc, argv);

    if (trafficLevel != "low" && trafficLevel != "medium" && trafficLevel != "high")
    {
        std::cerr << "Invalid trafficLevel. Use low, medium, or high." << std::endl;
        return 1;
    }
    if (routingMode != "baseline" && routingMode != "adaptive")
    {
        std::cerr << "Invalid routingMode. Use baseline or adaptive." << std::endl;
        return 1;
    }
    if (g_decisionMode != "rule" && g_decisionMode != "ml")
    {
        std::cerr << "Invalid decisionMode. Use rule or ml." << std::endl;
        return 1;
    }

    // Same base seed, different run number => independent, reproducible runs.
    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(seed);

    NodeContainer routers;
    routers.Create(10);
    NodeContainer hosts;
    hosts.Create(10);

    InternetStackHelper internet;
    internet.Install(routers);
    internet.Install(hosts);

    // Router links: 10 Mbps, 5 ms.
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));

    const std::vector<std::pair<uint32_t, uint32_t>> edges = {
        {0, 1}, {0, 2}, {1, 3}, {2, 3}, {1, 4}, {3, 5}, {4, 5},
        {4, 6}, {5, 7}, {6, 7}, {6, 8}, {7, 9}, {8, 9}};

    std::vector<NetDeviceContainer> links;
    for (const auto& e : edges)
    {
        links.push_back(p2p.Install(routers.Get(e.first), routers.Get(e.second)));
    }

    // Byte counters on d1, d2, d3, d4 (both directions).
    for (uint32_t l = 0; l < 4; ++l)
    {
        for (uint32_t side = 0; side < 2; ++side)
        {
            links[l].Get(side)->TraceConnectWithoutContext(
                "PhyTxEnd",
                MakeBoundCallback(&CountLinkTxBytes, 2 * l + side));
        }
    }

    // Host links: 100 Mbps, 2 ms. Hk attached to R(k-1).
    PointToPointHelper hostP2P;
    hostP2P.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    hostP2P.SetChannelAttribute("Delay", StringValue("2ms"));

    std::vector<NetDeviceContainer> hostLinks;
    for (uint32_t i = 0; i < 10; ++i)
    {
        hostLinks.push_back(hostP2P.Install(routers.Get(i), hosts.Get(i)));
    }

    Ipv4AddressHelper address;
    std::vector<Ipv4InterfaceContainer> interfaces;
    for (size_t i = 0; i < links.size(); ++i)
    {
        address.SetBase(("10.0." + std::to_string(i + 1) + ".0").c_str(), "255.255.255.0");
        interfaces.push_back(address.Assign(links[i]));
    }
    std::vector<Ipv4InterfaceContainer> hostIfs;
    for (uint32_t i = 0; i < 10; ++i)
    {
        address.SetBase(("10.1." + std::to_string(i + 1) + ".0").c_str(), "255.255.255.0");
        hostIfs.push_back(address.Assign(hostLinks[i]));
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ipv4GlobalRoutingHelper routingHelper;
    routingHelper.PrintRoutingTableAllAt(
        Seconds(1),
        Create<OutputStreamWrapper>(OutputFile("routing-table.txt"), std::ios::out));
    routingHelper.PrintRoutingTableAllAt(
        Seconds(11),
        Create<OutputStreamWrapper>(OutputFile("routing-table-adaptive.txt"), std::ios::out));

    std::string rate = "5Mbps";
    if (trafficLevel == "medium")
    {
        rate = "20Mbps";
    }
    else if (trafficLevel == "high")
    {
        rate = "40Mbps";
    }

    const std::string onOneSec = "ns3::ConstantRandomVariable[Constant=1.0]";
    const std::string onJitter = "ns3::UniformRandomVariable[Min=0.5|Max=1.5]";

    // Main UDP flow: H1 -> H10.
    if (packetTrace)
    {
        g_packetTrace.open(OutputFile("udp_packet_trace.csv"));
        g_packetTrace << "seq,tx_time,rx_time,delay\n";
    }
    InstallUdpFlow(hosts.Get(0),
                   hosts.Get(9),
                   hostIfs[9].GetAddress(1),
                   5000,
                   rate,
                   onJitter,
                   onJitter,
                   2.0,
                   20.0,
                   packetTrace);

    // Main TCP flow: H2 -> H9.
    {
        PacketSinkHelper tcpSink("ns3::TcpSocketFactory",
                                 InetSocketAddress(Ipv4Address::GetAny(), 5001));
        ApplicationContainer s = tcpSink.Install(hosts.Get(8));
        s.Start(Seconds(1));
        s.Stop(Seconds(20));

        OnOffHelper tcpClient("ns3::TcpSocketFactory",
                              InetSocketAddress(hostIfs[8].GetAddress(1), 5001));
        tcpClient.SetAttribute("DataRate", DataRateValue(DataRate(rate)));
        tcpClient.SetAttribute("PacketSize", UintegerValue(1024));
        tcpClient.SetAttribute("OnTime", StringValue(onOneSec));
        tcpClient.SetAttribute("OffTime", StringValue(onOneSec));
        ApplicationContainer c = tcpClient.Install(hosts.Get(1));
        c.Start(Seconds(3));
        c.Stop(Seconds(20));
    }

    // Bursty cross traffic: H2 (R1) -> H4 (R3) loads the Path A branch,
    // H3 (R2) -> H4 (R3) loads the Path B branch. Independent random bursts.
    if (crossTraffic)
    {
        const std::string burstOn = "ns3::ExponentialRandomVariable[Mean=2.5|Bound=8]";
        const std::string burstOff = "ns3::ExponentialRandomVariable[Mean=2.5|Bound=8]";
        InstallUdpFlow(hosts.Get(1),
                       hosts.Get(3),
                       hostIfs[3].GetAddress(1),
                       6000,
                       "9Mbps",
                       burstOn,
                       burstOff,
                       2.0,
                       20.0);
        InstallUdpFlow(hosts.Get(2),
                       hosts.Get(3),
                       hostIfs[3].GetAddress(1),
                       6001,
                       "9Mbps",
                       burstOn,
                       burstOff,
                       2.0,
                       20.0);
    }

    g_topo.routers = routers;
    g_topo.links = links;
    g_topo.interfaces = interfaces;

    {
        std::ofstream log(OutputFile("adaptive_route_log.csv"));
        log << "time,queue_A,queue_B,util_A,util_B,current_path,selected_path,decision_source\n";
    }
    {
        std::ofstream file(OutputFile("monitoring_data.csv"));
        file << "time,total_queue_packets,average_queue_packets,"
                "d1_utilization_percent,d2_utilization_percent,utilization_percent\n";
    }

    if (routingMode == "adaptive")
    {
        g_lastDecisionTime = g_decisionStart - g_decisionInterval;
        Simulator::Schedule(Seconds(g_decisionStart), &ApplyAdaptiveRoute);
    }

    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> monitor = flowHelper.InstallAll();

    Simulator::Schedule(Seconds(3), &MonitorQueues);

    // Let in-flight packets drain after the applications stop at 20 s.
    Simulator::Stop(Seconds(25));

    std::cout << "Starting simulation: trafficLevel=" << trafficLevel
              << ", routingMode=" << routingMode
              << (routingMode == "adaptive" ? ", decisionMode=" + g_decisionMode : "")
              << ", seed=" << seed << ", outputPrefix=" << g_outputPrefix << std::endl;

    Simulator::Run();

    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());

    std::ofstream csvFile(OutputFile("network_data.csv"));
    if (!csvFile.is_open())
    {
        std::cerr << "ERROR: Cannot create network_data.csv (does the output directory exist?)"
                  << std::endl;
        Simulator::Destroy();
        return 1;
    }

    csvFile << "flow_id,source,destination,dst_port,packets_sent,packets_received,packets_lost,"
               "delay,throughput,jitter\n";

    for (const auto& flow : monitor->GetFlowStats())
    {
        const Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(flow.first);
        const FlowMonitor::FlowStats& s = flow.second;

        const uint64_t lost = s.txPackets >= s.rxPackets ? s.txPackets - s.rxPackets : 0;
        const double delay = s.rxPackets > 0 ? s.delaySum.GetSeconds() / s.rxPackets : 0.0;
        const double span = s.timeLastRxPacket.GetSeconds() - s.timeFirstTxPacket.GetSeconds();
        const double throughput =
            (s.rxPackets > 0 && span > 0) ? s.rxBytes * 8.0 / span / 1e6 : 0.0;
        const double jitter =
            s.rxPackets > 1 ? s.jitterSum.GetSeconds() / (s.rxPackets - 1) : 0.0;

        csvFile << flow.first << "," << t.sourceAddress << "," << t.destinationAddress << ","
                << t.destinationPort << "," << s.txPackets << "," << s.rxPackets << "," << lost
                << "," << std::setprecision(10) << delay << "," << throughput << "," << jitter
                << "\n";
    }
    csvFile.close();

    std::cout << "Saved results with prefix " << g_outputPrefix << std::endl;
    Simulator::Destroy();
    return 0;
}
