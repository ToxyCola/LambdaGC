#define main sim_main
#include "sim.c"
#undef main
int main(int argc,char**argv){ load(&P,argv[1]); for(int i=2;i<argc;i++){int m=atoi(argv[i]); printf("mat %d tex %d tex2 %d kind %d rgb %d %d %d\n",m,P.mats[m].tex,P.mats[m].tex2,P.mats[m].kind,P.mats[m].r,P.mats[m].g,P.mats[m].b);} }
