
void Weap_BreakSound() {}
void Weap_Broke() {}
void Weap_Effect() {}
void Weap_PlaySound() {}
void Text_Begin(){}
void Text_End(){}
void Render_Model(){}
void Weap_VehicleShot(){}
void Weap_SurfaceSound() {}
int Weap_EntSurface() { return 0; }
void Weap_BulletImpact() {}

int stubSparks, stubExplosions, stubSplashes, stubMuzzles, stubDust;
void Weap_Sparks() { stubSparks++; }
void Weap_Dust() { stubDust++; }
void Weap_Splash() { stubSplashes++; }
void Weap_Explosion() { stubExplosions++; }
void Weap_Muzzle() { stubMuzzles++; }
float Weap_PlayerHealth(void) { return 100; }
void Weap_ItemImpulse(int e, const float *v) { (void)e; (void)v; }
int Weap_DropItem(const char *c, const float *p, const float *v) { (void)c; (void)p; (void)v; return 1; }
void Npc_HearCombat_unused(void) {}
