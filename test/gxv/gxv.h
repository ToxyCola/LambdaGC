#ifndef GXV_H
#define GXV_H
typedef struct {
	unsigned long draws, vertices, farVerts, errors, strayBytes, overflow;
	float maxDist;
} GxvStats;
extern GxvStats gxv;
extern const char *gxvTag;
void gxv_end_frame(void);
#endif
