#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define VID 0x2808
#define PID 0x9348
static libusb_device_handle*h;
static int wr(unsigned r,unsigned v,unsigned i){return libusb_control_transfer(h,0x40,r,v,i,NULL,0,800);}
// 9368ReadData(addr,len): 0x34=0xff; 0x34=3; 0x6f(wIndex=addr,wValue=len); bulk-read
static int rd9368(unsigned addr,unsigned char*buf,int len){
    wr(0x34,0xff,0); wr(0x34,3,0); wr(0x6f,len,addr);
    int got=0; int r=libusb_bulk_transfer(h,0x83,buf,len,&got,2000);
    return r?-1:got;
}
int main(int c,char**a){
    unsigned addr=(c>1)?(unsigned)strtol(a[1],0,16):0x9080;
    int len=(c>2)?atoi(a[2]):5120;
    libusb_context*x=0;libusb_init(&x);
    h=libusb_open_device_with_vid_pid(x,VID,PID);
    if(!h){fprintf(stderr,"нет устройства\n");return 1;}
    libusb_set_auto_detach_kernel_driver(h,1);libusb_claim_interface(h,0);
    // wake
    wr(0x34,0xff,0); wr(0x34,2,0); wr(0x6f,0,0xff00);
    // enter sensor mode
    wr(0x3b,1,0x1f); wr(0x3b,1,0x1e); usleep(10000);
    unsigned char b1=0; libusb_control_transfer(h,0xC0,0x3a,0,0x1d,&b1,1,800);
    unsigned char fp=0; libusb_control_transfer(h,0xC0,0x43,0,0,&fp,1,800);
    printf("finger=%d mode3a[1d]=0x%02x\n",fp,b1);
    // читаем кадр правильным методом
    static unsigned char buf[65536]; if(len>(int)sizeof buf)len=sizeof buf;
    int got=rd9368(addr,buf,len);
    printf("9368ReadData addr=0x%04x len=%d -> got=%d\n",addr,len,got);
    if(got>0){
        int mn=255,mx=0; long s=0; for(int i=0;i<got;i++){int v=buf[i];if(v<mn)mn=v;if(v>mx)mx=v;s+=v;}
        printf("  min=%d max=%d avg=%ld first16: ",mn,mx,s/got);
        for(int i=0;i<16&&i<got;i++)printf("%02x ",buf[i]); printf("\n");
        FILE*f=fopen("./frame.raw","wb"); if(f){fwrite(buf,1,got,f);fclose(f);}
    }
    libusb_release_interface(h,0);libusb_close(h);libusb_exit(x);
    return 0;
}
