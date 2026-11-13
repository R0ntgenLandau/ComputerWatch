"""Exercise the real stdio protocol. --refresh requests live collection explicitly."""
import argparse
import asyncio
from datetime import timedelta
import json
from pathlib import Path
import sys
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

async def run(args):
    params=StdioServerParameters(command=sys.executable,args=[str(args.script)])
    async with stdio_client(params) as (read,write):
        async with ClientSession(read,write) as session:
            await session.initialize()
            tools=await session.list_tools()
            assert len(tools.tools)==6
            async def call(name,arguments=None):
                result=await session.call_tool(name,arguments or {},read_timeout_seconds=timedelta(seconds=35))
                assert not result.isError,(name,result)
                payload=result.structuredContent
                if payload is None:
                    payload=json.loads(next(c.text for c in result.content if c.type=='text'))
                return payload
            if args.refresh:
                refreshed=await call('refresh_dashboard',{'start_traffic':args.start_traffic})
                assert not refreshed['refresh_pending']
            snapshot=await call('get_snapshot')
            stamp=snapshot['sample']['timestamp_ms']
            history=await call('get_history',{'from_ms':stamp-60000,'to_ms':stamp,'limit':20})
            assert history['samples']
            details=await call('get_hardware_details')
            for section in ['inventory','cpu','gpu','memory','disk','network','board','displays','settings']:
                section_data=await call('get_hardware_details',{'section':section})
                assert section_data['section']==section
            live=await call('get_realtime',{'include_rankings':True})
            assert live['samples']
            traffic={}
            for period in ['session','today','week','custom']:
                arguments={'period':period,'limit':3,'sort_by':'lan'}
                if period=='custom':arguments.update(from_ms=stamp-60000,to_ms=stamp)
                traffic[period]=await call('get_traffic',arguments)
            bad=await session.call_tool('get_traffic',{'sort_by':'invalid'})
            assert bad.isError
            data=details['data']
            report={
                'tools':[t.name for t in tools.tools],
                'generated_at_ms':details['generated_at_ms'],
                'snapshot_age_ms':snapshot['age_ms'],
                'live_points':len(live['samples']),
                'cpu_cores':len(data['cpu']['data']['cores']),
                'cpu_valid_loads':sum(c['loadPercent'] is not None for c in data['cpu']['data']['cores']),
                'disks':len((data['disk']['data'] or {}).get('disks',[])),
                'memory_timing_status':(data['memory']['data']['timings'] or {}).get('status'),
                'network_adapters':len((data['network']['data']['adapters'] or {}).get('connections',[])),
                'displays':len(data['displays'] or []),
                'traffic_status':traffic['session']['status'],
                'traffic_lost':traffic['session']['lost'],
                'traffic_programs':{k:v['total_programs'] for k,v in traffic.items()},
                'sensors':{k:snapshot['sample'][k] for k in ['cpu_temp','cpu_watts','gpu_temp','gpu_watts']},
            }
            print(json.dumps(report,ensure_ascii=False,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--script',type=Path,default=Path(__file__).resolve().parents[1]/'mcp_server.py')
    p.add_argument('--refresh',action='store_true')
    p.add_argument('--start-traffic',action='store_true')
    asyncio.run(run(p.parse_args()))
