#include "../mcp_export.h"
#include <cassert>
int main(){
    cw::DashboardExport x;
    x.generatedAt=1234;x.retention=7;
    x.inventory.processors={L"处理器\n\"quoted\"\\path"};
    cw::Snapshot sample;sample.timestampMs=1200;sample[cw::Metric::CpuPercent]=0;
    x.live.push_back(sample);
    x.details[1].pane=cw::DetailPane::Gpu;
    x.details[2].pane=cw::DetailPane::Memory;
    auto timings=std::make_shared<cw::MemoryTimingReport>();
    cw::MemoryTimingField f;f.name=L"测试";f.value=0;f.status=cw::ReadingStatus::Ok;timings->fields.push_back(f);
    x.details[2].memoryTimings=timings;
    x.details[3].pane=cw::DetailPane::Disk;
    cw::DiskDetail disk;disk.healthy=false;disk.smartRows.push_back({L"温度",L"0 °C",L"实测"});x.details[3].disks.push_back(disk);
    x.details[4].pane=cw::DetailPane::Network;
    x.details[4].traffic.session.push_back({L"c:\\程序.exe",0,10,20,cw::Missing});
    assert(cw::writeDashboardExport(L"build/test-dashboard.json",x));
}
