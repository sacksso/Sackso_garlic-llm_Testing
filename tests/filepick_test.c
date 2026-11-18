#include <stdio.h>
#include <string.h>
#include "filepick.h"
static int bad;
#define CHECK(c) do{if(!(c)){printf("FAIL %d: %s\n",__LINE__,#c);bad++;}}while(0)
static void show(FilePick*f){for(int i=0;i<f->n;i++)printf("  [%c]%s%s%s\n",fp_is_picked(f,i)?'x':' ',f->ent[i].name,f->ent[i].is_dir?"/":"",f->ent[i].is_zip?" (zip)":"");}
static int find(FilePick*f,const char*n){for(int i=0;i<f->n;i++)if(!strcmp(f->ent[i].name,n))return i;return -1;}
int main(void){
 static FilePick f; fp_open(&f); show(&f);
 CHECK(f.n==5);
 CHECK(f.ent[0].is_dir);   // out first
 fp_move(&f,find(&f,"notas.txt")); fp_enter(&f); CHECK(f.npicked==1);
 fp_move(&f,find(&f,"con espacio.txt")-f.cur); fp_toggle(&f); CHECK(f.npicked==1&&f.msg[0]);
 fp_move(&f,find(&f,"out")-f.cur); fp_enter(&f); fp_enter(&f); // into out, into chat
 CHECK(!strcmp(f.dir,"out/chat")); fp_enter(&f); CHECK(f.npicked==2);
 fp_up(&f); fp_up(&f); CHECK(f.dir[0]==0);
 fp_move(&f,find(&f,"repo.zip")-f.cur); fp_enter(&f); show(&f);
 CHECK(!strcmp(f.zip,"repo.zip")&&f.n==2);
 int c=find(&f,"chat"); fp_move(&f,c-f.cur); fp_enter(&f); show(&f); CHECK(f.n==1);
 fp_enter(&f); CHECK(f.npicked==3);
 char r[400]; fp_result(&f,r,sizeof r); printf("result: %s\n",r);
 CHECK(!strcmp(r,"@notas.txt @out/chat/x.c @repo.zip:chat/main.c "));
 fp_up(&f); CHECK(f.zdir[strlen(f.zdir)-1]=='/'&&f.n==2&&f.ent[f.cur].is_dir);
 fp_up(&f); CHECK(!f.zip[0]&&!strcmp(f.ent[f.cur].name,"repo.zip"));
 CHECK(fp_hit(&f,FP_X+400,FP_LIST_Y+5)==f.top);
 CHECK(fp_hit(&f,FP_X+20,FP_LIST_Y+5)==FP_HIT_CHECK+f.top);
 CHECK(fp_hit(&f,FP_DONE_X+5,FP_BTN_Y+5)==FP_HIT_DONE&&fp_hit(&f,FP_UP_X+5,FP_BTN_Y+5)==FP_HIT_UP&&fp_hit(&f,FP_CANCEL_X+5,FP_BTN_Y+5)==FP_HIT_CANCEL);
 CHECK(fp_hit(&f,5,5)==FP_HIT_NONE);
 fp_click(&f,FP_HIT_CHECK+find(&f,"repo.zip")); CHECK(fp_is_picked(&f,find(&f,"repo.zip")));
 printf(bad?"FAILED\n":"filepick ok\n");return bad;}
