// SPDX-License-Identifier: MIT
// Small playable Linux framebuffer game. No guest libc or package install needed.
typedef unsigned int u32;typedef unsigned long u64;typedef signed long i64;
void* memset(void* destination,int value,u64 count) {volatile unsigned char* p=destination;while(count--)*p++=(unsigned char)value;return destination;}
static i64 call(i64 n,i64 a,i64 b,i64 c,i64 d,i64 e,i64 f) {
  register i64 x10 __asm__("a0")=a,x11 __asm__("a1")=b,x12 __asm__("a2")=c,x13 __asm__("a3")=d,x14 __asm__("a4")=e,x15 __asm__("a5")=f,x17 __asm__("a7")=n;
  __asm__ volatile("ecall":"+r"(x10):"r"(x11),"r"(x12),"r"(x13),"r"(x14),"r"(x15),"r"(x17):"memory");return x10;
}
static void text(const char* p) {u64 n=0;while(p[n])++n;call(64,1,(i64)p,n,0,0,0);}
static void number(u64 n) {char b[32];int at=32;do {b[--at]='0'+n%10;n/=10;}while(n);call(64,1,(i64)(b+at),32-at,0,0,0);}
static void quit(int code) {call(94,code,0,0,0,0,0);for(;;);}
static int openfile(const char* p,int flags) {return (int)call(56,-100,(i64)p,flags,0,0,0);}
static u32 *pixels;static int width,height,pitch;static unsigned red,green,blue;
static u32 color(unsigned r,unsigned g,unsigned b) {return r<<red|g<<green|b<<blue;}
static void rect(int x,int y,int w,int h,u32 c) {
  for(int j=0;j<h;++j)if(y+j>=0 && y+j<height)for(int i=0;i<w;++i)if(x+i>=0 && x+i<width)pixels[(y+j)*pitch+x+i]=c;
}
static unsigned bricks[4][10];
static void wall(void) {
  for(int y=0;y<4;++y)for(int x=0;x<10;++x) {bricks[y][x]=1;rect(16+x*(width-32)/10,40+y*28,(width-32)/10-4,22,color(240-y*40,80+y*40,60+y*45));}
}
void main_start(u64* stack) {
  int automatic=0;
  if(stack[0]>1) {const char* p=(const char*)stack[2];automatic=p[0]=='-' && p[1]=='-' && p[2]=='t';}
  int fd=openfile("/dev/fb0",2);u32 variable[40]={0};unsigned char fixed[80]={0};
  if(fd<0 || call(29,fd,0x4600,(i64)variable,0,0,0)<0 || call(29,fd,0x4602,(i64)fixed,0,0,0)<0 || variable[6]!=32 || variable[9]!=8 || variable[12]!=8 || variable[15]!=8) {text("RVVM: 32-bit Linux framebuffer unavailable\n");quit(1);}
  width=(int)variable[0];height=(int)variable[1];pitch=*(u32*)(fixed+48)/4;red=variable[8];green=variable[11];blue=variable[14];
  if(width<320 || height<240 || width>4096 || height>4096 || pitch<width)quit(1);
  pixels=(u32*)call(222,0,(i64)pitch*height*4,3,1,fd,0);
  if((i64)pixels<0)quit(1);
  int input=-1;
  for(int n=0;n<16;++n) {
    char path[]="/dev/input/event00";path[16]='0'+n/10;path[17]='0'+n%10;
    if(n<10) {path[16]='0'+n;path[17]=0;} // /dev/input/event0
    int candidate=openfile(path,0x800);if(candidate<0)continue;
    unsigned char name[128]={0};call(29,candidate,0x80804506,(i64)name,0,0,0);
    if(name[0]=='R' && name[1]=='V' && name[2]=='V') {input=candidate;break;}call(57,candidate,0,0,0,0,0);
  }
  if(input<0 && !automatic) {text("RVVM: keyboard evdev unavailable\n");quit(1);}
  text("RVVM Breakout: Left/Right or A/D, Esc exits. Click the RVVM display first.\n");
  rect(0,0,width,height,0);rect(0,0,width,6,color(80,120,180));rect(0,0,6,height,color(80,120,180));rect(width-6,0,6,height,color(80,120,180));wall();
  int paddle=width/2-50,x=width/2,y=height/2,vx=4,vy=-4,left=0,right=0,score=0,frames=0,lives=3;
  const u32 ball=color(250,250,250),bat=color(60,200,255);struct {i64 sec,nsec;} delay={0,16666666};
  for(;;) {
    struct {i64 sec,usec;unsigned short type,code;int value;} event;
    while(input>=0 && call(63,input,(i64)&event,sizeof(event),0,0,0)==sizeof(event))if(event.type==1) {
      if(event.code==1 && event.value)goto done;
      if(event.code==105 || event.code==30)left=event.value!=0;
      if(event.code==106 || event.code==32)right=event.value!=0;
    }
    rect(paddle,height-35,100,12,0);rect(x,y,10,10,0);
    if(automatic) {paddle=x-45;if(paddle<8)paddle=8;if(paddle>width-108)paddle=width-108;}
    else {paddle+=(right-left)*9;if(paddle<8)paddle=8;if(paddle>width-108)paddle=width-108;}
    x+=vx;y+=vy;if(x<8) {x=8;vx=-vx;}if(x>width-18) {x=width-18;vx=-vx;}if(y<8) {y=8;vy=-vy;}
    if(vy>0 && y+10>=height-35 && y<height-20 && x+10>=paddle && x<=paddle+100) {vy=-4;y=height-46;vx=x-paddle<33?-5:x-paddle>66?5:3;}
    for(int by=0;by<4;++by)for(int bx=0;bx<10;++bx)if(bricks[by][bx]) {
      int ax=16+bx*(width-32)/10,ay=40+by*28;
      if(x+10>ax && x<ax+(width-32)/10-4 && y+10>ay && y<ay+22) {bricks[by][bx]=0;rect(ax,ay,(width-32)/10-4,22,0);vy=-vy;score++;goto hit;}
    }
hit:
    if(y>height) {x=width/2;y=height/2;vy=-4;if(--lives<=0) {lives=3;score=0;wall();}}
    if(score && score%40==0) {wall();score++;}
    rect(paddle,height-35,100,12,bat);rect(x,y,10,10,ball);
    ++frames;if(automatic && frames>=240)break;
    call(101,(i64)&delay,0,0,0,0,0);
  }
done:
  text("RVVM_GAME_OK frames=");number(frames);text(" score=");number(score);text("\n");quit(automatic && score==0?1:0);
}
__asm__(".section .text.start\n.global _start\n_start:\n.option push\n.option norelax\n la gp,__global_pointer$\n.option pop\n mv a0,sp\n call main_start\n");
