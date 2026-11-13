#include "traffic.h"
#include <evntrace.h>
#include <evntcons.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
namespace cw {
int trafficScope(const unsigned char* a, bool v6) {
    if (v6) {
        static const unsigned char mapped[12]={0,0,0,0,0,0,0,0,0,0,255,255};
        if (!memcmp(a,mapped,12)) return trafficScope(a+12,false);
        if ((a[0]&254)==252 || (a[0]==254 && (a[1]&192)==128)) return 1;
        if (a[0]==0x20 && a[1]==1 && a[2]==0x0d && a[3]==0xb8) return 2;
        if ((a[0]&224)==32) return 0;
        return 2;
    }
    if (a[0]==10 || (a[0]==172 && a[1]>=16 && a[1]<=31) ||
        (a[0]==192 && a[1]==168) || (a[0]==169 && a[1]==254)) return 1;
    if (a[0]==0 || a[0]==127 || a[0]>=224 || (a[0]==100 && a[1]>=64 && a[1]<=127) ||
        (a[0]==192 && a[1]==0) || (a[0]==198 && (a[1]==18 || a[1]==19 || (a[1]==51 && a[2]==100))) ||
        (a[0]==203 && a[1]==0 && a[2]==113)) return 2;
    return 0;
}
struct TrafficCapture::Impl {
    TRACEHANDLE session{}, consumer=INVALID_PROCESSTRACE_HANDLE;
    std::wstring name=L"ComputerWatch.Network."+std::to_wstring(GetCurrentProcessId());
    std::vector<unsigned char> buffer;
    std::jthread worker;
    std::mutex mutex;
    TrafficPage page;
    struct ProcessName { std::wstring first; ULONGLONG second{}; std::shared_ptr<void> process; };
    std::map<DWORD,ProcessName> names;
    static void WINAPI event(EVENT_RECORD* e) {
        auto& self=*static_cast<Impl*>(e->UserContext);
        const auto op=e->EventHeader.EventDescriptor.Opcode;
        static const GUID tcp={0x9a280ac0,0xc8e0,0x11d1,{0x84,0xe2,0,0xc0,0x4f,0xb9,0x98,0xa2}};
        static const GUID udp={0xbf3a50c5,0xa9c9,0x4988,{0xa0,0x05,0x2d,0xf0,0xb7,0xc8,0x0f,0x80}};
        if ((!IsEqualGUID(e->EventHeader.ProviderId,tcp) && !IsEqualGUID(e->EventHeader.ProviderId,udp)) ||
            (op!=10 && op!=26) || e->EventHeader.EventDescriptor.Version<1 || e->UserDataLength<(op==26?44:20)) return;
        DWORD pid=0,size=0; auto* data=static_cast<unsigned char*>(e->UserData);
        memcpy(&pid,data,4); memcpy(&size,data+4,4);
        if (!size) return;
        const auto tick=GetTickCount64();
        auto& cached=self.names[pid];
        if (cached.first.empty() || tick-cached.second>1000 ||
            (cached.process && WaitForSingleObject(cached.process.get(),0)!=WAIT_TIMEOUT)) {
            wchar_t path[520]{}; DWORD length=520;
            HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
            if (p && QueryFullProcessImageNameW(p,0,path,&length)) {
                cached.first=path;
                std::transform(cached.first.begin(),cached.first.end(),cached.first.begin(),towlower);
            } else cached.first=L"未识别进程 PID "+std::to_wstring(pid);
            cached.process=std::shared_ptr<void>(p,[](void* h){if(h) CloseHandle(h);});
            cached.second=tick;
        }
        std::scoped_lock lock(self.mutex);
        auto& page=self.page;
        auto end=page.rows.begin()+page.count;
        auto row=std::find_if(page.rows.begin(),end,[&](const auto& r){return cached.first==r.path;});
        if(row==end) {
            if(page.count==page.rows.size()) { ++page.lost; return; }
            row=&page.rows[page.count++];
            std::copy_n(cached.first.c_str(),std::min<size_t>(519,cached.first.size()),row->path);
        }
        int scope=trafficScope(data+8,op==26);
        (scope==0?row->internet:scope==1?row->lan:row->other)+=size;
        if(self.names.size()>8192) self.names.clear();
    }
    ULONGLONG lastAttempt{};
    void start() {
        if(session || (lastAttempt && GetTickCount64()-lastAttempt<30000)) return;
        lastAttempt=GetTickCount64();
        buffer.resize(sizeof(EVENT_TRACE_PROPERTIES)+(name.size()+1)*sizeof(wchar_t));
        auto* p=reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
        *p={}; p->Wnode.BufferSize=static_cast<ULONG>(buffer.size());
        p->Wnode.Flags=WNODE_FLAG_TRACED_GUID; p->Wnode.ClientContext=1;
        p->Wnode.Guid={0xe9b6ab21 ^ GetCurrentProcessId(),0x5438,0x4e19,{0x83,0x65,0x12,0xf0,0xb2,0x9c,0xa3,0x17}};
        p->LogFileMode=EVENT_TRACE_REAL_TIME_MODE|EVENT_TRACE_SYSTEM_LOGGER_MODE;
        p->EnableFlags=EVENT_TRACE_FLAG_NETWORK_TCPIP; p->BufferSize=64;
        p->MinimumBuffers=8; p->MaximumBuffers=64; p->FlushTimer=1;
        p->LoggerNameOffset=sizeof(EVENT_TRACE_PROPERTIES);
        page.status=StartTraceW(&session,name.c_str(),p);
        if(page.status) {session=0; return;}
        EVENT_TRACE_LOGFILEW log{}; log.LoggerName=name.data(); log.Context=this;
        log.ProcessTraceMode=PROCESS_TRACE_MODE_REAL_TIME|PROCESS_TRACE_MODE_EVENT_RECORD;
        log.EventRecordCallback=event;
        consumer=OpenTraceW(&log);
        if(consumer==INVALID_PROCESSTRACE_HANDLE) {
            page.status=GetLastError(); ControlTraceW(session,name.c_str(),p,EVENT_TRACE_CONTROL_STOP); session=0; return;
        }
        page.started=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        worker=std::jthread([this]{ProcessTrace(&consumer,1,nullptr,nullptr);});
    }
    ~Impl(){
        if(session) ControlTraceW(session,name.c_str(),reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data()),EVENT_TRACE_CONTROL_STOP);
        if(consumer!=INVALID_PROCESSTRACE_HANDLE) CloseTrace(consumer);
        if(worker.joinable()) worker.join();
    }
};
TrafficCapture::TrafficCapture():impl_(std::make_unique<Impl>()){}
TrafficCapture::~TrafficCapture()=default;
void TrafficCapture::start(){impl_->start();}
void TrafficCapture::snapshot(TrafficPage& p){
    std::scoped_lock lock(impl_->mutex); p=impl_->page;
    if(impl_->session) {
        auto* props=reinterpret_cast<EVENT_TRACE_PROPERTIES*>(impl_->buffer.data());
        if(ControlTraceW(impl_->session,impl_->name.c_str(),props,EVENT_TRACE_CONTROL_QUERY)==ERROR_SUCCESS)
            p.lost+=props->EventsLost+props->RealTimeBuffersLost;
    }
}
}
