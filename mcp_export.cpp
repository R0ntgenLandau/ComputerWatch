#include "mcp_export.h"
#include <windows.h>
#include <charconv>
#include <cmath>
#include <fstream>
#include <type_traits>
namespace cw {
namespace {
struct Json {
    std::string value="null";
    Json()=default;
    Json(bool b):value(b?"true":"false"){}
    template<class T> requires std::is_arithmetic_v<T>
    Json(T n) {
        if constexpr(std::is_floating_point_v<T>) if(!std::isfinite(n)) return;
        char buffer[128]; auto result=std::to_chars(buffer,buffer+sizeof(buffer),n);
        if(result.ec==std::errc{}) value.assign(buffer,result.ptr);
    }
    Json(const std::string& s) {
        value="\"";
        const char hex[]="0123456789abcdef";
        for(unsigned char c:s) {
            if(c=='"' || c=='\\'){value+='\\';value+=c;}
            else if(c<32){value+="\\u00";value+=hex[c>>4];value+=hex[c&15];}
            else value+=c;
        }
        value+='"';
    }
    Json(const char* s):Json(std::string(s)){}
    Json(const std::wstring& s) {
        int length=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
        std::string utf8(length,'\0');
        if(length) WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),utf8.data(),length,nullptr,nullptr);
        *this=Json(utf8);
    }
};
Json object(std::initializer_list<std::pair<const char*,Json>> fields) {
    Json j; j.value="{";bool first=true;
    for(const auto& [key,value]:fields){if(!first)j.value+=',';first=false;j.value+=Json(key).value+":"+value.value;}
    j.value+='}';return j;
}
template<class T> Json toJson(const T& x) requires std::is_arithmetic_v<T> {return Json(x);}
Json toJson(const std::wstring& s){return Json(s);}
template<class T> Json toJson(const std::optional<T>& x);
template<class T> Json toJson(const std::vector<T>& values);
template<class T,size_t N> Json toJson(const std::array<T,N>& values);
Json toJson(const CoreDetail& x) { return object({{"group",toJson(x.group)},{"number",toJson(x.number)},{"loadPercent",toJson(x.loadPercent)},{"frequencyMhz",toJson(x.frequencyMhz)}}); }
Json toJson(const MemoryDevice& x) { return object({{"slot",toJson(x.slot)},{"bank",toJson(x.bank)},{"manufacturer",toJson(x.manufacturer)},{"partNumber",toJson(x.partNumber)},{"serialNumber",toJson(x.serialNumber)},{"assetTag",toJson(x.assetTag)},{"type",toJson(x.type)},{"formFactor",toJson(x.formFactor)},{"empty",toJson(x.empty)},{"legacySpeed",toJson(x.legacySpeed)},{"capacity",toJson(x.capacity)},{"ratedSpeed",toJson(x.ratedSpeed)},{"configuredSpeed",toJson(x.configuredSpeed)},{"dataWidth",toJson(x.dataWidth)},{"totalWidth",toJson(x.totalWidth)},{"ranks",toJson(x.ranks)},{"minVolts",toJson(x.minVolts)},{"maxVolts",toJson(x.maxVolts)},{"configuredVolts",toJson(x.configuredVolts)}}); }
Json toJson(const MemoryUsage& x) { return object({{"cache",toJson(x.cache)},{"committed",toJson(x.committed)},{"commitLimit",toJson(x.commitLimit)},{"commitPeak",toJson(x.commitPeak)},{"pagedPool",toJson(x.pagedPool)},{"nonpagedPool",toJson(x.nonpagedPool)},{"pageSize",toJson(x.pageSize)}}); }
Json toJson(const GpuSpecifications& x) { return object({{"architecture",toJson(x.architecture)},{"cudaCapability",toJson(x.cudaCapability)},{"vbios",toJson(x.vbios)},{"boardPartNumber",toJson(x.boardPartNumber)},{"serial",toJson(x.serial)},{"cores",toJson(x.cores)},{"memoryCapacity",toJson(x.memoryCapacity)},{"memoryBusWidth",toJson(x.memoryBusWidth)},{"defaultPowerLimitWatts",toJson(x.defaultPowerLimitWatts)},{"maxPcieGeneration",toJson(x.maxPcieGeneration)},{"hostMaxPcieGeneration",toJson(x.hostMaxPcieGeneration)},{"hostMaxPcieWidth",toJson(x.hostMaxPcieWidth)}}); }
Json toJson(const TrafficTotal& x) { return object({{"path",toJson(x.path)},{"internet",toJson(x.internet)},{"lan",toJson(x.lan)},{"other",toJson(x.other)},{"rate",toJson(x.rate)}}); }
Json toJson(const TrafficReport& x) { return object({{"started",toJson(x.started)},{"status",toJson(x.status)},{"lost",toJson(x.lost)},{"session",toJson(x.session)},{"today",toJson(x.today)},{"week",toJson(x.week)}}); }
Json toJson(const NetworkConnection& x) { return object({{"name",toJson(x.name)},{"description",toJson(x.description)},{"macAddress",toJson(x.macAddress)},{"physical",toJson(x.physical)},{"receiveBitsPerSecond",toJson(x.receiveBitsPerSecond)},{"transmitBitsPerSecond",toJson(x.transmitBitsPerSecond)},{"ipv4",toJson(x.ipv4)},{"ipv6",toJson(x.ipv6)},{"gateways",toJson(x.gateways)},{"dnsServers",toJson(x.dnsServers)}}); }
Json toJson(const NetworkDetails& x) { return object({{"physicalAdapters",toJson(x.physicalAdapters)},{"virtualAdapters",toJson(x.virtualAdapters)},{"addressesAvailable",toJson(x.addressesAvailable)},{"connections",toJson(x.connections)}}); }
Json toJson(const Chromaticity& x) { return object({{"x",toJson(x.x)},{"y",toJson(x.y)}}); }
Json toJson(const DisplayGamut& x) { return object({{"primaries",toJson(x.primaries)},{"srgb",toJson(x.srgb)},{"displayP3",toJson(x.displayP3)},{"adobeRgb",toJson(x.adobeRgb)}}); }
Json toJson(const DisplayInfo& x) { return object({{"name",toJson(x.name)},{"connection",toJson(x.connection)},{"signalWidth",toJson(x.signalWidth)},{"signalHeight",toJson(x.signalHeight)},{"refreshHz",toJson(x.refreshHz)},{"gamut",toJson(x.gamut)}}); }

const char* statusName(ReadingStatus status) {
    switch(status){case ReadingStatus::Ok:return "ok";case ReadingStatus::NotApplicable:return "not_applicable";
    case ReadingStatus::BackendMissing:return "backend_missing";case ReadingStatus::ReadFailed:return "read_failed";
    case ReadingStatus::Unverified:return "unverified";} return "unknown";
}
Json toJson(const MemoryTimingField& x) {return object({{"name",x.name},{"unit",x.unit},{"scope",x.scope},{"source",x.source},{"reason",x.reason},{"value",toJson(x.value)},{"status",statusName(x.status)},{"sampledAtMs",x.sampledAtMs}});}
Json toJson(const MemoryTimingReport& x) {return object({{"platform",x.platform},{"backend",x.backend},{"reason",x.reason},{"status",statusName(x.status)},{"sampledAtMs",x.sampledAtMs},{"fields",toJson(x.fields)}});}
Json toJson(const std::pair<std::wstring,std::wstring>& x){return object({{"name",x.first},{"value",x.second}});}
Json toJson(const BoardSection& x){return object({{"title",x.title},{"fields",toJson(x.fields)}});}
Json toJson(const DiskDetail& x){return object({{"number",x.number},{"name",x.name},{"bus",x.bus},{"healthy",toJson(x.healthy)},{"temperatureC",x.temperatureC},{"wearPercent",x.wearPercent},{"powerOnHours",x.powerOnHours},{"powerCycles",x.powerCycles},{"errors",x.errors},{"note",x.note},{"smartRows",toJson(x.smartRows)}});}
Json toJson(const HardwareInventory& x){return object({{"processors",toJson(x.processors)},{"memory",toJson(x.memory)},{"disks",toJson(x.disks)},{"graphics",toJson(x.graphics)},{"sampledGpu",x.sampledGpu},{"gpu",toJson(x.gpu)},{"memoryDevices",toJson(x.memoryDevices)},{"board",toJson(x.board)},{"installedMemory",x.installedMemory}});}
Json toJson(const ProcessEntry& x){return object({{"pid",x.pid},{"name",std::wstring(x.name)},{"cpu_percent",x.cpu},{"memory_bytes",x.memory}});}
Json toJson(const Snapshot& x){
    static const char* keys[]={"cpu_percent","memory_percent","memory_used","memory_total","disk_used","disk_total","disk_read","disk_write","gpu_percent","gpu_memory_used","gpu_memory_total","network_down","network_up","cpu_temp","cpu_watts","gpu_temp","gpu_watts"};
    Json metrics;metrics.value="{";
    for(size_t i=0;i<MetricCount;++i){if(i)metrics.value+=',';metrics.value+=Json(keys[i]).value+":"+Json(x.values[i]).value;}
    metrics.value+='}';
    return object({{"timestamp_ms",x.timestampMs},{"metrics",metrics},{"by_cpu",toJson(x.byCpu)},{"by_memory",toJson(x.byMemory)}});
}
template<class T> Json toJson(const std::optional<T>& x){return x?toJson(*x):Json();}
template<class Range> Json arrayJson(const Range& values){Json j;j.value="[";bool first=true;for(const auto& x:values){if(!first)j.value+=',';first=false;j.value+=toJson(x).value;}j.value+=']';return j;}
template<class T> Json toJson(const std::vector<T>& values){return arrayJson(values);}
template<class T,size_t N> Json toJson(const std::array<T,N>& values){return arrayJson(values);}
Json paneJson(const HardwareDetails& x){
    switch(x.pane){
    case DetailPane::Cpu:return object({{"cores",toJson(x.cores)}});
    case DetailPane::Disk:return object({{"disks",toJson(x.disks)}});
    case DetailPane::Memory:return object({{"usage",toJson(x.memory)},{"timings",x.memoryTimings?toJson(*x.memoryTimings):Json()}});
    case DetailPane::Network:return object({{"adapters",toJson(x.network)},{"traffic",toJson(x.traffic)}});
    case DetailPane::Gpu:return object({
{"gpuCoreMhz",toJson(x.gpuCoreMhz)},{"gpuMemoryMhz",toJson(x.gpuMemoryMhz)},{"gpuFanPercent",toJson(x.gpuFanPercent)},{"gpuFanRpm",toJson(x.gpuFanRpm)},{"gpuHotspotC",toJson(x.gpuHotspotC)},{"gpuMemoryActivity",toJson(x.gpuMemoryActivity)},{"gpuEncoderPercent",toJson(x.gpuEncoderPercent)},{"gpuDecoderPercent",toJson(x.gpuDecoderPercent)},{"gpuPowerLimitWatts",toJson(x.gpuPowerLimitWatts)},{"gpuPerformanceState",toJson(x.gpuPerformanceState)},{"gpuPcieGeneration",toJson(x.gpuPcieGeneration)},{"gpuPcieWidth",toJson(x.gpuPcieWidth)},{"gpuLimitsAvailable",toJson(x.gpuLimitsAvailable)},{"gpuThermalLimited",x.gpuLimitsAvailable?Json(x.gpuThermalLimited):Json()},{"gpuPowerLimited",x.gpuLimitsAvailable?Json(x.gpuPowerLimited):Json()}});
    default:return Json();
    }
}
}
bool writeDashboardExport(const std::filesystem::path& path,const DashboardExport& x){
    const auto pane=[&](size_t i){return object({{"sampled_at_ms",x.detailTimes[i]?Json(x.detailTimes[i]):Json()},{"data",paneJson(x.details[i])}});};
    const auto json=object({{"schema_version",1},{"generated_at_ms",x.generatedAt},{"refresh_pending",x.pending},
        {"inventory",toJson(x.inventory)},{"cpu",pane(0)},{"gpu",pane(1)},{"memory",pane(2)},{"disk",pane(3)},{"network",pane(4)},
        {"displays",toJson(x.displays)},{"live",toJson(x.live)},
        {"settings",object({{"retention_days",x.retention},{"theme",x.dark?"dark":"light"}})},
        {"units",object({{"capacity","bytes"},{"traffic_rate","bytes/second"},{"link_rate","bits/second"},{"temperature","C"},{"power","W"},{"frequency","MHz"},{"timestamps","Unix milliseconds"}})}});
    auto temporary=path;temporary+=L".tmp";
    {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<json.value;file.flush();if(!file)return false;}
    return MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}
}
