/* Цикл-захват кадров для enroll: ждёт палец (reg 0x43==1), снимает кадр 64x80, сохраняет.
 * Гейт на детекте пальца (проверено: 0x43 = 0 без / 1 с пальцем).
 * Сборка: gcc -O2 -o ftenroll ftenroll.c $(pkg-config --cflags --libs libusb-1.0)
 * Запуск:  sudo systemctl stop fprintd; sudo ./ftenroll [N=12] [outdir=~/focaltech-fp/cap]
 * Приложи палец, подними, снова приложи (чуть смещая/поворачивая) — пока не наберётся N кадров.
 */
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#define VID 0x2808
#define PID 0x9348
#define W 64
#define H 80
#define FRAME (W*H)
static libusb_device_handle *h;
static int wr(unsigned r,unsigned v,unsigned i){return libusb_control_transfer(h,0x40,r,v,i,NULL,0,800);}
static int rdreg(unsigned r,unsigned i,unsigned char*b,int n){return libusb_control_transfer(h,0xC0,r,0,i,b,n,800);}
static int finger(void){ unsigned char f=0; rdreg(0x43,0,&f,1); return f; }
static int rd9368(unsigned char*buf,int len){
    wr(0x34,0xff,0); wr(0x34,3,0); wr(0x6f,len,0x9080);
    int got=0; return libusb_bulk_transfer(h,0x83,buf,len,&got,2000)?-1:got;
}
static double stdev(unsigned char*b,int n){ double m=0; for(int i=0;i<n;i++)m+=b[i]; m/=n;
    double s=0; for(int i=0;i<n;i++){double d=b[i]-m; s+=d*d;} return sqrt(s/n); }

int main(int c,char**a){
    int N=(c>1)?atoi(a[1]):12;
    const char*dir=(c>2)?a[2]:"./cap";
    char cmd[512]; snprintf(cmd,sizeof cmd,"mkdir -p '%s'",dir); if(system(cmd)){}
    libusb_context*x=0; libusb_init(&x);
    h=libusb_open_device_with_vid_pid(x,VID,PID);
    if(!h){fprintf(stderr,"нет устройства 2808:9348 (fprintd остановлен? sudo?)\n");return 1;}
    libusb_set_auto_detach_kernel_driver(h,1); libusb_claim_interface(h,0);
    /* wake + sensor mode */
    wr(0x34,0xff,0); wr(0x34,2,0); wr(0x6f,0,0xff00);
    wr(0x3b,1,0x1f); wr(0x3b,1,0x1e); usleep(10000);
    printf("Готов. Приложи палец к кнопке (не нажимая). Нужно %d кадров.\n",N);
    static unsigned char buf[FRAME];
    int saved=0, waitrel=0;
    for(int iter=0; iter<20000 && saved<N; iter++){
        int f=finger();
        if(waitrel){ if(!f) waitrel=0; usleep(50000); continue; }
        if(!f){ usleep(50000); continue; }
        usleep(60000); /* дать пальцу лечь */
        int got=rd9368(buf,FRAME);
        if(got!=FRAME){ usleep(50000); continue; }
        double sd=stdev(buf,got);
        if(sd<12.0){ usleep(50000); continue; } /* пустой/слабый кадр — пропустить */
        char path[600]; snprintf(path,sizeof path,"%s/cap%02d.raw",dir,saved);
        FILE*ff=fopen(path,"wb"); if(ff){fwrite(buf,1,got,ff);fclose(ff);}
        printf("  [%2d/%d] %s  stdev=%.1f — подними и приложи снова (чуть иначе)\n",saved+1,N,path,sd);
        saved++; waitrel=1;
    }
    printf("Готово: сохранено %d кадров в %s\n",saved,dir);
    libusb_release_interface(h,0); libusb_close(h); libusb_exit(x);
    return saved>0?0:2;
}
