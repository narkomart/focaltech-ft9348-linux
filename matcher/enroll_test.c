/* Enroll → Identify на проприетарном матчере FocalTech.
 * init алгоритма → focal_EnrollByImage(fid=1, idx, кадр) по всем enroll-кадрам →
 * focal_IdentifyByImage(кадр) → GetMatchFingerId/GetMatchScores.
 * Сборка: gcc -O2 -o enroll_test enroll_test.c -ldl
 * Запуск:  LD_PRELOAD=.../focaltech-shim.so ./enroll_test enroll1.raw enroll2.raw ... [-- identifyFrame.raw]
 *   (без аргументов берёт ~/focaltech-fp/cap/cap*.raw для enroll и все их + baseline для identify)
 * Offsets — строго из nm.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#define SO "/opt/focaltech/matcher.so"
#define OFF_ff_algo_do_config    0x43c20
#define OFF_InitFuncLog          0x6e300
#define OFF_EnrollByImage        0x75a70
#define OFF_IdentifyByImage      0x8d190
#define OFF_GetMatchFingerId     0x8af40
#define OFF_GetMatchScores       0x8aee0
#define OFF_SaveAlgTplData       0x794b0
#define OFF_InitAlgTplData       0x6e110
#define OFF_gEnrollInfo          0x30ef2b0
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

#define W 64
#define H 80
static uint8_t g_frame[W*H];
static int  cb_get_image (uint8_t *b,uint32_t n){memcpy(b,g_frame,n<W*H?n:W*H);return 0;}
static int  cb_spi_rw    (uint8_t*a,uint8_t*b,uint32_t l){(void)a;(void)b;(void)l;return 0;}
static void cb_usleep    (uint32_t us){struct timespec t={us/1000000,(long)(us%1000000)*1000};nanosleep(&t,0);}
static uint32_t cb_systime(void){static uint32_t t;return t+=10;}
static int  cb_ret0      (void){return 0;}
static void ftlog(const char*f,...){va_list ap;va_start(ap,f);printf("[ALG] ");vprintf(f,ap);printf("\n");va_end(ap);}

static unsigned long base;
static int load(const char*p){FILE*f=fopen(p,"rb");if(!f)return -1;size_t n=fread(g_frame,1,W*H,f);fclose(f);return n==W*H?0:-1;}

int main(int argc,char**argv){
  setvbuf(stdout,NULL,_IONBF,0);
  void*h=dlopen(SO,RTLD_NOW|RTLD_GLOBAL);
  if(!h){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
  struct link_map*lm=0;dlinfo(h,RTLD_DI_LINKMAP,&lm);base=lm->l_addr;
  printf("base=0x%lx\n",base);

  void (*init_log)(void*)=(void*)(base+OFF_InitFuncLog);
  int  (*algo_config)(void)=(void*)(base+OFF_ff_algo_do_config);
  int  (*enroll)(uint16_t,uint16_t,uint8_t*)=(void*)(base+OFF_EnrollByImage);
  int  (*identify)(uint8_t*,uint8_t,uint8_t*,uint8_t*)=(void*)(base+OFF_IdentifyByImage);
  int  (*save_tpl)(uint16_t,uint8_t*,uint32_t*,uint8_t*)=(void*)(base+OFF_SaveAlgTplData);
  int  (*init_tpl)(uint16_t,uint8_t,uint32_t,uint8_t*)=(void*)(base+OFF_InitAlgTplData);
  uint32_t (*match_fid)(void)=(void*)(base+OFF_GetMatchFingerId);
  uint32_t (*match_score)(void)=(void*)(base+OFF_GetMatchScores);
  uint8_t *gEnroll=(uint8_t*)(base+OFF_gEnrollInfo);

  #define REG(off,fn) ((void(*)(void*))(base+off))((void*)(fn))
  init_log((void*)ftlog);
  REG(OFF_InitFuncGetImage,cb_get_image); REG(OFF_InitFuncSpiRead,cb_spi_rw); REG(OFF_InitFuncSpiWrite,cb_spi_rw);
  REG(OFF_InitFuncUsleep,cb_usleep); REG(OFF_InitFuncGetSysTime,cb_systime); REG(OFF_InitFuncGetMcuStatus,cb_ret0);
  REG(OFF_InitFuncReadImgInit,cb_ret0); REG(OFF_InitFuncReadImgStart,cb_ret0); REG(OFF_InitFuncReadImgEnd,cb_ret0);
  REG(OFF_InitFuncWhilePoint,cb_ret0); REG(OFF_InitFuncGetImageBase,cb_ret0);

  static uint8_t gdev[224];
  *(uint16_t*)(gdev+210)=0x9362; *(int*)(gdev+212)=W; *(int*)(gdev+216)=H; *(void**)(base+0x211400)=gdev;

  int r=algo_config(); printf("ff_algo_do_config=%d\n\n",r); if(r){fprintf(stderr,"config fail\n");return 2;}

  /* включить подробный лог АЛГОРИТМА ПОСЛЕ config: config перезатёр focal_fp_log своим __log_printf — вернуть ftlog */
  if(getenv("FTLOG")){ init_log((void*)ftlog); *(uint8_t*)(base+0x30c4c19)=2; *(int*)(base+0x2113f8)=0; *(int*)(base+0x2113fc)=0; }

  /* собрать список enroll-кадров: argv до "--", иначе cap/cap00..cap19 */
  char *enroll_paths[64]; int ne=0; char *ident_paths[64]; int ni=0; int after=0;
  for(int i=1;i<argc;i++){
    if(!strcmp(argv[i],"--")){after=1;continue;}
    if(after){if(ni<64)ident_paths[ni++]=argv[i];} else {if(ne<64)enroll_paths[ne++]=argv[i];}
  }
  static char autobuf[64][256];
  if(ne==0){ for(int i=0;i<20;i++){snprintf(autobuf[i],256,"./cap/cap%02d.raw",i);
      FILE*t=fopen(autobuf[i],"rb"); if(t){fclose(t); enroll_paths[ne++]=autobuf[i];}} }
  if(ni==0){ for(int i=0;i<ne;i++)ident_paths[ni++]=enroll_paths[i];
             ident_paths[ni++]="./baseline.raw"; }

  static uint8_t tpl[262144]; uint8_t tpl_type=0; uint32_t tpl_size=0;
  const char *ftload=getenv("FTLOAD"), *ftsave=getenv("FTSAVE");

  if(ftload){
    /* режим verify-only: загрузить шаблон из файла (эмуляция отдельного процесса fprintd verify) */
    FILE*f=fopen(ftload,"rb"); if(!f){fprintf(stderr,"FTLOAD open fail\n");return 4;}
    if(fread(&tpl_type,1,1,f)){} if(fread(&tpl_size,4,1,f)){} size_t rd=fread(tpl,1,tpl_size,f); fclose(f);
    printf("LOADED tpl from %s: type=%u size=%u (read %zu)\n",ftload,tpl_type,tpl_size,rd);
    int ir0=init_tpl(1,tpl_type,tpl_size,tpl); printf("InitAlgTplData(load)=%d\n\n",ir0);
  } else {
    printf("=== ENROLL (fid=1), %d кадров ===\n",ne);
    for(int i=0;i<ne;i++){
      if(load(enroll_paths[i])){printf("  [%d] %s: load fail\n",i,enroll_paths[i]);continue;}
      int er=enroll(1,(uint16_t)i,g_frame);
      printf("  [idx %2d] %-40s EnrollByImage=%d | gEnrollInfo: ",i,enroll_paths[i],er);
      for(int k=0;k<24;k++)printf("%02x",gEnroll[k]); printf("\n");
    }
    /* сериализовать enrolled-шаблон fid=1 */
    int sr=save_tpl(1,&tpl_type,&tpl_size,tpl);
    printf("SaveAlgTplData=%d  tpl_type=%u tpl_size=%u\n",sr,tpl_type,tpl_size);
    if(sr==0 && ftsave){ FILE*f=fopen(ftsave,"wb");
      if(f){fwrite(&tpl_type,1,1,f);fwrite(&tpl_size,4,1,f);fwrite(tpl,1,tpl_size,f);fclose(f);
            printf("SAVED tpl -> %s\n",ftsave);} }
    if(sr==0){ int ir0=init_tpl(1,tpl_type,tpl_size,tpl); printf("InitAlgTplData(same-proc load)=%d\n",ir0); }
    printf("\n");
  }

  printf("=== IDENTIFY, %d кадров (fid должен=1 для своего пальца, mismatch для baseline) ===\n",ni);
  for(int i=0;i<ni;i++){
    if(load(ident_paths[i])){printf("  %s: load fail\n",ident_paths[i]);continue;}
    uint8_t outFid=0xff, outUpd=0xff;
    int ir=identify(g_frame,1,&outFid,&outUpd);
    uint32_t mfid=match_fid(), msc=match_score();
    printf("  %-40s Identify=%d outFid=%d upd=%d | MatchFingerId=%u Score=%u\n",
           ident_paths[i],ir,outFid,outUpd,mfid,msc);
  }
  return 0;
}
