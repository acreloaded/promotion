// processing of server events

void processevent(client *c, explodeevent &e)
{
    clientstate &gs = c->state;
    switch(e.gun)
    {
        case GUN_GRENADE:
            if(!gs.grenades.remove(e.id)) return;
            break;

        default:
            return;
    }
    for(int i = 1; i<c->events.length() && c->events[i].type==GE_HIT; i++)
    {
        hitevent &h = c->events[i].hit;
        if(!clients.inrange(h.target)) continue;
        client *target = clients[h.target];
        if(target->type==ST_EMPTY || target->state.state!=CS_ALIVE || h.lifesequence!=target->state.lifesequence || h.dist<0 || h.dist>EXPDAMRAD) continue;

        int j = 1;
        for(j = 1; j<i; j++) if(c->events[j].hit.target==h.target) break;
        if(j<i) continue;

        // [ACP] Nade damage fades with square root, not linear
        int damage = int(guns[e.gun].damage*sqrt(1-h.dist/(float)EXPDAMRAD));
        // [/ACP]
        bool chk_gun = e.gun==GUN_GRENADE;
        bool chk_dir = h.dir[0]+h.dir[1]+h.dir[2]==0;
        bool chk_dst = h.dist < 2.0f;
        bool chk_cnr = c->clientnum == target->clientnum;
        if(chk_gun && chk_dir && chk_dst && chk_cnr) damage = INT_MAX; // nade suicide
        serverdamage(target, c, damage, e.gun, true, h.dir);
    }
}

// [ACP] 'intersect sphere' from weapon.cpp for headshots!
static inline bool intersectsphere(const vec &from, const vec &to, vec center, float radius, float &dist)
{
    vec ray(to);
    ray.sub(from);
    center.sub(from);
    float v = center.dot(ray),
          inside = radius*radius - center.squaredlen();
    if(inside < 0 && v < 0) return false;
    float raysq = ray.squaredlen(), d = inside*raysq + v*v;
    if(d < 0) return false;
    dist = (v - sqrtf(d)) / raysq;
    return dist >= 0 && dist <= 1;
}
// [/ACP]

// [ACP] Damage fading table
static int ranges[NUMGUNS] = {
      0, // GUN_KNIFE
     90, // GUN_PISTOL
    110, // GUN_CARBINE
     80, // GUN_SHOTGUN
    100, // GUN_SUBGUN
    150, // GUN_SNIPER
    120, // GUN_ASSAULT
     90, // GUN_CPISTOL
      0, // GUN_GRENADE
     90, // GUN_AKIMBO
};
// [/ACP]

void processevent(client *c, shotevent &e)
{
    clientstate &gs = c->state;
    int wait = e.millis - gs.lastshot;
    if(!gs.isalive(gamemillis) ||
       e.gun<GUN_KNIFE || e.gun>=NUMGUNS ||
       wait<gs.gunwait[e.gun] ||
       gs.mag[e.gun]<=0)
        return;
    if(e.gun!=GUN_KNIFE) gs.mag[e.gun]--;
    loopi(NUMGUNS) if(gs.gunwait[i]) gs.gunwait[i] = max(gs.gunwait[i] - (e.millis-gs.lastshot), 0);
    gs.lastshot = e.millis;
    gs.gunwait[e.gun] = attackdelay(e.gun);
    if(e.gun==GUN_PISTOL && gs.akimbomillis>gamemillis) gs.gunwait[e.gun] /= 2;
    sendf(-1, 1, "ri6x", SV_SHOTFX, c->clientnum, e.gun,
//         int(e.from[0]*DMF), int(e.from[1]*DMF), int(e.from[2]*DMF),
        int(e.to[0]*DMF), int(e.to[1]*DMF), int(e.to[2]*DMF),
        c->clientnum);
    gs.shotdamage += guns[e.gun].damage*(e.gun==GUN_SHOTGUN ? SGMAXDMGLOC : 1); // 2011jan17:ft: so accuracy stays correct, since SNIPER:headshot also "exceeds expectations" we use SGMAXDMGLOC instead of SGMAXDMGABS!
    switch(e.gun)
    {
        case GUN_GRENADE: gs.grenades.add(e.id); break;
        default:
        {
            int totalrays = 0, maxrays = e.gun==GUN_SHOTGUN ? 3*SGRAYS: 1;
            int tothits_c = 0, tothits_m = 0, tothits_o = 0; // sgrays
            for(int i = 1; i<c->events.length() && c->events[i].type==GE_HIT; i++)
            {
                hitevent &h = c->events[i].hit;
                if(!clients.inrange(h.target)) continue;
                client *target = clients[h.target];
                if(target->type==ST_EMPTY || target->state.state!=CS_ALIVE || h.lifesequence!=target->state.lifesequence) continue;

                int rays = 1, damage = 0;
                bool gib = false;
                if(e.gun == GUN_SHOTGUN)
                {
                    h.info = isbigendian() ? endianswap(h.info) : h.info;
                    int bonusdist = h.info&0xFF;
                    int numhits_c = (h.info & 0x0000FF00) >> 8, numhits_m = (h.info & 0x00FF0000) >> 16, numhits_o = (h.info & 0xFF000000) >> 24;
                    tothits_c += numhits_c; tothits_m += numhits_m; tothits_o += numhits_o;
                    rays = numhits_c + numhits_m + numhits_o;
                    
                    if(rays < 1 || tothits_c > SGRAYS || tothits_m > SGRAYS || tothits_o > SGRAYS || bonusdist > SGDMGBONUS) continue;

                    gib = rays == maxrays;
                    float fdamage = (SGDMGTOTAL/(21*100.0f)) * (numhits_o * SGCOdmg/10.0f + numhits_m * SGCMdmg/10.0f + numhits_c * SGCCdmg/10.0f);
                    fdamage += (float)bonusdist;
                    damage = (int)ceil(fdamage);
#ifdef ACAC
                    if (!sg_engine(target, c, numhits_c, numhits_m, numhits_o, bonusdist)) continue;
#endif
                }
                else
                {
                    damage = rays*guns[e.gun].damage;
                    // [ACP] New shot damage: headshots with ALL weapons + damage fading
                    gib = e.gun == GUN_KNIFE;
                    if(gib)
                       // knife is now always a 1-hit gib kill
                       damage *= 5;
                    // Trace an estimated head spot
                    #define PLAYERHEIGHT 4.5f
                    vec virtualhead = vec(.2f, -.25f, .25f + PLAYERHEIGHT);
                    virtualhead.rotate_around_z(target->y * RAD);
                    virtualhead.add(target->state.o);
                    // [ACP] Extra sniper/carbine power!
                    if(e.gun == GUN_SNIPER || e.gun == GUN_CARBINE)
                        damage *= 5;
                    // [/ACP]
                    // Extend the line segment by ~2 meters (8 cubes)
                    float dist = vec(e.from).dist(e.to);
                    vec newto = e.to;
                    newto.sub(e.from).normalize().mul(dist + 8).add(e.from);
                    float dist2;
                    // The actual radius is 0.4, but compensate for lag by having a bigger hitbox
                    if(intersectsphere(e.from, newto, virtualhead, 1.2f, dist2))
                    {
                        gib = true;
                        damage *= 5;
                        e.gun = GUN_SNIPER; // as sniper to get the headshot sound
                        dist = (dist + 8) * dist2;
                    }
                    // Distance penalty (damage fading)
                    const int range = ranges[e.gun % NUMGUNS];
                    // Cut-off is at half damage
                    if(dist >= (range*3)>>2) damage /= 2;
                    else damage *= sqrtf(1.f - dist/range);
                    // [/ACP]
                }
                totalrays += rays;

                if(totalrays>maxrays) continue;
                serverdamage(target, c, damage, e.gun, gib, h.dir);
            }
            // [ACP] Explosive ammo
            static uchar buf[MAXTRANS];
            ucharbuf p(buf, MAXTRANS);
            putint(p, SV_THROWNADE);
            putint(p, int(e.to[0]*DMF));
            putint(p, int(e.to[1]*DMF));
            putint(p, int(e.to[2]*DMF));
            putint(p, 0);
            putint(p, 0);
            putint(p, 0);
            putint(p, 2000);
            if(numclients() >= 2)
            {
                int found = c->clientnum;
                loopv(clients)
                {
                    if(i != found && clients[i]->type != ST_EMPTY)
                    {
                        found = i;
                        break;
                    }
                }
                sendf(-1, 1, "ri3mx", SV_CLIENT, c->clientnum, p.length(), p.length(), p.buf, c->clientnum);
                sendf(c->clientnum, 1, "ri3m", SV_CLIENT, found, p.length(), p.length(), p.buf);
            }
            else
            {
                sendf(-1, 1, "ri3m", SV_CLIENT, c->clientnum, p.length(), p.length(), p.buf);
            }
            loopv(clients)
            {
                if(clients[i]->type == ST_EMPTY || clients[i]->state.state != CS_ALIVE)
                    continue;
                vec to(e.to);
                float dist = vec(clients[i]->state.o.x, clients[i]->state.o.y, clients[i]->state.o.z + PLAYERHEIGHT).dist(to);
                if(dist > 16)
                    continue;
                serverdamage(clients[i], c, (e.gun == GUN_SHOTGUN ? 95 : guns[e.gun].damage) * sqrt(1 - dist/16), GUN_SNIPER, true);
            }
            // [/ACP]
            break;
        }
    }
}

void processevent(client *c, suicideevent &e)
{
    // [ACP] Gibbing suicide
    serverdamage(c, c, INT_MAX, GUN_KNIFE, true);
    // [/ACP]
}

void processevent(client *c, pickupevent &e)
{
    clientstate &gs = c->state;
    if(m_mp(gamemode) && !gs.isalive(gamemillis)) return;
    serverpickup(e.ent, c->clientnum);
}

void processevent(client *c, reloadevent &e)
{
    clientstate &gs = c->state;
    if(!gs.isalive(gamemillis) ||
       e.gun<GUN_KNIFE || e.gun>=NUMGUNS ||
       !reloadable_gun(e.gun) ||
       gs.ammo[e.gun]<=0)
        return;

    bool akimbo = e.gun==GUN_PISTOL && gs.akimbomillis>e.millis;
    int mag = (akimbo ? 2 : 1) * magsize(e.gun), numbullets = min(gs.ammo[e.gun], mag - gs.mag[e.gun]);
    if(numbullets<=0) return;

    gs.mag[e.gun] += numbullets;
    gs.ammo[e.gun] -= numbullets;

    int wait = e.millis - gs.lastshot;
    sendf(-1, 1, "ri3", SV_RELOAD, c->clientnum, e.gun);
    if(gs.gunwait[e.gun] && wait<gs.gunwait[e.gun]) gs.gunwait[e.gun] += reloadtime(e.gun);
    else
    {
        loopi(NUMGUNS) if(gs.gunwait[i]) gs.gunwait[i] = max(gs.gunwait[i] - (e.millis-gs.lastshot), 0);
        gs.lastshot = e.millis;
        gs.gunwait[e.gun] += reloadtime(e.gun);
    }
}

void processevent(client *c, akimboevent &e)
{
    clientstate &gs = c->state;
    if(!gs.isalive(gamemillis) || gs.akimbomillis) return;
    gs.akimbomillis = e.millis+30000;
}

void clearevent(client *c)
{
    int n = 1;
    while(n<c->events.length() && c->events[n].type==GE_HIT) n++;
    c->events.remove(0, n);
}

void processevents()
{
    loopv(clients)
    {
        client *c = clients[i];
        if(c->type==ST_EMPTY) continue;
        if(c->state.akimbomillis && c->state.akimbomillis < gamemillis) { c->state.akimbomillis = 0; c->state.akimbo = false; }
        while(c->events.length())
        {
            gameevent &e = c->events[0];
            if(e.type<GE_SUICIDE)
            {
                if(e.shot.millis>gamemillis) break;
                if(e.shot.millis<c->lastevent) { clearevent(c); continue; }
                c->lastevent = e.shot.millis;
            }
            switch(e.type)
            {
                case GE_SHOT: processevent(c, e.shot); break;
                case GE_EXPLODE: processevent(c, e.explode); break;
                case GE_AKIMBO: processevent(c, e.akimbo); break;
                case GE_RELOAD: processevent(c, e.reload); break;
                // untimed events
                case GE_SUICIDE: processevent(c, e.suicide); break;
                case GE_PICKUP: processevent(c, e.pickup); break;
            }
            clearevent(c);
        }
        // [ACP] COD-like forced spawns
        if(c->state.state != CS_ALIVE)
        {
            extern int canspawn(client *c);
            int sp = canspawn(c);
            if(team_isspect(c->team) && sp < SP_OK_NUM)
            {
                updateclientteam(c->clientnum, TEAM_ANYACTIVE, FTR_PLAYERWISH);
                sp = canspawn(c);
            }
            if( sp < SP_OK_NUM && gamemillis > c->state.lastspawn + 1000 && gamemillis > c->state.lastdeath + 1500 ) sendspawn(c);
        }
        // [/ACP]
    }
}

