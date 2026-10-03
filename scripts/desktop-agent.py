#!/usr/bin/python3
"""Authenticated VM-local app broker. GUI children run as the desktop user."""
import configparser, hmac, json, os, pathlib, pwd, shlex, shutil, struct
import subprocess, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def applications():
    result={}
    for directory in ('/usr/share/applications','/usr/local/share/applications'):
        for file in sorted(pathlib.Path(directory).glob('*.desktop')):
            parser=configparser.ConfigParser(interpolation=None,strict=False)
            try:
                parser.read(file,encoding='utf-8');entry=parser['Desktop Entry']
                if entry.get('Type')!='Application' or entry.get('Hidden','false')=='true' or entry.get('NoDisplay','false')=='true':continue
                if not entry.get('Exec'):continue
                result[file.stem]={'id':file.stem,'name':entry.get('Name',file.stem),'exec':entry['Exec'],'icon':entry.get('Icon',''),'file':str(file)}
            except (configparser.Error,UnicodeError,KeyError):continue
    return result

def app_command(name,args):
    if not isinstance(name,str) or not name or len(name)>256:raise ValueError('invalid application')
    if not isinstance(args,list) or len(args)>128 or any(not isinstance(a,str) or '\0' in a or len(a)>8192 for a in args):raise ValueError('invalid arguments')
    app=applications().get(name.removesuffix('.desktop'))
    if app:
        command=[]
        for word in shlex.split(app['exec']):
            if word in ('%f','%F','%u','%U'):continue
            if word=='%i':
                if app['icon']:command+=['--icon',app['icon']]
            elif word=='%c':command.append(app['name'])
            elif word=='%k':command.append(app['file'])
            else:
                # Literal %% is supported; unsupported embedded field codes fail.
                literal=word.replace('%%','\0')
                if '%' in literal:raise ValueError('unsupported desktop field code')
                command.append(literal.replace('\0','%'))
    else:command=[name]
    if not command or not shutil.which(command[0]):raise ValueError('application is not installed')
    return command+args

class Broker:
    def __init__(self,session):
        self.session=session
        self.user=pwd.getpwnam('rvvm')
        self.children={};self.lock=threading.Lock()
        directory=pathlib.Path('/run/rvvm-desktop');directory.mkdir(mode=0o750,exist_ok=True)
        os.chown(directory,0,self.user.pw_gid)
        directory.chmod(0o750) # service UMask=0077 must not block the GUI user's xauth.
        auth=directory/'Xauthority'
        fields=(b'',str(session['display_number']).encode(),b'MIT-MAGIC-COOKIE-1',bytes.fromhex(session['cookie']))
        auth.write_bytes(struct.pack('>H',65535)+b''.join(struct.pack('>H',len(f))+f for f in fields))
        os.chown(auth,0,self.user.pw_gid);auth.chmod(0o640)
        runtime=pathlib.Path(f'/run/user/{self.user.pw_uid}')
        runtime.mkdir(mode=0o700,parents=True,exist_ok=True);os.chown(runtime,self.user.pw_uid,self.user.pw_gid)
        self.environment=dict(os.environ,HOME=self.user.pw_dir,USER='rvvm',LOGNAME='rvvm',
            DISPLAY=f"10.0.2.2:{session['display_number']}",XAUTHORITY=str(auth),XDG_RUNTIME_DIR=str(runtime),
            GDK_BACKEND='x11',QT_QPA_PLATFORM='xcb',QT_X11_NO_MITSHM='1')
        self.environment.pop('DBUS_SESSION_BUS_ADDRESS',None)
    def launch(self,name,args):
        command=app_command(name,args)
        with self.lock:
            if sum(p.poll() is None for p in self.children.values())>=64:raise ValueError('too many running applications')
            if len(self.children)>=256:
                for pid in list(self.children):
                    if self.children[pid].poll() is not None:del self.children[pid]
                    if len(self.children)<256:break
            log=open('/var/log/rvvm-desktop-apps.log','ab',buffering=0)
            try:
                process=subprocess.Popen(['/usr/bin/dbus-run-session','--']+command,
                    cwd=self.user.pw_dir,env=self.environment,user=self.user.pw_uid,group=self.user.pw_gid,
                    extra_groups=[self.user.pw_gid],start_new_session=True,stdin=subprocess.DEVNULL,stdout=log,stderr=log)
            finally:log.close()
            self.children[process.pid]=process
            return {'pid':process.pid,'command':command}

class Handler(BaseHTTPRequestHandler):
    broker=None
    def log_message(self,*args):pass # Never log authentication headers.
    def reply(self,status,body):
        data=json.dumps(body,ensure_ascii=False).encode()
        self.send_response(status);self.send_header('Content-Type','application/json; charset=utf-8')
        self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def authorized(self):
        expected='Bearer '+self.broker.session['token']
        if not hmac.compare_digest(self.headers.get('Authorization',''),expected):
            self.reply(401,{'error':'authentication required'});return False
        return True
    def do_GET(self):
        if not self.authorized():return
        if self.path=='/health':self.reply(200,{'ready':True,'protocol':1,'user':'rvvm'})
        elif self.path=='/apps':self.reply(200,[{'id':a['id'],'name':a['name']} for a in applications().values()])
        elif self.path=='/logs':
            file=pathlib.Path('/var/log/rvvm-desktop-apps.log')
            if file.exists():
                with file.open('rb') as stream:
                    stream.seek(max(0,file.stat().st_size-16384));self.reply(200,{'text':stream.read().decode(errors='replace')})
            else:self.reply(200,{'text':''})
        elif self.path.startswith('/status/'):
            try:
                pid=int(self.path.removeprefix('/status/'))
                with self.broker.lock:
                    process=self.broker.children.get(pid)
                    if process is None:self.reply(404,{'error':'unknown application'});return
                    code=process.poll()
                self.reply(200,{'pid':pid,'running':code is None,'exit_code':code})
            except ValueError:self.reply(400,{'error':'invalid pid'})
        else:self.reply(404,{'error':'unknown endpoint'})
    def do_POST(self):
        if not self.authorized():return
        try:
            length=int(self.headers.get('Content-Length','-1'))
            if not 0<=length<=65536:raise ValueError('invalid content length')
            self.connection.settimeout(10)
            request=json.loads(self.rfile.read(length))
            if not isinstance(request,dict):raise ValueError('expected an object')
            if self.path=='/launch':self.reply(200,self.broker.launch(request.get('app'),request.get('args',[])))
            elif self.path=='/shutdown':
                self.reply(202,{'stopping':True})
                threading.Timer(1,lambda:subprocess.Popen(['/sbin/poweroff'])).start()
            else:self.reply(404,{'error':'unknown endpoint'})
        except (ValueError,KeyError,OSError,TimeoutError) as error:self.reply(400,{'error':str(error)})

if __name__=='__main__':
    session=json.loads(pathlib.Path(sys.argv[1]).read_text())
    if len(session['token'])!=64 or len(session['cookie'])!=32:raise ValueError('invalid session credentials')
    bytes.fromhex(session['token']);bytes.fromhex(session['cookie'])
    Handler.broker=Broker(session)
    ThreadingHTTPServer(('0.0.0.0',46000),Handler).serve_forever()
