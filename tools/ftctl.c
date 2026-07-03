// ftctl — чтение "регистра" FocalTech FT9348 через vendor CONTROL transfer.
// Из RE Windows-драйвера: ReadDevice = control IN bmRequestType=0xC0, bRequest=<reg>, wValue, wIndex, wLength.
// QuerySensorPinStatus(детект пальца) = reg 0x43, len 1, байт==1 -> палец.
// Сборка: gcc -O2 -o ftctl ftctl.c $(pkg-config --cflags --libs libusb-1.0)
// Запуск:  sudo ./ftctl <bRequest_hex> [len=1] [wValue=0] [wIndex=0] [timeout_ms=1500]
//   пример: sudo ./ftctl 43        (детект пальца, 1 байт)
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>

#define VID 0x2808
#define PID 0x9348

int main(int argc, char **argv){
    if(argc<2){fprintf(stderr,"usage: %s <bRequest_hex> [len] [wValue] [wIndex] [timeout_ms]\n",argv[0]);return 2;}
    unsigned bReq=(unsigned)strtol(argv[1],0,16);
    int len=(argc>2)?atoi(argv[2]):1;
    unsigned wValue=(argc>3)?(unsigned)strtol(argv[3],0,16):0;
    unsigned wIndex=(argc>4)?(unsigned)strtol(argv[4],0,16):0;
    int tmo=(argc>5)?atoi(argv[5]):1500;

    libusb_context*ctx=0; if(libusb_init(&ctx)){fprintf(stderr,"init fail\n");return 1;}
    libusb_device_handle*h=libusb_open_device_with_vid_pid(ctx,VID,PID);
    if(!h){fprintf(stderr,"устройство %04x:%04x не открылось (sudo? fprintd stop?)\n",VID,PID);return 1;}
    libusb_set_auto_detach_kernel_driver(h,1);
    libusb_claim_interface(h,0); // не критично для control-to-device

    unsigned char buf[512]={0};
    if(len>(int)sizeof buf) len=sizeof buf;
    printf("CTRL IN  bmRequestType=0xC0 bRequest=0x%02x wValue=0x%04x wIndex=0x%04x wLength=%d\n",bReq,wValue,wIndex,len);
    int r=libusb_control_transfer(h,0xC0,(uint8_t)bReq,(uint16_t)wValue,(uint16_t)wIndex,buf,(uint16_t)len,tmo);
    if(r<0){printf("result: %s (%d)\n",libusb_error_name(r),r);}
    else{
        printf("result: OK, got=%d bytes: ",r);
        for(int i=0;i<r;i++)printf("%02x ",buf[i]);
        printf("\n");
        if(r>=1)printf("  -> byte[0]=0x%02x  (finger present если ==1)\n",buf[0]);
    }
    libusb_release_interface(h,0); libusb_close(h); libusb_exit(ctx);
    return 0;
}
