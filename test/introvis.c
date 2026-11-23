#define main sim_main
#include "sim.c"
#undef main
#include "world.h"

int main(int argc,char**argv){
 if(load(&P,argv[1]))return 1;
 World w; World_Init(&w,&P);
 f32 pts[][4]={{-11756,-10717,-1271,-122},{-12038,-10661,-1271,-83},{-12400,-11062,-1271,-3},{-10408,-14384,-1154,123},{-10488,-14011,-1272,108},{-6943,-6017,86,0},{-15816,-14296,-1133,180}};
 for(int i=0;i<7;i++){Camera c={0};c.origin=(guVector){pts[i][0],pts[i][1],pts[i][2]};c.yaw=pts[i][3];c.fovx=75;c.fovy=60;Camera_Update(&c);
  World_Collect(&w,&c,0); printf("%d: leaf %d cluster %d visible faces %u leafs %u\n",i,w.leaf,w.cluster,w.numVisible,w.leafsDrawn);}
}
