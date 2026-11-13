#include "../history.h"
#include <cassert>
#include <iostream>
int main(){
    unsigned char ip[16]={10,0,0,1}; assert(cw::trafficScope(ip,false)==1);
    ip[0]=8;ip[1]=8;assert(cw::trafficScope(ip,false)==0);
    ip[0]=127;assert(cw::trafficScope(ip,false)==2);
    ip[0]=172;ip[1]=31;assert(cw::trafficScope(ip,false)==1);
    ip[1]=32;assert(cw::trafficScope(ip,false)==0);
    ip[0]=0xfc;assert(cw::trafficScope(ip,true)==1);
    ip[0]=0x20;ip[1]=1;assert(cw::trafficScope(ip,true)==0);
    ip[0]=0xff;assert(cw::trafficScope(ip,true)==2);
    auto path=std::filesystem::path(L"build")/(L"traffic-test-"+std::to_wstring(GetCurrentProcessId())+L".db");
    cw::History h(path,true); assert(h.ready());
    auto p=std::make_unique<cw::TrafficPage>(); p->status=0;p->started=100;p->count=1;
    wcscpy_s(p->rows[0].path,L"c:\\test.exe");p->rows[0].internet=100;p->rows[0].lan=50;p->rows[0].other=10;
    assert(h.saveTraffic(*p,1000));assert(h.saveTraffic(*p,2000));
    auto rows=h.trafficTotals(0);assert(rows.size()==1 && rows[0].internet==100 && rows[0].lan==50);
    p->rows[0].internet=140;assert(h.saveTraffic(*p,3000));
    rows=h.trafficTotals(2500);assert(rows.size()==1 && rows[0].internet==40);
    p->started=200;p->rows[0].internet=20;assert(h.saveTraffic(*p,4000));
    rows=h.trafficTotals(0);assert(rows[0].internet==160);
    bool more=false;assert(h.prune(3500,more));rows=h.trafficTotals(0);assert(rows[0].internet==20);
    assert(h.saveTraffic(*p,5000));rows=h.trafficTotals(0);assert(rows[0].internet==20);
    cw::TrafficCapture capture;capture.start();capture.snapshot(*p);
    std::cout<<"PASS classification, deduplication, increments, session restart, retention; ETW status="<<p->status<<"\n";
}
