import asyncio
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('cw_mcp', ROOT/'mcp_server.py')
server=importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)

class McpTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.env=patch.dict(os.environ, {'LOCALAPPDATA':self.temp.name});self.env.start()
        root=Path(self.temp.name)/'ComputerWatch';root.mkdir()
        self.cache=root/'dashboard.json'
        self.cache.write_text(json.dumps({'schema_version':1,'generated_at_ms':1000,'refresh_pending':False,
            'inventory':{'board':[{'title':'主板'}]},
            'live':[{'timestamp_ms':900,'metrics':{'cpu_temp':None,'cpu_percent':0},'by_cpu':[{'pid':0},{'pid':42}],'by_memory':[]}],
            'network':{'data':{'traffic':{'started':10,'status':0,'lost':2,'session':[
                {'path':'C:\\a.exe','internet':0,'lan':50,'other':10,'rate':None},
                {'path':'C:\\b.exe','internet':100,'lan':0,'other':0,'rate':0}]}}}}),encoding='utf-8')
        self.db=sqlite3.connect(root/'history.db')
        self.db.executescript('CREATE TABLE traffic_bytes(time INTEGER,path TEXT,public INTEGER,lan INTEGER,other INTEGER); CREATE TABLE settings(id INTEGER,retention_days INTEGER); INSERT INTO settings VALUES(1,7);')
        self.db.executemany('INSERT INTO traffic_bytes VALUES(?,?,?,?,?)',[(100,'C:\\a.exe',10,5,0),(200,'C:\\a.exe',20,5,0),(200,'C:\\b.exe',0,100,1)])
        self.db.commit()
    def tearDown(self):
        self.db.close();self.env.stop();self.temp.cleanup()
    def test_traffic_aggregation_sort_pagination(self):
        a=server.get_traffic('custom',100,200,'lan',True,1,0)
        self.assertEqual(a['programs'][0]['name'],'b.exe');self.assertEqual(a['next_offset'],1)
        b=server.get_traffic('custom',100,200,'lan',True,1,1)
        self.assertEqual(b['programs'][0]['internet'],30);self.assertIsNone(b['next_offset'])
        self.assertEqual(b['total_programs'],2)
    def test_boundaries_and_empty(self):
        r=server.get_traffic('custom',100,100)
        self.assertEqual(r['programs'][0]['internet'],10)
        r=server.get_traffic('custom',300,400)
        self.assertEqual(r['programs'],[]);self.assertIsNone(r['last_record_ms'])
        with self.assertRaises(server.ToolError):server.get_traffic('custom',200,100)
        with self.assertRaises(server.ToolError):server.get_traffic('today',100,200)
    def test_cache_zero_missing_status(self):
        r=server.get_traffic('session',sort_by='lan')
        self.assertEqual(r['lost'],2);self.assertEqual(r['programs'][0]['internet'],0)
        self.assertIsNone(r['programs'][0]['rate'])
        self.assertEqual(server.get_hardware_details('board')['data'][0]['title'],'主板')
        r=server.get_realtime(include_rankings=True)
        self.assertEqual(r['samples'][0]['by_cpu'],[{'pid':42}])
        self.assertIsNone(r['samples'][0]['metrics']['cpu_temp'])
    def test_invalid_cache_and_old_database(self):
        self.cache.write_text('{"schema_version":1,"generated_at_ms":NaN}')
        with self.assertRaises(server.ToolError):server.get_hardware_details()
        self.db.execute('DROP TABLE traffic_bytes');self.db.commit()
        with self.assertRaises(server.ToolError):server.get_traffic()
    def test_refresh_default_and_explicit_traffic(self):
        with patch.object(server, 'signal_collector') as signal, patch.object(server.time, 'time_ns', return_value=1000000):
            server.refresh_dashboard()
            self.assertEqual([c.args[0] for c in signal.call_args_list],['Refresh'])
            signal.reset_mock()
            server.refresh_dashboard(start_traffic=True)
            self.assertEqual([c.args[0] for c in signal.call_args_list],['StartTraffic','Refresh'])
    def test_original_snapshot_and_history(self):
        columns=[c.strip() for c in server.SAMPLE_COLUMNS.split(',')]
        self.db.execute('CREATE TABLE samples('+','.join(c+' REAL' for c in columns)+')')
        self.db.execute('CREATE TABLE rankings(timestamp_ms INTEGER,sort_kind INTEGER,place INTEGER,pid INTEGER,name TEXT,cpu REAL,memory INTEGER)')
        row=[100,0]+[None]*(len(columns)-2)
        self.db.execute('INSERT INTO samples VALUES('+','.join('?' for _ in columns)+')',row)
        row[0]=200
        self.db.execute('INSERT INTO samples VALUES('+','.join('?' for _ in columns)+')',row)
        self.db.execute("INSERT INTO rankings VALUES(100,0,0,42,'test.exe',NULL,1024)")
        self.db.commit()
        result=server.get_snapshot(100)
        self.assertEqual(result['sample']['cpu_percent'],0)
        self.assertIsNone(result['by_cpu'][0]['cpu_percent'])
        self.assertEqual(result['by_cpu'][0]['place'],1)
        result=server.get_history(100,200,1)
        self.assertEqual(result['next_from_ms'],101)
        self.assertEqual(len(server.get_history(101,200)['samples']),1)
    def test_tools_and_argument_validation(self):
        async def check():
            tools=await server.mcp.list_tools()
            self.assertEqual({t.name for t in tools},{'get_snapshot','get_history','refresh_dashboard','get_hardware_details','get_realtime','get_traffic'})
            refresh=next(t for t in tools if t.name=='refresh_dashboard')
            self.assertFalse(refresh.annotations.readOnlyHint)
            try:
                result=await server.mcp.call_tool('get_traffic',{'sort_by':'DROP TABLE traffic_bytes'})
            except Exception:
                return
            self.fail(f'invalid enum accepted: {result}')
        asyncio.run(check())

if __name__=='__main__':unittest.main()
