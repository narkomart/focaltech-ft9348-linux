/* ftbio — автономная биометрия FocalTech FT9348: захват с сенсора + проприетарный матчер (dlopen .so).
 * Команды:
 *   ftbio enroll <user>     — снять кадры пальца, обучить, сохранить шаблон в /etc/focaltech/<user>.tpl
 *   ftbio verify <user>     — снять кадр, сравнить с шаблоном; exit 0 = совпало, 1 = нет, 2 = ошибка/таймаут
 *   ftbio verify-file <user> <frame.raw>  — оффлайн-проверка одного кадра (для тестов)
 * Требует root (доступ к USB, чтение /etc/focaltech). Сборка: см. bio/build.sh
 * Матчер: your FocalTech libfprint .so (see README), focal_* по base+offset (offsets из nm, БЕЗ +0x100000).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <errno.h>

#define SO      "/opt/focaltech/matcher.so"
#define TPLDIR  "/etc/focaltech"
#define VID 0x2808
#define PID 0x9348
#define W 64
#define H 80
#define FRAME (W*H)
#define FID 1

/* offsets (nm on your FocalTech .so) */
#define OFF_ff_algo_do_config    0x43c20
#define OFF_InitFuncLog          0x6e300
#define OFF_EnrollByImage        0x75a70
#define OFF_IdentifyByImage      0x8d190
#define OFF_GetMatchFingerId     0x8af40
#define OFF_GetMatchScores       0x8aee0
#define OFF_SaveAlgTplData       0x794b0
#define OFF_InitAlgTplData       0x6e110
#define OFF_InitFuncGetImage     0x6e2a0
#define OFF_InitFuncSpiRead      0x6e2c0
#define OFF_InitFuncSpiWrite     0x6e2b0
#define OFF_InitFuncUsleep       0x6e2d0
#define OFF_InitFuncGetSysTime   0x6e2f0
#define OFF_InitFuncGetMcuStatus 0x6e2e0
#define OFF_InitFuncReadImgInit  0x6e330
#define OFF_InitFuncReadImgStart 0x6e340
#define OFF_InitFuncReadImgEnd   0x6e350
#define OFF_InitFuncWhilePoint   0x6e370
#define OFF_InitFuncGetImageBase 0x6e360
#define OFF_g_device             0x211400
#define OFF_g_debuginfo          0x30c4c19
#define OFF_g_lib_log_level      0x2113f8
#define OFF_g_log_level          0x2113fc

/* ---- алгоритм ---- */
static unsigned long base;
static uint8_t g_frame[FRAME];
static int  cb_get_image(uint8_t*b,uint32_t n){memcpy(b,g_frame,n<FRAME?n:FRAME);return 0;}
static int  cb_spi_rw(uint8_t*a,uint8_t*b,uint32_t l){(void)a;(void)b;(void)l;return 0;}
static void cb_usleep(uint32_t us){struct timespec t={us/1000000,(long)(us%1000000)*1000};nanosleep(&t,0);}
static uint32_t cb_systime(void){static uint32_t t;return t+=10;}
static int  cb_ret0(void){return 0;}
static void ftlog(const char*f,...){if(!getenv("FTLOG"))return;va_list ap;__builtin_va_start(ap,f);vfprintf(stderr,f,ap);fputc('\n',stderr);__builtin_va_end(ap);}

static int (*algo_config)(void);
static int (*fn_enroll)(uint16_t,uint16_t,uint8_t*);
static int (*fn_identify)(uint8_t*,uint8_t,uint8_t*,uint8_t*);
static uint32_t (*fn_match_fid)(void);
static uint32_t (*fn_match_score)(void);
static int (*fn_save)(uint16_t,uint8_t*,uint32_t*,uint8_t*);
static int (*fn_init_tpl)(uint16_t,uint8_t,uint32_t,uint8_t*);
static void (*fn_init_log)(void*);

static const char*so_path(void){
  const char*e=getenv("FT_SO"); if(e)return e;
  if(access("/opt/focaltech/matcher.so",R_OK)==0) return "/opt/focaltech/matcher.so";
  return SO;
}
static int algo_open(void){
  void*h=dlopen(so_path(),RTLD_LAZY|RTLD_GLOBAL);
  if(!h){fprintf(stderr,"dlopen(%s): %s\n",so_path(),dlerror());return -1;}
  struct link_map*lm=0; dlinfo(h,RTLD_DI_LINKMAP,&lm); base=lm->l_addr;
  fn_init_log =(void*)(base+OFF_InitFuncLog);
  algo_config =(void*)(base+OFF_ff_algo_do_config);
  fn_enroll   =(void*)(base+OFF_EnrollByImage);
  fn_identify =(void*)(base+OFF_IdentifyByImage);
  fn_match_fid=(void*)(base+OFF_GetMatchFingerId);
  fn_match_score=(void*)(base+OFF_GetMatchScores);
  fn_save     =(void*)(base+OFF_SaveAlgTplData);
  fn_init_tpl =(void*)(base+OFF_InitAlgTplData);
  #define REG(off,fn) ((void(*)(void*))(base+off))((void*)(fn))
  fn_init_log((void*)ftlog);
  REG(OFF_InitFuncGetImage,cb_get_image); REG(OFF_InitFuncSpiRead,cb_spi_rw); REG(OFF_InitFuncSpiWrite,cb_spi_rw);
  REG(OFF_InitFuncUsleep,cb_usleep); REG(OFF_InitFuncGetSysTime,cb_systime); REG(OFF_InitFuncGetMcuStatus,cb_ret0);
  REG(OFF_InitFuncReadImgInit,cb_ret0); REG(OFF_InitFuncReadImgStart,cb_ret0); REG(OFF_InitFuncReadImgEnd,cb_ret0);
  REG(OFF_InitFuncWhilePoint,cb_ret0); REG(OFF_InitFuncGetImageBase,cb_ret0);
  static uint8_t gdev[224];
  *(uint16_t*)(gdev+210)=0x9362; *(int*)(gdev+212)=W; *(int*)(gdev+216)=H; *(void**)(base+OFF_g_device)=gdev;
  int r=algo_config();
  if(r){fprintf(stderr,"ff_algo_do_config=%d\n",r);return -1;}
  if(getenv("FTLOG")){ fn_init_log((void*)ftlog); *(uint8_t*)(base+OFF_g_debuginfo)=2;
    *(int*)(base+OFF_g_lib_log_level)=0; *(int*)(base+OFF_g_log_level)=0; }
  return 0;
}

/* ---- сенсор ---- */
static libusb_device_handle*uh;
static int wr(unsigned r,unsigned v,unsigned i){return libusb_control_transfer(uh,0x40,r,v,i,NULL,0,800);}
static int finger(void){unsigned char f=0;libusb_control_transfer(uh,0xC0,0x43,0,0,&f,1,800);return f;}
static int grab(unsigned char*buf){
  wr(0x34,0xff,0);wr(0x34,3,0);wr(0x6f,FRAME,0x9080);
  int got=0;return libusb_bulk_transfer(uh,0x83,buf,FRAME,&got,2000)?-1:got;
}
static double stdev(unsigned char*b,int n){double m=0;for(int i=0;i<n;i++)m+=b[i];m/=n;
  double s=0;for(int i=0;i<n;i++){double d=b[i]-m;s+=d*d;}return sqrt(s/n);}
static libusb_context*uctx;
static int sensor_open(void){
  libusb_init(&uctx);
  uh=libusb_open_device_with_vid_pid(uctx,VID,PID);
  if(!uh){fprintf(stderr,"нет устройства %04x:%04x (fprintd остановлен? root?)\n",VID,PID);return -1;}
  libusb_set_auto_detach_kernel_driver(uh,1); libusb_claim_interface(uh,0);
  wr(0x34,0xff,0);wr(0x34,2,0);wr(0x6f,0,0xff00);
  wr(0x3b,1,0x1f);wr(0x3b,1,0x1e);usleep(10000);
  return 0;
}
static void sensor_close(void){ if(uh){libusb_release_interface(uh,0);libusb_close(uh);} if(uctx)libusb_exit(uctx); }
/* ждать палец, снять кадр c контентом; timeout_ms<=0 = без ожидания одна попытка. ret 0 ok */
static int wait_and_grab(unsigned char*buf,int timeout_ms){
  int waited=0;
  for(;;){
    if(finger()){ usleep(60000); if(grab(buf)==FRAME && stdev(buf,FRAME)>=12.0) return 0; }
    usleep(50000); waited+=50;
    if(timeout_ms>0 && waited>=timeout_ms) return -1;
    if(timeout_ms==0) return -1;
  }
}
/* ждать снятия пальца */
static void wait_release(void){int n=0;while(finger()&&n<200){usleep(50000);n++;}}

/* ---- шаблон ---- */
static const char*tpldir(void){const char*e=getenv("FT_TPLDIR");return e?e:TPLDIR;}
static int tpl_path(const char*user,char*out,size_t n){return snprintf(out,n,"%s/%s.tpl",tpldir(),user);}
static int tpl_write(const char*user,uint8_t type,uint32_t size,uint8_t*data){
  mkdir(tpldir(),0700);
  char p[512]; tpl_path(user,p,sizeof p);
  FILE*f=fopen(p,"wb"); if(!f){fprintf(stderr,"write %s: %s\n",p,strerror(errno));return -1;}
  fwrite(&type,1,1,f); fwrite(&size,4,1,f); fwrite(data,1,size,f); fclose(f);
  chmod(p,0600);
  return 0;
}
static int tpl_read(const char*user,uint8_t*type,uint32_t*size,uint8_t*data,uint32_t cap){
  char p[512]; tpl_path(user,p,sizeof p);
  FILE*f=fopen(p,"rb"); if(!f)return -1;
  if(fread(type,1,1,f)!=1||fread(size,4,1,f)!=1||*size>cap){fclose(f);return -1;}
  size_t rd=fread(data,1,*size,f); fclose(f);
  return rd==*size?0:-1;
}

/* ---- команды ---- */
static uint8_t tpl[262144];

static int cmd_enroll(const char*user){
  const int NEED=12;
  if(sensor_open()||algo_open())return 2;
  printf("Регистрация пальца для '%s'. Приложите палец к кнопке питания (не нажимая), %d раз, чуть смещая.\n",user,NEED);
  int accepted=0;
  for(int cap=0; cap<40 && accepted<NEED; cap++){
    unsigned char buf[FRAME];
    if(wait_and_grab(buf,30000)){printf("таймаут ожидания пальца\n");break;}
    memcpy(g_frame,buf,FRAME);
    int er=fn_enroll(FID,(uint16_t)accepted,g_frame);
    if(er==0){accepted++;printf("  [%2d/%d] принято\n",accepted,NEED);}
    else printf("  кадр отклонён (er=%d) — приложите чуть иначе\n",er);
    wait_release();
  }
  if(accepted<3){printf("недостаточно кадров (%d) — регистрация не удалась\n",accepted);sensor_close();return 2;}
  uint8_t type=0; uint32_t size=0;
  int sr=fn_save(FID,&type,&size,tpl);
  if(sr!=0){printf("SaveAlgTplData=%d — не удалось\n",sr);sensor_close();return 2;}
  if(tpl_write(user,type,size,tpl)){sensor_close();return 2;}
  char p[512];tpl_path(user,p,sizeof p);
  printf("Готово: шаблон (%u байт) сохранён в %s. Принято кадров: %d.\n",size,p,accepted);
  sensor_close();
  return 0;
}

static int do_match(void){ /* g_frame уже заполнен; 0=совпало */
  uint8_t fid=0xff,upd=0; int ir=fn_identify(g_frame,1,&fid,&upd);
  uint32_t mfid=fn_match_fid(); uint32_t sc=fn_match_score();
  if(getenv("FTLOG"))fprintf(stderr,"identify ret=%d fid=%u score=%u\n",ir,mfid,sc);
  return (ir==0 && mfid==FID)?0:1;
}

static int cmd_verify(const char*user){
  uint8_t type=0; uint32_t size=0;
  if(tpl_read(user,&type,&size,tpl,sizeof tpl)){fprintf(stderr,"нет шаблона для '%s'\n",user);return 2;}
  if(algo_open())return 2;
  int ir0=fn_init_tpl(FID,type,size,tpl);
  if(ir0!=0){fprintf(stderr,"InitAlgTplData=%d\n",ir0);return 2;}
  if(sensor_open())return 2;
  fprintf(stderr,"Приложите палец...\n");
  /* таймаут: короче в PAM-контексте (иначе пароль ждёт зря); env FT_TIMEOUT переопределяет (мс) */
  int timeout_ms = getenv("PAM_TYPE") ? 8000 : 15000;
  const char*to=getenv("FT_TIMEOUT"); if(to){int v=atoi(to); if(v>0)timeout_ms=v;}
  int rc=1, waited=0;
  for(;;){
    unsigned char buf[FRAME];
    if(finger()){
      usleep(60000);
      if(grab(buf)==FRAME && stdev(buf,FRAME)>=12.0){
        memcpy(g_frame,buf,FRAME);
        if(do_match()==0){rc=0;break;}
      }
    }
    usleep(50000); waited+=50; if(waited>=timeout_ms)break;
  }
  sensor_close();
  fprintf(stderr, rc==0?"совпадение\n":"нет совпадения\n");
  return rc;
}

static int cmd_verify_file(const char*user,const char*path){
  uint8_t type=0; uint32_t size=0;
  if(tpl_read(user,&type,&size,tpl,sizeof tpl)){fprintf(stderr,"нет шаблона для '%s'\n",user);return 2;}
  if(algo_open())return 2;
  if(fn_init_tpl(FID,type,size,tpl)!=0){fprintf(stderr,"InitAlgTplData fail\n");return 2;}
  FILE*f=fopen(path,"rb"); if(!f){fprintf(stderr,"open %s\n",path);return 2;}
  if(fread(g_frame,1,FRAME,f)!=FRAME){fclose(f);return 2;} fclose(f);
  int rc=do_match();
  printf("%s: %s\n",path, rc==0?"MATCH":"no match");
  return rc;
}

int main(int argc,char**argv){
  /* пользователь: argv[2], иначе PAM_USER (pam_exec) */
  const char*pu=getenv("PAM_USER");
  const char*user=(argc>=3)?argv[2]:pu;
  if(argc>=2 && !strcmp(argv[1],"enroll") && user)  return cmd_enroll(user);
  if(argc>=2 && !strcmp(argv[1],"verify") && user)  return cmd_verify(user);
  if(argc>=4 && !strcmp(argv[1],"verify-file"))     return cmd_verify_file(argv[2],argv[3]);
  fprintf(stderr,"usage: %s enroll <user> | verify <user> | verify-file <user> <frame.raw>\n",argv[0]);
  return 2;
}
