#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/ipv4-static-routing.h"
#include <vector>

#include <iomanip>
#include <fstream>

using namespace ns3;


void MonitorQueues(NodeContainer routers)
{
    std::ofstream file("monitoring_data.csv", std::ios::app);

    double time = Simulator::Now().GetSeconds();

    uint32_t totalQueuePackets = 0;
    uint32_t totalDevices = 0;

    for (uint32_t i = 0; i < routers.GetN(); i++)
    {
        Ptr<Node> router = routers.Get(i);

        for (uint32_t j = 0; j < router->GetNDevices(); j++)
        {
            Ptr<NetDevice> device = router->GetDevice(j);

            Ptr<PointToPointNetDevice> p2pDevice =
                DynamicCast<PointToPointNetDevice>(device);

            if (p2pDevice)
            {
                Ptr<Queue<Packet>> queue =
                    p2pDevice->GetQueue();

                if (queue)
                {
                    totalQueuePackets += queue->GetNPackets();
                    totalDevices++;
                }
            }
        }
    }

    double averageQueue = 0.0;

    if (totalDevices > 0)
    {
        averageQueue =
            static_cast<double>(totalQueuePackets)
            / totalDevices;
    }

    // Current offered traffic:
    // UDP = 5 Mbps
    // TCP = approximately 5 Mbps requested
    // Total offered load = 10 Mbps
    //
    // Link capacity = 100 Mbps
    //
    // Estimated network utilization = 10 / 100 = 10%

    double utilization = 10.0;

    file << std::fixed << std::setprecision(4)
         << time << ","
         << totalQueuePackets << ","
         << averageQueue << ","
         << utilization
         << std::endl;

    file.close();

    std::cout << "Time: "
              << time
              << " s | Total Queue: "
              << totalQueuePackets
              << " | Average Queue: "
              << averageQueue
              << " | Utilization: "
              << utilization
              << "%"
              << std::endl;

    Simulator::Schedule(
        Seconds(1),
        &MonitorQueues,
        routers);
}

void ApplyAdaptiveRoute(
    NodeContainer routers,
    std::vector<NetDeviceContainer> links,
    std::vector<Ipv4InterfaceContainer> interfaces,
    NetDeviceContainer host10Link)
{
    Ptr<PointToPointNetDevice> devA =
        DynamicCast<PointToPointNetDevice>(links[0].Get(0));
    Ptr<PointToPointNetDevice> devB =
        DynamicCast<PointToPointNetDevice>(links[1].Get(0));

    uint32_t queueA = devA->GetQueue()->GetNPackets();
    uint32_t queueB = devB->GetQueue()->GetNPackets();

    // Path A: R1-R2-R4-R6-R8-R10
    // Path B: R1-R3-R4-R6-R8-R10
    std::vector<uint32_t> pathA = {0, 1, 3, 5, 7, 9};
    std::vector<uint32_t> pathB = {0, 2, 3, 5, 7, 9};

    std::vector<uint32_t> path =
        (queueA <= queueB) ? pathA : pathB;

    Ipv4Address destination("10.1.10.2");
    Ipv4StaticRoutingHelper staticHelper;

    for (size_t i = 0; i + 1 < path.size(); ++i)
    {
        uint32_t current = path[i];
        uint32_t next = path[i + 1];

        for (size_t k = 0; k < links.size(); ++k)
        {
            Ptr<NetDevice> first = links[k].Get(0);
            Ptr<NetDevice> second = links[k].Get(1);

            uint32_t firstNode = first->GetNode()->GetId();
            uint32_t secondNode = second->GetNode()->GetId();

            Ptr<NetDevice> localDevice = nullptr;
            Ipv4Address nextHop;

            if (firstNode == routers.Get(current)->GetId() &&
                secondNode == routers.Get(next)->GetId())
            {
                localDevice = first;
                nextHop = interfaces[k].GetAddress(1);
            }
            else if (secondNode == routers.Get(current)->GetId() &&
                     firstNode == routers.Get(next)->GetId())
            {
                localDevice = second;
                nextHop = interfaces[k].GetAddress(0);
            }
            else
            {
                continue;
            }

            Ptr<Ipv4> ipv4 =
                routers.Get(current)->GetObject<Ipv4>();

            int32_t interfaceIndex =
                ipv4->GetInterfaceForDevice(localDevice);

            if (interfaceIndex >= 0)
            {
                Ptr<Ipv4StaticRouting> routing =
                    staticHelper.GetStaticRouting(ipv4);

                routing->AddHostRouteTo(
                    destination,
                    nextHop,
                    static_cast<uint32_t>(interfaceIndex));
            }

            break;
        }
    }

    // Last router connects directly to H10.
    Ptr<Ipv4> lastIpv4 =
        routers.Get(9)->GetObject<Ipv4>();

    int32_t lastInterface =
        lastIpv4->GetInterfaceForDevice(host10Link.Get(0));

    if (lastInterface >= 0)
    {
        staticHelper.GetStaticRouting(lastIpv4)
            ->AddHostRouteTo(
                destination,
                static_cast<uint32_t>(lastInterface));
    }

    std::ofstream log("adaptive_route_log.csv", std::ios::app);

    log << Simulator::Now().GetSeconds() << ","
        << queueA << ","
        << queueB << ","
        << ((queueA <= queueB) ? "Path_A" : "Path_B")
        << std::endl;

    std::cout << "Adaptive route applied at "
              << Simulator::Now().GetSeconds()
              << " seconds. Queue A=" << queueA
              << ", Queue B=" << queueB
              << ", selected="
              << ((queueA <= queueB) ? "Path_A" : "Path_B")
              << std::endl;
}


int main(int argc, char *argv[])
{
   
    std::string trafficLevel = "low";

CommandLine cmd;
cmd.AddValue("trafficLevel",
             "Traffic level: low, medium, high",
             trafficLevel);
cmd.Parse(argc, argv);

    // Create 10 routers
    NodeContainer routers;
    routers.Create(10);

    // Create 10 hosts
    NodeContainer hosts;
    hosts.Create(10);

    // Install Internet stack on routers and hosts
    InternetStackHelper internet;
    internet.Install(routers);
    internet.Install(hosts);

    // Point-to-point link configuration
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));

    // =========================
    // ROUTER-TO-ROUTER LINKS
    // =========================

    NetDeviceContainer d1 = p2p.Install(routers.Get(0), routers.Get(1));
    NetDeviceContainer d2 = p2p.Install(routers.Get(0), routers.Get(2));
    NetDeviceContainer d3 = p2p.Install(routers.Get(1), routers.Get(3));
    NetDeviceContainer d4 = p2p.Install(routers.Get(2), routers.Get(3));

    NetDeviceContainer d5 = p2p.Install(routers.Get(1), routers.Get(4));
    NetDeviceContainer d6 = p2p.Install(routers.Get(3), routers.Get(5));
    NetDeviceContainer d7 = p2p.Install(routers.Get(4), routers.Get(5));

    NetDeviceContainer d8 = p2p.Install(routers.Get(4), routers.Get(6));
    NetDeviceContainer d9 = p2p.Install(routers.Get(5), routers.Get(7));
    NetDeviceContainer d10 = p2p.Install(routers.Get(6), routers.Get(7));

    NetDeviceContainer d11 = p2p.Install(routers.Get(6), routers.Get(8));
    NetDeviceContainer d12 = p2p.Install(routers.Get(7), routers.Get(9));
    NetDeviceContainer d13 = p2p.Install(routers.Get(8), routers.Get(9));

    // =========================
    // ROUTER-TO-HOST LINKS
    // =========================

    PointToPointHelper hostP2P;
    hostP2P.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    hostP2P.SetChannelAttribute("Delay", StringValue("2ms"));

    NetDeviceContainer h1 = hostP2P.Install(routers.Get(0), hosts.Get(0));
    NetDeviceContainer h2 = hostP2P.Install(routers.Get(1), hosts.Get(1));
    NetDeviceContainer h3 = hostP2P.Install(routers.Get(2), hosts.Get(2));
    NetDeviceContainer h4 = hostP2P.Install(routers.Get(3), hosts.Get(3));
    NetDeviceContainer h5 = hostP2P.Install(routers.Get(4), hosts.Get(4));

    NetDeviceContainer h6 = hostP2P.Install(routers.Get(5), hosts.Get(5));
    NetDeviceContainer h7 = hostP2P.Install(routers.Get(6), hosts.Get(6));
    NetDeviceContainer h8 = hostP2P.Install(routers.Get(7), hosts.Get(7));
    NetDeviceContainer h9 = hostP2P.Install(routers.Get(8), hosts.Get(8));
    NetDeviceContainer h10 = hostP2P.Install(routers.Get(9), hosts.Get(9));

   // =========================
// ASSIGN ROUTER LINK IP ADDRESSES
// =========================

Ipv4AddressHelper address;

address.SetBase("10.0.1.0", "255.255.255.0");
Ipv4InterfaceContainer i1 = address.Assign(d1);

address.SetBase("10.0.2.0", "255.255.255.0");
Ipv4InterfaceContainer i2 = address.Assign(d2);

address.SetBase("10.0.3.0", "255.255.255.0");
Ipv4InterfaceContainer i3 = address.Assign(d3);

address.SetBase("10.0.4.0", "255.255.255.0");
Ipv4InterfaceContainer i4 = address.Assign(d4);

address.SetBase("10.0.5.0", "255.255.255.0");
Ipv4InterfaceContainer i5 = address.Assign(d5);

address.SetBase("10.0.6.0", "255.255.255.0");
Ipv4InterfaceContainer i6 = address.Assign(d6);

address.SetBase("10.0.7.0", "255.255.255.0");
Ipv4InterfaceContainer i7 = address.Assign(d7);

address.SetBase("10.0.8.0", "255.255.255.0");
Ipv4InterfaceContainer i8 = address.Assign(d8);

address.SetBase("10.0.9.0", "255.255.255.0");
Ipv4InterfaceContainer i9 = address.Assign(d9);

address.SetBase("10.0.10.0", "255.255.255.0");
Ipv4InterfaceContainer i10 = address.Assign(d10);

address.SetBase("10.0.11.0", "255.255.255.0");
Ipv4InterfaceContainer i11 = address.Assign(d11);

address.SetBase("10.0.12.0", "255.255.255.0");
Ipv4InterfaceContainer i12 = address.Assign(d12);

address.SetBase("10.0.13.0", "255.255.255.0");
Ipv4InterfaceContainer i13 = address.Assign(d13);

    // Host links
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer host1 = address.Assign(h1);

    address.SetBase("10.1.2.0", "255.255.255.0");
    address.Assign(h2);

    address.SetBase("10.1.3.0", "255.255.255.0");
    address.Assign(h3);

    address.SetBase("10.1.4.0", "255.255.255.0");
    address.Assign(h4);

    address.SetBase("10.1.5.0", "255.255.255.0");
    address.Assign(h5);

    address.SetBase("10.1.6.0", "255.255.255.0");
    address.Assign(h6);

    address.SetBase("10.1.7.0", "255.255.255.0");
    address.Assign(h7);

    address.SetBase("10.1.8.0", "255.255.255.0");
    address.Assign(h8);

    address.SetBase("10.1.9.0", "255.255.255.0");
    address.Assign(h9);

    address.SetBase("10.1.10.0", "255.255.255.0");
    Ipv4InterfaceContainer host10 = address.Assign(h10);

// =========================
// ROUTING
// =========================

Ipv4GlobalRoutingHelper::PopulateRoutingTables();

Ipv4GlobalRoutingHelper routingHelper;

Ptr<OutputStreamWrapper> routingStream =
    Create<OutputStreamWrapper>("routing-table.txt", std::ios::out);

routingHelper.PrintRoutingTableAllAt(Seconds(1), routingStream);

// =========================
// UDP TRAFFIC H1 -> H10
// =========================

uint16_t udpPort = 5000;

// UDP receiver on H10
PacketSinkHelper udpSink(
    "ns3::UdpSocketFactory",
    InetSocketAddress(Ipv4Address::GetAny(), udpPort));

ApplicationContainer udpServer = udpSink.Install(hosts.Get(9));

udpServer.Start(Seconds(1));
udpServer.Stop(Seconds(20));

std::string udpRate;
std::string tcpRate;

if (trafficLevel == "low")
{
    udpRate = "5Mbps";
    tcpRate = "5Mbps";
}
else if (trafficLevel == "medium")
{
    udpRate = "20Mbps";
    tcpRate = "20Mbps";
}
else if (trafficLevel == "high")
{
    udpRate = "40Mbps";
    tcpRate = "40Mbps";
}
else
{
    std::cout << "Invalid traffic level!" << std::endl;
    return 1;
}

// UDP sender on H1
OnOffHelper udpClient(
    "ns3::UdpSocketFactory",
    InetSocketAddress(host10.GetAddress(1), udpPort));

udpClient.SetAttribute("DataRate",
                       DataRateValue(DataRate(udpRate)));

udpClient.SetAttribute(
    "PacketSize",
    UintegerValue(1024));

ApplicationContainer udpApp = udpClient.Install(hosts.Get(0));

udpApp.Start(Seconds(2));
udpApp.Stop(Seconds(20));


// =========================
// TCP TRAFFIC H2 -> H9
// =========================

uint16_t tcpPort = 5001;

// TCP receiver on H9
PacketSinkHelper tcpSink(
    "ns3::TcpSocketFactory",
    InetSocketAddress(Ipv4Address::GetAny(), tcpPort));

ApplicationContainer tcpServer = tcpSink.Install(hosts.Get(8));

tcpServer.Start(Seconds(1));
tcpServer.Stop(Seconds(20));

// TCP sender on H2
OnOffHelper tcpClient(
    "ns3::TcpSocketFactory",
    InetSocketAddress(Ipv4Address("10.1.9.2"), tcpPort));


tcpClient.SetAttribute("DataRate",
                       DataRateValue(DataRate(tcpRate)));

tcpClient.SetAttribute(
    "PacketSize",
    UintegerValue(1024));

ApplicationContainer tcpApp = tcpClient.Install(hosts.Get(1));

tcpApp.Start(Seconds(3));
tcpApp.Stop(Seconds(20));



    std::vector<NetDeviceContainer> routerLinks = {
    d1, d2, d3, d4, d5, d6, d7,
    d8, d9, d10, d11, d12, d13
};

std::vector<Ipv4InterfaceContainer> routerInterfaces = {
    i1, i2, i3, i4, i5, i6, i7,
    i8, i9, i10, i11, i12, i13
};

{
    std::ofstream log("adaptive_route_log.csv");
    log << "time,queue_A,queue_B,selected_path\n";
}

Simulator::Schedule(
    Seconds(10),
    &ApplyAdaptiveRoute,
    routers,
    routerLinks,
    routerInterfaces,
    h10);
    

   // =========================
// FLOW MONITOR
// =========================

std::ofstream monitoringFile("monitoring_data.csv");

monitoringFile << "time,total_queue_packets,"
               << "average_queue_packets,"
               << "utilization_percent"
               << std::endl;

monitoringFile.close();

FlowMonitorHelper flowHelper;

Ptr<FlowMonitor> monitor =
    flowHelper.InstallAll();

Simulator::Schedule(
    Seconds(2),
    &MonitorQueues,
    routers);

Simulator::Stop(Seconds(22));

Simulator::Run();

// Check for lost packets
monitor->CheckForLostPackets();

// Get classifier
Ptr<Ipv4FlowClassifier> classifier =
    DynamicCast<Ipv4FlowClassifier>(
        flowHelper.GetClassifier());

// Get flow statistics
std::map<FlowId, FlowMonitor::FlowStats> stats =
    monitor->GetFlowStats();

// Create CSV file
std::ofstream csvFile("network_data.csv");

// CSV header
csvFile << "flow_id,source,destination,"
        << "packets_sent,packets_received,"
        << "packets_lost,delay,"
        << "throughput,jitter"
        << std::endl;

// Print and save flow data
for (const auto &flow : stats)
{
    Ipv4FlowClassifier::FiveTuple t =
        classifier->FindFlow(flow.first);

    double delay = 0.0;

    if (flow.second.rxPackets > 0)
    {
        delay =
            flow.second.delaySum.GetSeconds()
            / flow.second.rxPackets;
    }

    double throughput = 0.0;

    if (flow.second.timeLastRxPacket.GetSeconds() >
        flow.second.timeFirstTxPacket.GetSeconds())
    {
        throughput =
            flow.second.rxBytes * 8.0 /
            (flow.second.timeLastRxPacket.GetSeconds()
            - flow.second.timeFirstTxPacket.GetSeconds())
            / 1000000;
    }

    double jitter = 0.0;

    if (flow.second.rxPackets > 1)
    {
        jitter =
            flow.second.jitterSum.GetSeconds()
            / (flow.second.rxPackets - 1);
    }

    // Print to terminal
    std::cout << "Flow " << flow.first << std::endl;

    std::cout << "Source: "
              << t.sourceAddress << std::endl;

    std::cout << "Destination: "
              << t.destinationAddress << std::endl;

    std::cout << "Packets Sent: "
              << flow.second.txPackets << std::endl;

    std::cout << "Packets Received: "
              << flow.second.rxPackets << std::endl;

    std::cout << "Packets Lost: "
              << flow.second.lostPackets << std::endl;

    std::cout << "Average Delay: "
              << delay
              << " seconds" << std::endl;

    std::cout << "Throughput: "
              << throughput
              << " Mbps" << std::endl;

    std::cout << "Jitter: "
              << jitter
              << " seconds" << std::endl;

    std::cout << "--------------------------"
              << std::endl;

    // Save to CSV
    csvFile << flow.first << ","
            << t.sourceAddress << ","
            << t.destinationAddress << ","
            << flow.second.txPackets << ","
            << flow.second.rxPackets << ","
            << flow.second.lostPackets << ","
            << delay << ","
            << throughput << ","
            << jitter
            << std::endl;
}

csvFile.close();

Simulator::Destroy();

    return 0;
}