#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

/** Guarda os resultados de um cliente ou de um grupo. */
struct Metrics
{
    uint32_t clients{0};             ///< Quantidade de clientes do grupo.
    uint32_t associatedStart{0};     ///< Associados quando o trafego comeca.
    uint32_t associatedEnd{0};       ///< Associados no fim da simulacao.
    uint32_t receiving{0};           ///< Clientes que receberam dados.
    uint32_t connected{0};           ///< Conexoes TCP estabelecidas.
    uint64_t appTxBytes{0};          ///< Bytes aceitos pelo socket do gerador.
    uint64_t appRxBytes{0};          ///< Bytes entregues a aplicacao receptora.
    uint64_t ipTxPackets{0};         ///< Pacotes IP enviados, incluindo controle e retransmissoes.
    uint64_t ipRxPackets{0};         ///< Pacotes IP recebidos no cliente.
    uint64_t ipLostPackets{0};       ///< Perdas informadas pelo FlowMonitor.
    uint64_t ipUnresolvedPackets{0}; ///< Pacotes sem recepcao ou perda confirmada.
    double delaySeconds{0};         ///< Soma dos atrasos IP, em segundos.
};

/**
 * Conta os bytes que o socket aceitou para envio.
 * @param metrics Contadores do cliente.
 * @param packet Pacote entregue ao socket.
 */
void
CountTx(Metrics* metrics, Ptr<const Packet> packet)
{
    metrics->appTxBytes += packet->GetSize();
}

/**
 * Marca que a conexao TCP foi estabelecida.
 * @param metrics Contadores do cliente.
 * @param socket Socket conectado.
 * @param local Endereco local.
 * @param remote Endereco de destino.
 */
void
CountConnection(Metrics* metrics, Ptr<Socket> socket, const Address& local, const Address& remote)
{
    metrics->connected = 1;
}

/**
 * Soma os contadores de um cliente aos de seu grupo.
 * @param total Resultado acumulado.
 * @param client Resultado do cliente.
 */
void
Accumulate(Metrics& total, const Metrics& client)
{
    total.clients += client.clients;
    total.associatedStart += client.associatedStart;
    total.associatedEnd += client.associatedEnd;
    total.receiving += client.receiving;
    total.connected += client.connected;
    total.appTxBytes += client.appTxBytes;
    total.appRxBytes += client.appRxBytes;
    total.ipTxPackets += client.ipTxPackets;
    total.ipRxPackets += client.ipRxPackets;
    total.ipLostPackets += client.ipLostPackets;
    total.ipUnresolvedPackets += client.ipUnresolvedPackets;
    total.delaySeconds += client.delaySeconds;
}

/**
 * Escreve a porcentagem; deixa vazio quando nao ha denominador.
 * @param stream Saida que recebe o texto.
 * @param numerator Quantidade considerada.
 * @param denominator Quantidade total.
 */
void
WritePercent(std::ostream& stream, uint64_t numerator, uint64_t denominator)
{
    if (denominator > 0)
    {
        stream << 100.0 * static_cast<double>(numerator) / denominator;
    }
}

/**
 * Escreve as metricas nas colunas comuns dos arquivos CSV.
 * @param stream Saida que recebe o texto.
 * @param metrics Contadores que serao gravados.
 * @param window Duracao da geracao de trafego, em segundos.
 */
void
WriteMetrics(std::ostream& stream, const Metrics& metrics, double window)
{
    // Bytes viram bits ao multiplicar por 8; 1e6 converte bit/s em Mbps.
    const double throughput = metrics.appRxBytes * 8.0 / (window * 1e6);
    stream << metrics.clients << ',' << metrics.associatedStart << ',' << metrics.associatedEnd
           << ',' << metrics.receiving << ',' << metrics.connected << ',' << metrics.appTxBytes
           << ',' << metrics.appRxBytes << ',' << metrics.ipTxPackets << ',' << metrics.ipRxPackets
           << ',' << metrics.ipLostPackets << ',' << metrics.ipUnresolvedPackets << ',' << throughput
           << ',' << throughput / metrics.clients << ',';
    WritePercent(stream, metrics.ipTxPackets - metrics.ipRxPackets, metrics.ipTxPackets);
    stream << ',';
    if (metrics.ipRxPackets > 0)
    {
        stream << metrics.delaySeconds * 1000.0 / metrics.ipRxPackets;
    }
    stream << ',';
    WritePercent(stream, metrics.appRxBytes, metrics.appTxBytes);
}

/**
 * Monta a rede e executa um dos cenarios do trabalho.
 * @param argc Quantidade de argumentos.
 * @param argv Argumentos informados no terminal.
 * @return Zero se a execucao terminou sem erro.
 */
int
main(int argc, char* argv[])
{
    uint32_t nClients = 0;
    std::string mobileInput;
    std::string trafficType;
    uint32_t seed = 1;
    uint64_t run = 1;
    double simTime = 60.0;
    double appStart = 2.0;
    double cleanupTime = 10.0;
    std::string outputPrefix;
    bool exportXml = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("nClients", "Clientes: 1,2,4,8,16,32; adicionais: 6,12,24,48", nClients);
    cmd.AddValue("isMobile", "Mobilidade: true ou false", mobileInput);
    cmd.AddValue("trafficType", "CBR (UDP), Rajada (TCP) ou CBR_Rajada (misto)", trafficType);
    cmd.AddValue("seed", "Semente aleatoria positiva", seed);
    cmd.AddValue("run", "Numero positivo da repeticao independente", run);
    cmd.AddValue("simTime", "Tempo total simulado em segundos", simTime);
    cmd.AddValue("appStart", "Inicio comum dos geradores em segundos", appStart);
    cmd.AddValue("cleanupTime", "Tempo final sem gerar novos dados, em segundos", cleanupTime);
    cmd.AddValue("outputPrefix", "Prefixo opcional dos arquivos CSV/XML", outputPrefix);
    cmd.AddValue("exportXml", "Exportar tambem os fluxos completos em XML", exportXml);
    cmd.Parse(argc, argv);

    // Aceitamos tambem os nomes dos protocolos no comando.
    if (trafficType == "UDP")
    {
        trafficType = "CBR";
    }
    else if (trafficType == "TCP")
    {
        trafficType = "Rajada";
    }
    else if (trafficType == "Misto")
    {
        trafficType = "CBR_Rajada";
    }

    const std::vector<uint32_t> allowed{1, 2, 4, 6, 8, 12, 16, 24, 32, 48};
    const bool validClients = std::find(allowed.begin(), allowed.end(), nClients) != allowed.end();
    const bool validMobility = mobileInput == "true" || mobileInput == "false";
    const bool validTraffic = trafficType == "CBR" || trafficType == "Rajada" ||
                              trafficType == "CBR_Rajada";
    const bool validRandom = seed > 0 && seed < 2147483647 && run > 0;
    const bool validTimes = std::isfinite(simTime) && std::isfinite(appStart) &&
                            std::isfinite(cleanupTime) && appStart >= 1.0 &&
                            cleanupTime >= 2.0 && simTime > appStart + cleanupTime;
    if (!validClients || !validMobility || !validTraffic || !validRandom || !validTimes)
    {
        std::cerr << "Parametros invalidos. Use --help. Exemplo:\n"
                  << "./ns3 run 'scratch/equipe_2_3q2026 --nClients=6 "
                  << "--isMobile=false --trafficType=CBR'\n";
        return 1;
    }
    if (trafficType == "CBR_Rajada" && nClients % 2 != 0)
    {
        std::cerr << "O cenario misto exige numero par de clientes para dividir os nos 50%/50%.\n";
        return 1;
    }

    const bool isMobile = mobileInput == "true";
    // Paramos de gerar antes de encerrar a simulacao. Assim os pacotes
    // que ja estao nas filas ainda podem chegar aos receptores.
    const double appStop = simTime - cleanupTime;
    const double window = appStop - appStart;
    // Seed fixa permite repetir o teste; run escolhe outra repeticao.
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TcpCubic::GetTypeId()));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    // Os clientes iniciam juntos. A fila maior evita descartar os primeiros
    // pacotes enquanto o servidor ainda descobre o MAC do gateway por ARP.
    Config::SetDefault("ns3::ArpCache::PendingQueueSize", UintegerValue(100));

    // Os quatro dispositivos cabeados compartilham o mesmo canal CSMA.
    NodeContainer wiredNodes;
    wiredNodes.Create(3); // Ordem dos hosts: s2 (servidor), s1 e s0.
    NodeContainer apNode;
    apNode.Create(1);
    NodeContainer clientNodes;
    clientNodes.Create(nClients);
    NodeContainer lanNodes(wiredNodes, apNode);

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(NanoSeconds(6560)));
    csma.SetDeviceAttribute("Mtu", UintegerValue(1500));
    auto lanDevices = csma.Install(lanNodes);

    // Consideramos uma area aberta, sem paredes. Com Friis, podemos estudar
    // o aumento da carga sem misturar isso com clientes fora de cobertura.
    YansWifiChannelHelper channel;
    channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channel.AddPropagationLoss("ns3::FriisPropagationLossModel", "Frequency", DoubleValue(5.18e9));
    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());
    phy.Set("TxPowerStart", DoubleValue(16.0));
    phy.Set("TxPowerEnd", DoubleValue(16.0));
    phy.Set("Antennas", UintegerValue(1));
    phy.Set("ChannelSettings", StringValue("{36, 20, BAND_5GHZ, 0}"));
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211a);
    wifi.SetRemoteStationManager("ns3::IdealWifiManager");
    WifiMacHelper mac;
    const Ssid ssid("Equipe2");
    mac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(false));
    auto staDevices = wifi.Install(phy, mac, clientNodes);
    mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
    auto apDevices = wifi.Install(phy, mac, apNode);
    // Cada parte recebe seus proprios streams de sorteio. Esses numeros
    // sao identificadores; nao sao velocidades nem potencias.
    wifi.AssignStreams(apDevices, 20000);
    for (uint32_t i = 0; i < nClients; ++i)
    {
        NetDeviceContainer one(staDevices.Get(i));
        wifi.AssignStreams(one, 1000 + 100 * i);
    }

    auto position = CreateObject<RandomRectanglePositionAllocator>();
    auto x = CreateObject<UniformRandomVariable>();
    auto y = CreateObject<UniformRandomVariable>();
    x->SetAttribute("Min", DoubleValue(0.0));
    x->SetAttribute("Max", DoubleValue(140.0));
    y->SetAttribute("Min", DoubleValue(0.0));
    y->SetAttribute("Max", DoubleValue(140.0));
    position->SetAttribute("X", PointerValue(x));
    position->SetAttribute("Y", PointerValue(y));
    // Mantemos os sorteios de posicao iguais ao comparar os cenarios.
    position->AssignStreams(1);
    MobilityHelper mobility;
    mobility.SetPositionAllocator(position);
    if (isMobile)
    {
        mobility.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                                  "Bounds", RectangleValue(Rectangle(0, 140, 0, 140)),
                                  "Mode", EnumValue(RandomWalk2dMobilityModel::MODE_TIME),
                                  "Time", TimeValue(Seconds(1)),
                                  "Speed", StringValue("ns3::UniformRandomVariable[Min=1|Max=2]"));
    }
    else
    {
        mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    }
    mobility.Install(clientNodes);
    mobility.AssignStreams(clientNodes, 100);
    MobilityHelper fixed;
    fixed.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    fixed.Install(lanNodes);
    apNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(70, 70, 0));
    for (uint32_t i = 0; i < 3; ++i)
    {
        wiredNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(Vector(-30.0 + 10.0 * i, 70, 0));
    }

    InternetStackHelper stack;
    stack.Install(lanNodes);
    stack.Install(clientNodes);
    stack.AssignStreams(NodeContainer(lanNodes, clientNodes), 30000);
    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    auto lanInterfaces = address.Assign(lanDevices);
    address.SetBase("192.168.0.0", "255.255.255.0");
    auto staInterfaces = address.Assign(staDevices);
    address.Assign(apDevices);
    // O AP tem uma interface em cada sub-rede e encaminha os pacotes entre elas.
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();
    const auto server = wiredNodes.Get(0);
    const auto serverAddress = lanInterfaces.GetAddress(0);

    std::vector<Metrics> clients(nClients);
    std::vector<Ptr<PacketSink>> sinks(nClients);
    std::vector<Vector> initialPositions(nClients);
    std::vector<bool> udp(nClients);
    std::map<Ipv4Address, uint32_t> clientIndex;
    for (uint32_t i = 0; i < nClients; ++i)
    {
        clients[i].clients = 1;
        // No misto, indices pares recebem UDP e impares recebem TCP.
        udp[i] = trafficType == "CBR" || (trafficType == "CBR_Rajada" && i % 2 == 0);
        const std::string factory = udp[i] ? "ns3::UdpSocketFactory" : "ns3::TcpSocketFactory";
        const uint16_t port = udp[i] ? 9000 : 9001;
        clientIndex.emplace(staInterfaces.GetAddress(i), i);
        initialPositions[i] = clientNodes.Get(i)->GetObject<MobilityModel>()->GetPosition();
        // O cliente recebe; o gerador fica no servidor s2.
        PacketSinkHelper sink(factory, InetSocketAddress(Ipv4Address::GetAny(), port));
        auto receiver = sink.Install(clientNodes.Get(i));
        receiver.Start(Seconds(1));
        receiver.Stop(Seconds(simTime));
        sinks[i] = DynamicCast<PacketSink>(receiver.Get(0));
        OnOffHelper source(factory, InetSocketAddress(staInterfaces.GetAddress(i), port));
        source.SetAttribute("DataRate", StringValue("512kbps"));
        if (udp[i])
        {
            // Sem pausa entre os periodos On: o envio UDP fica constante.
            source.SetAttribute("PacketSize", UintegerValue(512));
            source.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
            source.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        }
        else
        {
            // O TCP alterna envio e pausa. Os 512 kbps valem apenas durante On.
            // O bloco de 1500 bytes pode ser dividido em segmentos pelo TCP.
            source.SetAttribute("PacketSize", UintegerValue(1500));
            source.SetAttribute("OnTime", StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));
            source.SetAttribute("OffTime", StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));
        }
        auto sender = source.Install(server);
        auto onoff = DynamicCast<OnOffApplication>(sender.Get(0));
        onoff->AssignStreams(50000 + 100 * i);
        // Contar o que o socket aceitou nao e o mesmo que contar o que chegou.
        // Os bytes recebidos serao lidos do PacketSink depois da simulacao.
        onoff->TraceConnectWithoutContext("Tx", MakeBoundCallback(&CountTx, &clients[i]));
        if (!udp[i])
        {
            onoff->TraceConnectWithoutContext("ConnectionSucceeded",
                                             MakeBoundCallback(&CountConnection, &clients[i]));
        }
        sender.Start(Seconds(appStart));
        sender.Stop(Seconds(appStop));
    }

    // Esta verificacao mostra a associacao no inicio, nao um historico continuo.
    Simulator::Schedule(Seconds(appStart), [&]() {
        for (uint32_t i = 0; i < nClients; ++i)
        {
            auto device = DynamicCast<WifiNetDevice>(staDevices.Get(i));
            auto clientMac = DynamicCast<StaWifiMac>(device->GetMac());
            clients[i].associatedStart = clientMac->IsAssociated();
        }
    });
    FlowMonitorHelper flowmon;
    // O timeout e menor que a drenagem. Mesmo assim, conferimos no final
    // se sobrou algum pacote que ainda nao foi recebido nem marcado como perdido.
    flowmon.SetMonitorAttribute("MaxPerHopDelay", TimeValue(Seconds(cleanupTime / 2)));
    auto monitor = flowmon.InstallAll();
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    monitor->CheckForLostPackets();
    auto classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    for (const auto& [id, statistics] : monitor->GetFlowStats())
    {
        const auto flow = classifier->FindFlow(id);
        const auto destination = clientIndex.find(flow.destinationAddress);
        // Os ACKs do TCP voltam do cliente para o servidor. Eles nao entram
        // na medicao de descida, que acompanha o sentido servidor-cliente.
        if (flow.sourceAddress != serverAddress || destination == clientIndex.end())
        {
            continue;
        }
        const uint32_t i = destination->second;
        const uint8_t expectedProtocol = udp[i] ? 17 : 6; // Identificadores IP: UDP e TCP.
        const uint16_t expectedPort = udp[i] ? 9000 : 9001;
        if (flow.protocol != expectedProtocol || flow.destinationPort != expectedPort)
        {
            continue;
        }
        auto& metrics = clients[i];
        metrics.ipTxPackets += statistics.txPackets;
        metrics.ipRxPackets += statistics.rxPackets;
        metrics.ipLostPackets += statistics.lostPackets;
        const uint64_t missing = statistics.txPackets - statistics.rxPackets;
        if (missing > statistics.lostPackets)
        {
            metrics.ipUnresolvedPackets += missing - statistics.lostPackets;
        }
        // Guardamos os segundos com a fracao. A conversao para ms vem na media.
        metrics.delaySeconds += statistics.delaySum.GetSeconds();
    }
    // ALL soma todos os clientes. UDP e TCP permitem conferir o misto separadamente.
    std::map<std::string, Metrics> groups;
    for (uint32_t i = 0; i < nClients; ++i)
    {
        clients[i].appRxBytes = sinks[i]->GetTotalRx();
        clients[i].receiving = clients[i].appRxBytes > 0;
        auto device = DynamicCast<WifiNetDevice>(staDevices.Get(i));
        auto clientMac = DynamicCast<StaWifiMac>(device->GetMac());
        clients[i].associatedEnd = clientMac->IsAssociated();
        Accumulate(groups["ALL"], clients[i]);
        Accumulate(groups[udp[i] ? "UDP" : "TCP"], clients[i]);
    }

    std::cout << std::fixed << std::setprecision(6)
              << "\nEquipe 2: clientes=" << nClients << " mobilidade=" << mobileInput
              << " trafego=" << trafficType << " seed=" << seed << " run=" << run
              << "\nDados: s2 -> AP -> clientes. Geracao: " << appStart << " a " << appStop
              << " s; encerramento: " << simTime << " s.\n";
    for (const auto& [group, metrics] : groups)
    {
        const double throughput = metrics.appRxBytes * 8.0 / (window * 1e6);
        std::cout << group << ": vazao util total=" << throughput << " Mbps; por cliente="
                  << throughput / metrics.clients << " Mbps; perda IP=";
        if (metrics.ipTxPackets > 0)
            WritePercent(std::cout, metrics.ipTxPackets - metrics.ipRxPackets, metrics.ipTxPackets);
        else std::cout << "N/A";
        std::cout << " %; atraso IP=";
        if (metrics.ipRxPackets > 0) std::cout << metrics.delaySeconds * 1000 / metrics.ipRxPackets;
        else std::cout << "N/A";
        std::cout << " ms; clientes com dados=" << metrics.receiving << '/' << metrics.clients
                  << "; associados inicio/fim=" << metrics.associatedStart << '/'
                  << metrics.associatedEnd << "; pendentes=" << metrics.ipUnresolvedPackets << '\n';
    }

    int status = 0;
    if (!outputPrefix.empty())
    {
        const std::string commonHeader = "nClients,isMobile,trafficType,seed,run,simTime,appStart,appStop,cleanupTime,";
        const std::string metricsHeader = "clients,associatedAtStart,associatedAtEnd,clientsReceiving,connectionsSucceeded,appTxBytes,appRxBytes,ipTxPackets,ipRxPackets,ipLostPackets,ipUnresolvedPackets,throughputTotalMbps,throughputPerClientMbps,ipLossPercent,delayMs,appDeliveryPercent\n";
        // A precisao extra evita arredondar os tempos informados no comando.
        std::ostringstream parameters;
        parameters << std::setprecision(17) << nClients << ',' << mobileInput << ',' << trafficType
                   << ',' << seed << ',' << run << ',' << simTime << ',' << appStart << ','
                   << appStop << ',' << cleanupTime << ',';
        const std::string common = parameters.str();
        try
        {
            const auto parent = std::filesystem::path(outputPrefix).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            std::ofstream summary(outputPrefix + "_summary.csv");
            std::ofstream perClient(outputPrefix + "_clients.csv");
            summary.exceptions(std::ios::failbit | std::ios::badbit);
            perClient.exceptions(std::ios::failbit | std::ios::badbit);
            summary << std::setprecision(12) << commonHeader << "group," << metricsHeader;
            for (const auto& [group, metrics] : groups)
            {
                summary << common << group << ',';
                WriteMetrics(summary, metrics, window);
                summary << '\n';
            }
            perClient << std::setprecision(12) << commonHeader
                      << "client,protocol,address,initialX,initialY,finalX,finalY," << metricsHeader;
            for (uint32_t i = 0; i < nClients; ++i)
            {
                const auto finalPosition = clientNodes.Get(i)->GetObject<MobilityModel>()->GetPosition();
                perClient << common << i << ',' << (udp[i] ? "UDP" : "TCP") << ','
                          << staInterfaces.GetAddress(i) << ',' << initialPositions[i].x << ','
                          << initialPositions[i].y << ',' << finalPosition.x << ',' << finalPosition.y << ',';
                WriteMetrics(perClient, clients[i], window);
                perClient << '\n';
            }
            summary.close();
            perClient.close();
            if (exportXml) monitor->SerializeToXmlFile(outputPrefix + "_flow.xml", false, false);
            std::cout << "CSV: " << outputPrefix << "_{summary,clients}.csv\n";
        }
        catch (const std::exception& error)
        {
            std::cerr << "Falha ao escrever resultados: " << error.what() << '\n';
            status = 2;
        }
    }
    Simulator::Destroy();
    return status;
}
