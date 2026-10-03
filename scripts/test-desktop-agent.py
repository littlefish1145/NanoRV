"""Run with Linux Python; exercise authentication and launch argument boundaries."""
import importlib.util, json, pathlib, threading, unittest, urllib.error, urllib.request
from unittest.mock import patch
file=pathlib.Path(__file__).with_name('desktop-agent.py')
spec=importlib.util.spec_from_file_location('agent',file);agent=importlib.util.module_from_spec(spec);spec.loader.exec_module(agent)
class DesktopTests(unittest.TestCase):
    def test_desktop_fields_keep_arguments_separate(self):
        entry={'exec':'echo "%c" %i %F %%','name':'two words','icon':'my icon','file':'/tmp/app.desktop'}
        with patch.object(agent,'applications',return_value={'sample':entry}):
            result=agent.app_command('sample',['; touch /tmp/never-created'])
        self.assertEqual(result,['echo','two words','--icon','my icon','%','; touch /tmp/never-created'])
    def test_reject_invalid_argument_types_and_fields(self):
        for args in ('text',[None],['\0'],['x']*129):
            with self.assertRaises(ValueError):agent.app_command('echo',args)
        with patch.object(agent,'applications',return_value={'sample':{'exec':'echo foo%F'}}):
            with self.assertRaises(ValueError):agent.app_command('sample',[])
    def test_http_auth_body_validation_and_launch(self):
        class Fake:
            session={'token':'a'*64}
            def launch(self,name,args):return {'command':agent.app_command(name,args)}
        agent.Handler.broker=Fake()
        server=agent.ThreadingHTTPServer(('127.0.0.1',0),agent.Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        base=f'http://127.0.0.1:{server.server_port}'
        def request(path,data=None,authorized=True):
            headers={'Authorization':'Bearer '+'a'*64} if authorized else {}
            return urllib.request.urlopen(urllib.request.Request(base+path,data=data,headers=headers),timeout=3)
        try:
            with self.assertRaises(urllib.error.HTTPError) as error:request('/health',authorized=False)
            self.assertEqual(error.exception.code,401)
            with request('/health') as response:self.assertTrue(json.load(response)['ready'])
            with request('/launch',json.dumps({'app':'echo','args':['a b','; false']}).encode()) as response:
                self.assertEqual(json.load(response)['command'],['echo','a b','; false'])
            with self.assertRaises(urllib.error.HTTPError) as error:request('/launch',b'[]')
            self.assertEqual(error.exception.code,400)
        finally:server.shutdown();server.server_close();thread.join()
if __name__=='__main__':unittest.main()
