// mo-pilot: in-process helper injected into gamemd.exe (Mental Omega / YR 1.001)
// Exposes a tiny line-based named-pipe protocol so an external agent can read
// the unit roster and issue orders WITHOUT screenshots or mouse input.
#include <GeneralStructures.h>
#include <YRPP.h>

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

volatile unsigned long g_catchAddr = 0;
volatile unsigned long g_catchCode = 0;

#define MO_LOG  "mo-pilot.log"

// pipe name is derived from the dll file name so several builds can live in the
// same process: mo_pilot.dll -> \\.\pipe\mo-pilot, mo_pilot_v3.dll -> \\.\pipe\mo-pilot-v3
static wchar_t g_pipe[160];
static const wchar_t* PipeName()
{
    if (g_pipe[0]) return g_pipe;

    char path[MAX_PATH] = {0};
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&PipeName, &hm) && hm)
        GetModuleFileNameA(hm, path, MAX_PATH);

    char* base = path;
    for (char* q = path; *q; ++q) if (*q == '\\' || *q == '/') base = q + 1;

    char stem[96] = "mo-pilot";
    if (*base) {
        _snprintf_s(stem, sizeof(stem), _TRUNCATE, "%s", base);
        char* dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        for (char* q = stem; *q; ++q) if (*q == '_') *q = '-';
    }
    _snwprintf_s(g_pipe, 160, _TRUNCATE, L"\\\\.\\pipe\\%hs", stem);
    return g_pipe;
}

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    HANDLE h = CreateFileA(MO_LOG, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w = 0; DWORD len = (DWORD)strlen(buf);
    WriteFile(h, buf, len, &w, nullptr);
    WriteFile(h, "\r\n", 2, &w, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------- game access

static HouseClass* Me() { return HouseClass::Player; }

static DynamicVectorClass<TechnoClass*>* Units() { return TechnoClass::Array; }

static bool Ready()
{
    auto me = Me();
    if (!me) return false;
    auto arr = Units();
    if (!arr) return false;
    return arr->Count > 0;
}

// money lives at HouseClass + 0x30C (validated by the user's older cheat DLL)
static int Money() { return *(int*)((char*)Me() + 0x30C); }

static int Frame() { return (int)Unsorted::CurrentFrame; }

static int MyCount()
{
    auto me = Me(); auto arr = Units(); int n = 0;
    for (int i = 0; i < arr->Count; ++i) {
        TechnoClass* p = arr->GetItem(i);
        if (p && p->Owner == me) ++n;
    }
    return n;
}

static const char* IdOf(TechnoClass* p)
{
    if (!p) return "?";
    auto id = p->get_ID();
    return id ? id : "?";
}

// selection: "all" | "g:N" | "i:1,2,3"
static int Collect(const char* sel, TechnoClass** out, int cap)
{
    auto me = Me(); auto arr = Units();
    int n = 0;
    if (!sel || !*sel) return 0;

    if (_stricmp(sel, "all") == 0) {
        for (int i = 0; i < arr->Count && n < cap; ++i) {
            TechnoClass* p = arr->GetItem(i);
            if (p && p->Owner == me && p->IsHumanControlled) out[n++] = p;
        }
        return n;
    }
    if (sel[0] == 'g' && sel[1] == ':') {
        int g = atoi(sel + 2);
        for (int i = 0; i < arr->Count && n < cap; ++i) {
            TechnoClass* p = arr->GetItem(i);
            if (p && p->Owner == me && p->Group == g) out[n++] = p;
        }
        return n;
    }
    if (sel[0] == 'i' && sel[1] == ':') {
        char tmp[512]; strncpy_s(tmp, sel + 2, sizeof(tmp) - 1);
        char* ctx = nullptr;
        for (char* tok = strtok_s(tmp, ",", &ctx); tok && n < cap; tok = strtok_s(nullptr, ",", &ctx)) {
            int idx = atoi(tok);
            if (idx >= 0 && idx < arr->Count) {
                TechnoClass* p = arr->GetItem(idx);
                if (p && p->Owner == me) out[n++] = p;
            }
        }
        return n;
    }
    return 0;
}

// ---------------------------------------------------------------- production

static const char* KindOf(AbstractType t)
{
    switch (t) {
    case AbstractType::Building: return "B";
    case AbstractType::Unit:     return "U";
    case AbstractType::Infantry: return "I";
    case AbstractType::Aircraft: return "A";
    default: return "?";
    }
}

static void CmdCanBuild(char* out, size_t cap, char* body, size_t bodyCap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 n=0"); return; }
    auto me = Me();
    size_t used = 0; int n = 0; body[0] = 0;

    auto add = [&](TechnoTypeClass* t) {
        if (!t) return;
        if (me->CanBuild(t, false, true) != CanBuildResult::Buildable) return;
        auto id = t->get_ID();
        int w = _snprintf_s(body + used, bodyCap - used, _TRUNCATE, "%s|%s|%d\n",
                            id ? id : "?", KindOf(t->WhatAmI()), t->GetCost());
        if (w <= 0) return;
        used += (size_t)w; ++n;
    };

    for (int i = 0; i < BuildingTypeClass::Array->Count; ++i) add(BuildingTypeClass::Array->GetItem(i));
    for (int i = 0; i < UnitTypeClass::Array->Count; ++i) add(UnitTypeClass::Array->GetItem(i));
    for (int i = 0; i < InfantryTypeClass::Array->Count; ++i) add(InfantryTypeClass::Array->GetItem(i));
    for (int i = 0; i < AircraftTypeClass::Array->Count; ++i) add(AircraftTypeClass::Array->GetItem(i));

    _snprintf_s(out, cap, _TRUNCATE, "ok ready=1 n=%d\n%s", n, body);
}

// Find the factory able to produce `type`. First ask the game for the house's
// primary factory, then fall back to scanning our own buildings (each factory
// plant holds its own FactoryClass pointer at BuildingClass::Factory).
static FactoryClass* FindFactoryFor(TechnoTypeClass* type)
{
    auto me = Me();
    int cat = -1;
    if (type->WhatAmI() == AbstractType::BuildingType)
        cat = (int)static_cast<BuildingTypeClass*>(type)->BuildCat;

    FactoryClass* f = nullptr;
    if (cat >= 0)
        f = me->GetPrimaryFactory(type->WhatAmI(), false, (BuildCat)cat);
    if (!f)
        f = me->GetPrimaryFactory(type->WhatAmI(), false, BuildCat::DontCare);
    if (f) return f;

    for (int i = 0; i < me->Buildings.Count; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (!b || !b->Type || !b->Factory) continue;
        if (b->Type->Factory != type->WhatAmI()) continue;
        if (cat >= 0 && (int)b->Type->BuildCat != cat && (int)b->Type->BuildCat != 0) continue;
        return b->Factory;
    }
    return nullptr;
}

// Pump our buildings' Update() a few times to see whether production advances
// when the frame logic is driven by hand (our factories are not ticked by the game).
static void CmdPump(char* out, size_t cap, int times)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    if (times < 1) times = 1;
    if (times > 2000) times = 2000;

    int n = 0, err = 0;
    for (int t = 0; t < times; ++t) {
        for (int i = 0; i < me->Buildings.Count; ++i) {
            BuildingClass* b = me->Buildings.GetItem(i);
            if (!b) continue;
            __try { b->Update(); } __except (EXCEPTION_EXECUTE_HANDLER) { ++err; }
            ++n;
        }
    }

    int pv = -1, step = -1;
    for (int i = 0; i < me->Buildings.Count; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (b && b->Factory) { pv = (int)b->Factory->Production.Value; step = (int)b->Factory->Production.Step; break; }
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok calls=%d err=%d pv=%d ps=%d", n, err, pv, step);
}

// Tick the FactoryClass objects themselves (FactoryClass::Update) instead of the
// buildings: production progress is advanced by the factory's own Update.
static void CmdTickF(char* out, size_t cap, int times)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    if (times < 1) times = 1;
    if (times > 20000) times = 20000;

    int money0 = *(int*)((char*)me + 0x30C);
    FactoryClass* f = nullptr;
    for (int i = 0; i < me->Buildings.Count && !f; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (b && b->Factory) f = b->Factory;
    }
    if (!f) { _snprintf_s(out, cap, _TRUNCATE, "ok nofactory"); return; }

    int err = 0;
    for (int t = 0; t < times; ++t) {
        __try { f->Update(); } __except (EXCEPTION_EXECUTE_HANDLER) { ++err; break; }
    }
    int money1 = *(int*)((char*)me + 0x30C);
    _snprintf_s(out, cap, _TRUNCATE, "ok ticks=%d err=%d pv=%d ps=%d money=%d->%d obj=%p",
                times, err, (int)f->Production.Value, (int)f->Production.Step, money0, money1,
                (void*)f->Object);
}

// FactoryClass::StartProduction() 閳?the step that actually pays for the item and
// puts it into production. DemandProduction alone only queues it.
static void CmdStartProd(char* out, size_t cap, const char* id)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    auto type = TechnoTypeClass::Find(id);
    if (!type) { _snprintf_s(out, cap, _TRUNCATE, "err badtype %s", id); return; }
    TechnoTypeClass* t = type;

    FactoryClass* f = nullptr;
    for (int i = 0; i < me->Buildings.Count && !f; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (b && b->Factory) f = b->Factory;
    }
    if (!f) { _snprintf_s(out, cap, _TRUNCATE, "ok nofactory"); return; }

    int money0 = *(int*)((char*)me + 0x30C);
    int err = 0, demand = -1, started = 0;
    __try { demand = f->DemandProduction(t, me, false) ? 1 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { err = 1; }
    if (!err) {
        __try { f->StartProduction(); started = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { err = 2; }
    }
    int money1 = *(int*)((char*)me + 0x30C);
    _snprintf_s(out, cap, _TRUNCATE, "ok demand=%d started=%d err=%d money=%d->%d pv=%d bal=%d obj=%p",
                demand, started, err, money0, money1, (int)f->Production.Value,
                f->Balance, (void*)f->Object);
}

// FactoryClass::StartProduction() / CompletedProduction() driver
static void CmdFinishTick(char* out, size_t cap, int times)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    FactoryClass* f = nullptr;
    for (int i = 0; i < me->Buildings.Count && !f; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (b && b->Factory) f = b->Factory;
    }
    if (!f) { _snprintf_s(out, cap, _TRUNCATE, "ok nofactory"); return; }

    int money0 = *(int*)((char*)me + 0x30C);
    int done = 0, err = 0;
    for (int t = 0; t < times; ++t) {
        __try {
            f->CompletedProduction();
            if (f->IsDone()) ++done;
        } __except (EXCEPTION_EXECUTE_HANDLER) { err = 1; break; }
    }
    int money1 = *(int*)((char*)me + 0x30C);
    _snprintf_s(out, cap, _TRUNCATE, "ok ticks=%d done=%d err=%d money=%d->%d pv=%d bal=%d obj=%p",
                times, done, err, money0, money1, (int)f->Production.Value,
                f->Balance, (void*)f->Object);
}

// Give a limbo (factory-produced, never unlimbo'd) unit a locomotor and put it on
// the map next to our base. Without a locomotor every SetDestination/mission AVs.
static void CmdUnlimbo(char* out, size_t cap, const char* sel)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();

    CoordStruct base = {0, 0, 0};
    if (me->Buildings.Count > 0) base = me->Buildings.GetItem(0)->GetCoords();

    TechnoClass* sel_list[1024];
    int nsel = Collect(sel, sel_list, 1024);
    int done = 0, skip = 0, err = 0, spawn = 0, stage = 0;
    for (int i = 0; i < nsel; ++i) {
        TechnoClass* t = sel_list[i];
        if (!t) continue;
        AbstractType kind = t->WhatAmI();
        if (kind != AbstractType::Unit && kind != AbstractType::Infantry &&
            kind != AbstractType::Aircraft) { ++skip; continue; }
        UnitClass* u = (UnitClass*)t;
        if (!u->InLimbo) { ++skip; continue; }

        CoordStruct c = u->GetCoords();
        if (c.X == 0 && c.Y == 0) {
            // keep clear of the yard's 4x4 foundation
            c.X = base.X + (2 + (spawn % 4)) * 256;
            c.Y = base.Y + (2 + (spawn / 4)) * 256;
            ++spawn;
        }

        // Each step gets its own guard: Put partially succeeds and then faults
        // inside the game, so we must keep going instead of aborting the whole unit.
        stage = 1;
        __try { u->Put(c, Direction::North); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++err; }

        stage = 2;
        __try { u->InLimbo = false; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++err; }

        stage = 3;
        __try {
            if (!((FootClass*)t)->Locomotor) {
                switch (kind) {
                    case AbstractType::Infantry:
                        LocomotionClass::ChangeLocomotorTo((FootClass*)t, LocomotionClass::CLSIDs::Walk);
                        break;
                    case AbstractType::Aircraft:
                        LocomotionClass::ChangeLocomotorTo((FootClass*)t, LocomotionClass::CLSIDs::Fly);
                        break;
                    default:
                        LocomotionClass::ChangeLocomotorTo((FootClass*)t, LocomotionClass::CLSIDs::Drive);
                        break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ++err; }

        ++done;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok unlimbo=%d skip=%d fail=%d stage=%d", done, skip, err, stage);
}

// Give the selected units a locomotor even when they are already on the map.
// Build a locomotor by hand for an object that never went through the normal
// unlimbo path. Kept free of C++ objects so __try is legal (C2712).
static int g_locoStage = 0;

static bool ForceLoco(FootClass* f, const CLSID* clsid)
{
    __try {
        g_locoStage = 1;
        ILocomotion* loco = nullptr;
        if (FAILED(LocomotionClass::CreateInstance(&loco, clsid, nullptr, CLSCTX_INPROC_SERVER)) || !loco)
            return false;
        g_locoStage = 2;
        if (FAILED(loco->Link_To_Object(f))) { loco->Release(); return false; }
        g_locoStage = 3;
        f->Locomotor.reset(loco);
        g_locoStage = 4;
        return (bool)f->Locomotor;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void CmdLoco(char* out, size_t cap, const char* sel)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    TechnoClass* list[1024];
    int n = Collect(sel, list, 1024);
    int ok = 0, manual = 0, err = 0;
    for (int i = 0; i < n; ++i) {
        TechnoClass* t = list[i];
        if (!t) continue;
        AbstractType kind = t->WhatAmI();
        FootClass* f = (FootClass*)t;
        if (f->Locomotor) { ++ok; continue; }

        const CLSID* clsid = (kind == AbstractType::Infantry) ? &LocomotionClass::CLSIDs::Walk
                           : (kind == AbstractType::Aircraft) ? &LocomotionClass::CLSIDs::Fly
                                                             : &LocomotionClass::CLSIDs::Drive;

        if (ForceLoco(f, clsid)) { ++ok; ++manual; }
        else ++err;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok loco=%d manual=%d err=%d stage=%d", ok, manual, err, g_locoStage);
}

// Ask the game itself to (re)register our buildings as primary factories: this is
// the same entry point the sidebar "primary" button uses and it creates the
// FactoryClass the game needs to tick production.
static void CmdPrimary(char* out, size_t cap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    int n = 0, ok = 0;
    for (int i = 0; i < me->Buildings.Count; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (!b) continue;
        __try { if (b->TogglePrimaryFactory()) ++ok; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        ++n;
    }
    _snprintf_s(out, cap, _TRUNCATE,
                "ok calls=%d toggled=%d factories=%d p_build=%p", n, ok,
                FactoryClass::Array->Count, (void*)me->Primary_ForBuildings);
}

// Where did our placed buildings actually go? Scan the global techno array.
static void CmdWhoami(char* out, size_t cap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    char buf[1400] = {0};
    size_t used = 0;
    int nb = 0;
    for (int i = 0; i < TechnoClass::Array->Count; ++i) {
        TechnoClass* t = TechnoClass::Array->GetItem(i);
        if (!t) continue;
        if (t->WhatAmI() != AbstractType::Building) continue;
        ++nb;
        const char* id = t->get_ID();
        int mine = (t->Owner == me) ? 1 : 0;
        used += _snprintf_s(buf + used, sizeof(buf) - used, _TRUNCATE, " %s(mine=%d)", id ? id : "?", mine);
        if (used > sizeof(buf) - 64) break;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok techno_buildings=%d house_buildings=%d:%s",
                nb, me->Buildings.Count, buf);
}

// Force any factory that already holds a finished object to hand it to the world.
static void CmdFinish(char* out, size_t cap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    int n = 0, okc = 0;

    auto step = [&](FactoryClass* f) {
        if (!f || f->Owner != me) return;
        ++n;
        __try {
            if (f->CompletedProduction()) ++okc;
            if (!f->Object) { f->IsSuspended = false; f->OnHold = false; f->Production.Value = 0; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    };

    for (int i = 0; i < me->Buildings.Count; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (b && b->Factory) step(b->Factory);
    }
    if (FactoryClass::Array)
        for (int i = 0; i < FactoryClass::Array->Count; ++i) step(FactoryClass::Array->GetItem(i));

    _snprintf_s(out, cap, _TRUNCATE, "ok n=%d completed=%d", n, okc);
}

static void CmdSpawn(char* out, size_t cap, int cx, int cy)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 spawned=0"); return; }
    auto me = Me(); auto arr = Units();
    CellStruct cell{ (short)cx, (short)cy };
    CoordStruct crd = CellClass::Cell2Coord(cell, 0);

    int done = 0;
    for (int i = 0; i < arr->Count; ++i) {
        TechnoClass* p = arr->GetItem(i);
        if (!p || p->Owner != me) continue;
        auto c = p->GetCoords();
        if (c.X != 0 || c.Y != 0) continue;          // already in the world
        if (p->WhatAmI() == AbstractType::Building) continue;
        if (p->Put(crd, Direction::North)) ++done;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok spawned=%d cell=%d,%d", done, cx, cy);
}

static void CmdProd(char* out, size_t cap, const char* id, int count)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 made=0"); return; }
    if (count < 1) count = 1;

    auto me = Me();
    auto type = TechnoTypeClass::Find(id);
    if (!type) { _snprintf_s(out, cap, _TRUNCATE, "err badtype %s", id); return; }

    auto result = me->CanBuild(type, false, true);
    if (result != CanBuildResult::Buildable) {
        _snprintf_s(out, cap, _TRUNCATE, "err cannot_build %s (result=%d)", id, (int)result);
        return;
    }

    FactoryClass* f = FindFactoryFor(type);
    if (!f) { _snprintf_s(out, cap, _TRUNCATE, "err nofactory %s", id); return; }

    int made = 0;
    for (int i = 0; i < count; ++i) {
        __try {
            bool ok = f->DemandProduction(type, me, made > 0);
            if (!ok && made == 0) ok = f->DemandProduction(type, me, true);
            if (!ok) break;
            ++made;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (made == 0) { _snprintf_s(out, cap, _TRUNCATE, "err exception in prod"); return; }
            break;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok made=%d type=%s", made, id);
}

static void CmdQProd(char* out, size_t cap, const char* id, int count)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    if (count < 1) count = 1;
    auto me = Me();
    auto type = TechnoTypeClass::Find(id);
    if (!type) { _snprintf_s(out, cap, _TRUNCATE, "err badtype %s", id); return; }

    FactoryClass* f = FindFactoryFor(type);
    if (!f) { _snprintf_s(out, cap, _TRUNCATE, "err nofactory %s", id); return; }

    int made = 0;
    int faults = 0;
    unsigned faultAddr = 0, faultCode = 0;
    for (int i = 0; i < count; ++i) {
        int ok = 0;
        bool grew = false;
        __try {
            int before = f->QueuedObjects.Count;
            ok = f->DemandProduction(type, me, true) ? 1 : 0;
            grew = (f->QueuedObjects.Count > before) || ok;
        } __except (g_catchAddr = (unsigned long)GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                    g_catchCode = (unsigned long)GetExceptionInformation()->ExceptionRecord->ExceptionCode,
                    EXCEPTION_EXECUTE_HANDLER) {
            ok = 0; grew = false; ++faults;
            faultAddr = g_catchAddr; faultCode = g_catchCode;
        }
        if (!ok && !grew) break;
        ++made;
    }
    __try {
        f->OnHold = false;
        if (!f->Object) { f->IsSuspended = false; f->StartProduction(); }
        else if (f->IsSuspended) { f->IsSuspended = false; f->Unsuspend(false); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++faults;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok qmade=%d obj=%s pct=%d q=%d susp=%d hold=%d faults=%d addr=0x%08X code=0x%08X",
                made, f->Object ? "obj" : "none", (int)f->Production.Value,
                f->QueuedObjects.Count, (int)f->IsSuspended, (int)f->OnHold, faults,
                (unsigned)faultAddr, (unsigned)faultCode);
}

// 试探 GetPrimaryFactory：调用前后看 FactoryClass::Array 是否增加（=游戏是否现场建厂）
static void CmdTryFac(char* out, size_t cap, int absType, int cat)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    int before = FactoryClass::Array ? FactoryClass::Array->Count : -1;
    FactoryClass* f = nullptr;
    __try {
        f = me->GetPrimaryFactory((AbstractType)absType, false, (BuildCat)cat);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(out, cap, _TRUNCATE, "err exception in getprimary");
        return;
    }
    int after = FactoryClass::Array ? FactoryClass::Array->Count : -1;
    unsigned char* flags = (unsigned char*)me + 0x53d0;
    _snprintf_s(out, cap, _TRUNCATE, "ok abs=%d cat=%d fac=%p grown=%d flags=%d,%d,%d,%d,%d,%d,%d,%d,%d",
                absType, cat, (void*)f, after - before,
                flags[0], flags[1], flags[2], flags[3], flags[4], flags[5], flags[6], flags[7], flags[8]);
}

// 直接改 HouseClass 里的「本类已开始生产」标记位（0x53d0 起），再试建厂
static void CmdForceFlag(char* out, size_t cap, int mode)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    unsigned char* flags = (unsigned char*)me + 0x53d0;
    __try {
        for (int i = 0; i < 9; ++i) flags[i] = mode ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(out, cap, _TRUNCATE, "err exception flags");
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok flags=%d%d%d%d%d%d%d%d%d",
                flags[0], flags[1], flags[2], flags[3], flags[4], flags[5], flags[6], flags[7], flags[8]);
}


static void CmdQueue(char* out, size_t cap, const char* id, int count)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    if (count < 1) count = 1;
    auto me = Me();
    auto type = TechnoTypeClass::Find(id);
    if (!type) { _snprintf_s(out, cap, _TRUNCATE, "err badtype %s", id); return; }
    FactoryClass* f = FindFactoryFor(type);
    if (!f) { _snprintf_s(out, cap, _TRUNCATE, "err nofactory %s", id); return; }

    int made = 0, faults = 0;
    for (int i = 0; i < count; ++i) {
        __try {
            if (!f->QueuedObjects.AddItem(type)) break;
            ++made;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ++faults; break;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok queued=%d q=%d obj=%s faults=%d",
                made, f->QueuedObjects.Count,
                f->Object ? "busy" : "idle", faults);
}

static void CmdDrainQ(char* out, size_t cap, const char* id, int all)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    TechnoTypeClass* type = nullptr;
    if (!all && id && id[0]) {
        type = TechnoTypeClass::Find(id);
        if (!type) { _snprintf_s(out, cap, _TRUNCATE, "err badtype %s", id); return; }
    }

    int dropped = 0, aborted = 0, touched = 0, faults = 0;
    for (int i = 0; i < FactoryClass::Array->Count; ++i) {
        __try {
            FactoryClass* f = FactoryClass::Array->GetItem(i);
            if (!f || f->Owner != me) continue;
            if (type) {
                bool matches = false;
                if (f->Object && f->Object->GetType()) matches = (f->Object->GetType() == type);
                if (!matches && f->QueuedObjects.Count > 0 && f->QueuedObjects[0] == type) matches = true;
                if (!matches) continue;
            }
            ++touched;
            while (f->QueuedObjects.Count > 0 && dropped < 200) {
                f->QueuedObjects.RemoveItem(f->QueuedObjects.Count - 1);
                ++dropped;
            }
            if (f->Object) { f->AbandonProduction(); ++aborted; }
            f->IsSuspended = false;
            f->OnHold = false;
            f->IsManual = false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ++faults;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok drained=%d aborted=%d factories=%d faults=%d",
                dropped, aborted, touched, faults);
}

static void CmdResume(char* out, size_t cap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    int n = 0;
    __try {
        for (int i = 0; i < FactoryClass::Array->Count; ++i) {
            FactoryClass* f = FactoryClass::Array->GetItem(i);
            if (!f || f->Owner != me) continue;
            f->OnHold = false;
            f->IsSuspended = false;
                ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(out, cap, _TRUNCATE, "err exception resume n=%d", n);
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok resumed=%d", n);
}
static void CmdFixFactory(char* out, size_t cap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    me->Update_FactoriesQueues(AbstractType::BuildingType, false, BuildCat::DontCare);
    me->Update_FactoriesQueues(AbstractType::BuildingType, false, BuildCat::Combat);
    me->Update_FactoriesQueues(AbstractType::BuildingType, false, BuildCat::Power);
    me->Update_FactoriesQueues(AbstractType::BuildingType, false, BuildCat::Resoure);
    me->Update_FactoriesQueues(AbstractType::BuildingType, false, BuildCat::Infrastructure);
    me->Update_FactoriesQueues(AbstractType::BuildingType, false, BuildCat::Tech);
    me->Update_FactoriesQueues(AbstractType::UnitType, false, BuildCat::DontCare);
    me->Update_FactoriesQueues(AbstractType::InfantryType, false, BuildCat::DontCare);
    me->Update_FactoriesQueues(AbstractType::AircraftType, false, BuildCat::DontCare);
    _snprintf_s(out, cap, _TRUNCATE, "ok refact=1 n=%d pb=%p pi=%p pv=%p",
                FactoryClass::Array->Count, (void*)me->Primary_ForBuildings,
                (void*)me->Primary_ForInfantry, (void*)me->Primary_ForVehicles);
}

// Emergency: a building created without a FactoryClass cannot order anything.
// Attach one by hand to each of our plants and register it with the house.
// ---- our own factory pump: the game does not tick factories we created ----
static FactoryClass* g_fact[16];
static int g_factN = 0;

static void RegisterFactory(FactoryClass* f)
{
    if (!f) return;
    for (int i = 0; i < g_factN; ++i) if (g_fact[i] == f) return;
    if (g_factN < 16) g_fact[g_factN++] = f;
}

static DWORD WINAPI PumpThread(LPVOID)
{
    bool loggedComp = false;
    while (true) {
        Sleep(66);
        for (int i = 0; i < g_factN; ++i) {
            FactoryClass* f = g_fact[i];
            if (!f) continue;
            __try {
                if (f->Object) {
                    f->Update();
                    int pv = (int)f->Production.Value;
                    if (pv >= 54 || f->IsDone()) {
                        if (!loggedComp) { Log("pump: completed candidate pv=%d\n", pv); loggedComp = true; }
                        f->CompletedProduction();
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("pump: exception on factory %p\n", (void*)f);
                g_fact[i] = nullptr;
            }
        }
    }
    return 0;
}

// ---- ctor probe: find out who creates factory objects when the player clicks ----
extern "C" {
    unsigned long g_ctorRet = 0;
    int g_ctorHits = 0;
    void* g_ctorTramp = 0;
    int g_ctorHooked = 0;
}

void __declspec(naked) CtorProbe()
{
    __asm {
        mov eax, [esp]
        mov dword ptr [g_ctorRet], eax
        inc dword ptr [g_ctorHits]
        jmp dword ptr [g_ctorTramp]
    }
}

static void InstallCtorHook()
{
    if (g_ctorHooked) return;
    BYTE* target = (BYTE*)0x4C98B0;
    g_ctorTramp = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_ctorTramp) return;
    BYTE* tr = (BYTE*)g_ctorTramp;
    memcpy(tr, target, 5);
    tr[5] = 0xE9;
    *(int*)(tr + 6) = (int)(target + 5) - (int)(tr + 10);

    DWORD old = 0;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(int*)(target + 1) = (int)&CtorProbe - (int)(target + 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    g_ctorHooked = 1;
    Log("ctor probe installed, target=%p tramp=%p\n", target, tr);
}

static void RemoveCtorHook()
{
    if (!g_ctorHooked) return;
    BYTE* target = (BYTE*)0x4C98B0;
    DWORD old = 0;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(target, g_ctorTramp, 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    g_ctorHooked = 0;
    Log("ctor probe removed\n");
}
static int TypeArrayIndex(TechnoTypeClass* t)
{
    switch (t->WhatAmI()) {
    case AbstractType::BuildingType:
        for (int i = 0; i < BuildingTypeClass::Array->Count; ++i)
            if (BuildingTypeClass::Array->GetItem(i) == (BuildingTypeClass*)t) return i;
        break;
    case AbstractType::UnitType:
        for (int i = 0; i < UnitTypeClass::Array->Count; ++i)
            if (UnitTypeClass::Array->GetItem(i) == (UnitTypeClass*)t) return i;
        break;
    case AbstractType::InfantryType:
        for (int i = 0; i < InfantryTypeClass::Array->Count; ++i)
            if (InfantryTypeClass::Array->GetItem(i) == (InfantryTypeClass*)t) return i;
        break;
    case AbstractType::AircraftType:
        for (int i = 0; i < AircraftTypeClass::Array->Count; ++i)
            if (AircraftTypeClass::Array->GetItem(i) == (AircraftTypeClass*)t) return i;
        break;
    default: break;
    }
    return -1;
}

static void CmdCameos(char* out, size_t cap, char* body, size_t bodyCap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    SidebarClass* sb = SidebarClass::Instance;
    size_t used = 0; body[0] = 0;
    if (!sb) { _snprintf_s(out, cap, _TRUNCATE, "err nosidebar"); return; }
    for (int tab = 0; tab < 4; ++tab) {
        int n = sb->Tabs[tab].CameoCount;
        if (n < 0 || n > 75) n = 0;
        for (int i = 0; i < n; ++i) {
            CameoDataStruct* c = &sb->Tabs[tab].Cameos[i];
            int w = _snprintf_s(body + used, bodyCap - used, _TRUNCATE, "t%d/%d type=%d idx=%d fac=%p\n",
                                tab, i, (int)c->ItemType, c->ItemIndex, (void*)c->CurrentFactory);
            if (w > 0) used += (size_t)w;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok active=%d n=%d\n%s", sb->ActiveTabIndex, (int)used, body);
}

static void CmdLinkCameo(char* out, size_t cap, const char* id)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    TechnoTypeClass* t = TechnoTypeClass::Find(id);
    if (!t) { _snprintf_s(out, cap, _TRUNCATE, "err badtype %s", id); return; }
    FactoryClass* f = FindFactoryFor(t);
    if (!f) { _snprintf_s(out, cap, _TRUNCATE, "err nofactory %s", id); return; }
    SidebarClass* sb = SidebarClass::Instance;
    if (!sb) { _snprintf_s(out, cap, _TRUNCATE, "err nosidebar"); return; }
    int idx = TypeArrayIndex(t);
    int linked = 0, seen = 0;
    for (int tab = 0; tab < 4; ++tab) {
        for (int i = 0; i < sb->Tabs[tab].CameoCount; ++i) {
            CameoDataStruct* c = &sb->Tabs[tab].Cameos[i];
            if (c->ItemIndex != idx) continue;
            ++seen;
            c->CurrentFactory = f;
            c->Progress = f->Production;
            ++linked;
        }
    }
    sb->SidebarNeedsRepaint(0);
    _snprintf_s(out, cap, _TRUNCATE, "ok idx=%d seen=%d linked=%d", idx, seen, linked);
}
static void CmdNewFactory(char* out, size_t cap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();

    int made = 0;
    for (int i = 0; i < me->Buildings.Count; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (!b || !b->Type) continue;
        AbstractType abs = b->Type->Factory;
        if ((int)abs <= 0) continue;
        if (b->Factory) continue;

        // allocate with the game's own allocator so that later frees stay consistent
        // use the game's own allocator + constructor so the vtable and heap match
        typedef void* (__cdecl * AllocFn)(unsigned);
        void* raw = ((AllocFn)0x7C8E17)(0x74u);
        if (!raw) continue;
        ((void (__thiscall *)(void*))0x4C98B0)(raw);
        FactoryClass* f = (FactoryClass*)raw;
        f->Owner = me;
        f->Object = nullptr;
        f->IsSuspended = false;
        f->IsManual = false;
        f->OnHold = false;
        b->Factory = f;

        const BuildCat cats[] = { BuildCat::DontCare, BuildCat::Tech, BuildCat::Resoure,
                                  BuildCat::Power, BuildCat::Infrastructure, BuildCat::Combat };
        for (auto c : cats) me->SetPrimaryFactory(f, abs, false, c);
        RegisterFactory(f);
        ++made;
    }

    _snprintf_s(out, cap, _TRUNCATE, "ok made=%d array=%d pb=%p pi=%p pv=%p pa=%p",
                made, FactoryClass::Array ? FactoryClass::Array->Count : -1,
                (void*)me->Primary_ForBuildings, (void*)me->Primary_ForInfantry,
                (void*)me->Primary_ForVehicles, (void*)me->Primary_ForAircraft);
}

// 诊断「建好了但放不下去」：看 pending 建筑与目标格的可放置条件
static void CmdPutInfo(char* out, size_t cap, char* body, size_t bodyCap, int cx, int cy)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    size_t used = 0; body[0] = 0;
    auto put = [&](const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        int w = _vsnprintf_s(body + used, bodyCap - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (w > 0) used += (size_t)w;
    };

    CellStruct cell{ (short)cx, (short)cy };
    CoordStruct crd = CellClass::Cell2Coord(cell, 0);
    put("crd=%d,%d,%d\n", crd.X, crd.Y, crd.Z);

    CellClass* c = MapClass::Instance->TryGetCellAt(crd);
    if (!c) { put("nocell\n"); _snprintf_s(out, cap, _TRUNCATE, "ok ready=1\n%s", body); return; }
    put("mapcell=%d,%d land=%d flags=0x%x content=%p height=%d\n",
        (int)c->MapCoords.X, (int)c->MapCoords.Y,
        (int)c->LandType, (unsigned)c->Flags, (void*)c->GetContent(), (int)c->Level);

    BuildingClass* pending = nullptr;
    if (FactoryClass::Array) {
        for (int i = 0; i < FactoryClass::Array->Count; ++i) {
            FactoryClass* f = FactoryClass::Array->GetItem(i);
            if (!f || f->Owner != me || !f->Object) continue;
            if (f->Object->WhatAmI() == AbstractType::Building) pending = (BuildingClass*)f->Object;
        }
    }
    if (!pending) { put("nopending\n"); _snprintf_s(out, cap, _TRUNCATE, "ok ready=1\n%s", body); return; }

    auto id = pending->get_ID();
    auto bt = (BuildingTypeClass*)pending->Type;
    put("pending=%s limbo=%d alive=%d hp=%d own=%d\n", id ? id : "?", pending->InLimbo ? 1 : 0,
        pending->IsAlive ? 1 : 0, pending->Health, pending->Owner == me ? 1 : 0);
    put("foundation=%d w=%d h=%d\n", bt ? (int)bt->Foundation : -1,
        bt ? (int)bt->GetFoundationWidth() : -1, bt ? (int)bt->GetFoundationHeight(true) : -1);

    bool put1 = false;
    __try { put1 = pending->Put(crd, Direction::North); }
    __except (EXCEPTION_EXECUTE_HANDLER) { put("exc=1\n"); }
    put("put=%d\n", put1 ? 1 : 0);
    if (put1) {
        pending->Place(false);
        pending->ActuallyPlacedOnMap = true;
        put("placed=1 hp=%d\n", pending->Health);
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok ready=1\n%s", body);
}

static void CmdDiag(char* out, size_t cap, char* body, size_t bodyCap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    size_t used = 0; body[0] = 0;

    auto put = [&](const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        int w = _vsnprintf_s(body + used, bodyCap - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (w > 0) used += (size_t)w;
    };

    put("factories=%d\n", FactoryClass::Array ? FactoryClass::Array->Count : -1);
    if (FactoryClass::Array) {
        for (int i = 0; i < FactoryClass::Array->Count; ++i) {
            FactoryClass* f = FactoryClass::Array->GetItem(i);
            if (!f) { put("  #%d null\n", i); continue; }
            const char* id = "-";
            if (f->Object) { auto s = f->Object->get_ID(); if (s) id = s; }
            put("  #%d mine=%d obj=%s pct=%d q=%d susp=%d\n",
                i, f->Owner == me ? 1 : 0, id, f->GetProgress(),
                f->QueuedObjects.Count, f->IsSuspended ? 1 : 0);
        }
    }
    put("p_build=%p p_inf=%p p_veh=%p p_air=%p\n",
        (void*)me->Primary_ForBuildings, (void*)me->Primary_ForInfantry,
        (void*)me->Primary_ForVehicles, (void*)me->Primary_ForAircraft);
    put("buildings=%d\n", me->Buildings.Count);
    for (int i = 0; i < me->Buildings.Count && i < 16; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (!b) continue;
        auto s = b->get_ID();
        CoordStruct c = b->GetCoords();
        CellClass* bc2 = MapClass::Instance->TryGetCellAt(c);
        put("  %s x=%d y=%d map=%d,%d hp=%d typeFactory=%d typeBuildCat=%d canPower=%d bfact=%p\n", s ? s : "?",
            c.X, c.Y, bc2 ? (int)bc2->MapCoords.X : -1, bc2 ? (int)bc2->MapCoords.Y : -1,
            b->Health, b->Type ? (int)b->Type->Factory : -1,
            b->Type ? (int)b->Type->BuildCat : -1, (int)b->Type->WhatAmI(), (void*)b->Factory);
    }
    auto bt = static_cast<BuildingTypeClass*>(TechnoTypeClass::Find("NAPOWR"));
    if (bt) put("napowr_whatami=%d naval=%d factory=%d buildcat=%d cost=%d\n",
                (int)bt->WhatAmI(), bt->Naval ? 1 : 0, (int)bt->Factory, (int)bt->BuildCat, bt->GetCost());

    _snprintf_s(out, cap, _TRUNCATE, "ok ready=1\n%s", body);
}

static void CmdStatus(char* out, size_t cap, char* body, size_t bodyCap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 n=0"); return; }

    auto me = Me();
    size_t used = 0; int n = 0; body[0] = 0;

    for (int i = 0; i < FactoryClass::Array->Count; ++i) {
        FactoryClass* f = FactoryClass::Array->GetItem(i);
        if (!f || f->Owner != me) continue;

        const char* id = "-";
        int pct = 0, q = 0;
        if (f->Object) { auto t = f->Object->get_ID(); id = t ? t : "?"; }
        else if (f->QueuedObjects.Count > 0) { auto t = f->QueuedObjects[0]; auto s = t->get_ID(); id = s ? s : "?"; }
        pct = f->GetProgress();
        q = f->QueuedObjects.Count;

        int w = _snprintf_s(body + used, bodyCap - used, _TRUNCATE, "%s|%d|%d|%d|%d|pv=%d ps=%d\n",
                            id, pct, q, f->IsSuspended ? 1 : 0, f->OnHold ? 1 : 0,
                            (int)f->Production.Value, (int)f->Production.Step);
        if (w <= 0) break;
        used += (size_t)w; ++n;
        if (bodyCap - used < 128) break;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok ready=1 n=%d\n%s", n, body);
}

static void CmdPlace(char* out, size_t cap, int cx, int cy)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 placed=0"); return; }

    auto me = Me();
    CellStruct cell{ (short)cx, (short)cy };
    CoordStruct crd = CellClass::Cell2Coord(cell, 0);

    int placed = 0; const char* last = "-";

    auto tryPlace = [&](FactoryClass* f) {
        if (!f || f->Owner != me) return;
        TechnoClass* obj = f->Object;
        if (!obj || obj->WhatAmI() != AbstractType::Building) return;
        auto id = obj->get_ID();
        BuildingClass* bc = (BuildingClass*)obj;
        bool put = bc->Put(crd, Direction::North);
        if (put) {
            bc->Place(false);
            bc->ActuallyPlacedOnMap = true;
            // the game clears the finished object itself once it is placed; do it
            // by hand so the factory can start the next item
            f->Object = nullptr;
            f->IsSuspended = false;
            f->IsManual = false;
            f->OnHold = false;
            f->Balance = 0;
            f->OriginalBalance = 0;
            f->Production.Value = 0;
            ++placed; last = id ? id : "?";
        }
    };

    for (int i = 0; i < me->Buildings.Count; ++i) {
        BuildingClass* b = me->Buildings.GetItem(i);
        if (b && b->Factory) tryPlace(b->Factory);
    }
    if (FactoryClass::Array) {
        for (int i = 0; i < FactoryClass::Array->Count; ++i) tryPlace(FactoryClass::Array->GetItem(i));
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok placed=%d last=%s cell=%d,%d", placed, last, cx, cy);
}

// ---------------------------------------------------------------- commands

static void CmdState(char* out, size_t cap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    _snprintf_s(out, cap, _TRUNCATE, "ok ready=1 frame=%d money=%d mine=%d",
                Frame(), Money(), MyCount());
}

// 扫描战场上不属于我的东西（敌人 / 中立），用于决定打谁
// 列出所有阵营，用来判断这局到底有没有敌人
// 通过 HouseClass 自己的列表查看某个阵营的家底（绕开 TechnoClass::Owner 偏移问题）
static void CmdHouseDump(char* out, size_t cap, char* body, size_t bodyCap, int idx)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto& harr = HouseClass::Array;
    if (!harr || idx < 0 || idx >= harr->Count) { _snprintf_s(out, cap, _TRUNCATE, "err bad house"); return; }
    HouseClass* h = harr->GetItem(idx);
    if (!h) { _snprintf_s(out, cap, _TRUNCATE, "err null house"); return; }
    size_t used = 0; body[0] = 0;
    auto put = [&](const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        int w = _vsnprintf_s(body + used, bodyCap - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (w > 0) used += (size_t)w;
    };
    put("house=%d id=%s bld=%d money=%d\n", idx,
        (h->Type && h->Type->ID) ? h->Type->ID : "?", h->Buildings.Count, h->Available_Money());
    for (int i = 0; i < h->Buildings.Count && i < 24; ++i) {
        BuildingClass* b = h->Buildings.GetItem(i);
        if (!b) continue;
        CoordStruct c = b->GetCoords();
        put("  B %s x=%d y=%d hp=%d\n", IdOf(b), c.X, c.Y, b->Health);
    }
    auto arr = Units();
    for (int i = 0; arr && i < arr->Count; ++i) {
        TechnoClass* p = arr->GetItem(i);
        if (!p || !p->IsAlive || p->WhatAmI() == AbstractType::Building) continue;
        if (p->Owner != h) continue;
        CoordStruct c = p->GetCoords();
        put("  T %s x=%d y=%d hp=%d\n", IdOf(p), c.X, c.Y, p->Health);
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok ready=1\n%s", body);
}

static void CmdHouses(char* out, size_t cap, char* body, size_t bodyCap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 n=0"); return; }
    auto me = Me();
    size_t used = 0; int n = 0;
    body[0] = 0;
    auto& harr = HouseClass::Array;
    for (int i = 0; harr && i < harr->Count; ++i) {
        HouseClass* h = harr->GetItem(i);
        if (!h) continue;
        const char* id = h->Type ? h->Type->ID : "?";
        int w = _snprintf_s(body + used, bodyCap - used, _TRUNCATE,
                            "%d|%s|me=%d|player=%d|neutral=%d|dead=%d|bld=%d\n",
                            i, id ? id : "?", h == me ? 1 : 0, h->IsPlayer() ? 1 : 0,
                            h->IsNeutral() ? 1 : 0, h->Defeated ? 1 : 0, h->Buildings.Count);
        if (w <= 0) break;
        used += (size_t)w; ++n;
        if (bodyCap - used < 128) break;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok ready=1 n=%d\n%s", n, body);
}

static void CmdScan(char* out, size_t cap, char* body, size_t bodyCap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 n=0"); return; }
    auto me = Me(); auto arr = Units();
    size_t used = 0; int n = 0, mine = 0;
    body[0] = 0;
    CoordStruct mc = CellClass::Cell2Coord(me->GetBaseCenter(), 0);
    for (int i = 0; i < arr->Count; ++i) {
        TechnoClass* p = arr->GetItem(i);
        if (!p || !p->IsAlive) continue;
        auto crd = p->GetCoords();
        if (p->Owner == me) { ++mine; continue; }
        if (p->Owner && p->Owner->IsNeutral()) continue;
        if (p->Owner && p->Owner->Defeated) continue;
        if (p->Owner && p->Owner->Type && me->Type && p->Owner->Type->ID && me->Type->ID &&
            !_stricmp(p->Owner->Type->ID, me->Type->ID)) continue; // 同 ID 的幽灵对象
        long long dx = crd.X - mc.X, dy = crd.Y - mc.Y;
        int d = (int)((dx * dx + dy * dy) / 65536); // 距离，单位=格（256 坐标单位）
        int w = _snprintf_s(body + used, bodyCap - used, _TRUNCATE,
                            "%d|%s|x=%d|y=%d|hp=%d|d=%d|bld=%d|owner=%s\n",
                            i, IdOf(p), crd.X, crd.Y, p->Health, d,
                            p->WhatAmI() == AbstractType::Building ? 1 : 0,
                            (p->Owner && p->Owner->Type && p->Owner->Type->ID) ? p->Owner->Type->ID : "?");
        if (w <= 0) break;
        used += (size_t)w; ++n;
        if (bodyCap - used < 128) break;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok ready=1 mine=%d others=%d\n%s", mine, n, body);
}

static void CmdUnits(char* out, size_t cap, char* body, size_t bodyCap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 n=0"); return; }

    auto me = Me(); auto arr = Units();
    size_t used = 0; int n = 0;
    body[0] = 0;

    for (int i = 0; i < arr->Count; ++i) {
        TechnoClass* p = arr->GetItem(i);
        if (!p || p->Owner != me) continue;

        auto crd = p->GetCoords();
        int tgt = -1;
        if (p->Target) {
            for (int j = 0; j < arr->Count; ++j)
                if (arr->GetItem(j) == p->Target) { tgt = j; break; }
        }
        int w = _snprintf_s(body + used, bodyCap - used, _TRUNCATE,
                            "%d|%s|%d|%d|%d|%d|%d|%d|%d\n",
                            i, IdOf(p), crd.X, crd.Y, crd.Z,
                            p->Health, p->Group, (int)p->GetCurrentMission(), tgt);
        if (w <= 0) break;
        used += (size_t)w; ++n;
        if (bodyCap - used < 128) break;
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok ready=1 n=%d\n%s", n, body);
}

// 验证用户 CE 外挂的判据：cmp [esi+0x6C+0x3B0],0 —— 为 0 表示「这是我方」
// 这里把 TechnoClass::Array 里的对象逐个打 Owner==Me()、Health(+0x6C)、+0x41C
static void CmdProbe(char* out, size_t cap, const char* sel)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    TechnoClass* list[8] = {0};
    int n = Collect(sel && *sel ? sel : "all", list, 8);
    int yrppOwnerOff = (int)((char*)&list[0]->Owner - (char*)list[0]);
    size_t used = 0;
    used += _snprintf_s(out + used, cap - used, _TRUNCATE,
        "ok men=%d me=%p ownerOffYrpp=0x%X\n", n, (void*)me, yrppOwnerOff);
    for (int k = 0; k < n && k < 2; ++k) {
        TechnoClass* t = list[k];
        used += _snprintf_s(out + used, cap - used, _TRUNCATE,
            "obj%p id=%s hp=%d f41C=%08X\n", (void*)t, IdOf(t),
            (int)(uintptr_t)(*(void**)((char*)t + 0x6C)),
            (unsigned)(uintptr_t)(*(void**)((char*)t + 0x41C)));
    }
    // 全表抽查：我方 vs 敌方在 +0x41C 上的差别
    used += _snprintf_s(out + used, cap - used, _TRUNCATE, "-- +0x41C scan --\n");
    auto arr = Units();
    if (arr) {
        int mine = 0, foe = 0;
        for (int i = 0; i < arr->Count && (mine < 4 || foe < 4); ++i) {
            TechnoClass* t = arr->GetItem(i);
            if (!t || !t->Owner) continue;
            int v = (int)(uintptr_t)(*(void**)((char*)t + 0x41C));
            bool isMine = (t->Owner == me);
            if (isMine && mine < 4) {
                used += _snprintf_s(out + used, cap - used, _TRUNCATE,
                    "mine %s hp=%d f41C=%08X\n", IdOf(t),
                    (int)(uintptr_t)(*(void**)((char*)t + 0x6C)), (unsigned)v);
                ++mine;
            } else if (!isMine && foe < 4) {
                used += _snprintf_s(out + used, cap - used, _TRUNCATE,
                    "foe  %s(%s) hp=%d f41C=%08X\n", IdOf(t),
                    (t->Owner->Type && t->Owner->Type->ID) ? t->Owner->Type->ID : "?",
                    (int)(uintptr_t)(*(void**)((char*)t + 0x6C)), (unsigned)v);
                ++foe;
            }
        }
    }
    if (n == 0) _snprintf_s(out + used, cap - used, _TRUNCATE, "no units matched '%s'", sel ? sel : "");

    // HouseClass 侧：看「我方」与「敌方」在 +0x60..+0x7C 有什么不同
    used += _snprintf_s(out + used, cap - used, _TRUNCATE, "-- houses --\n");
    if (HouseClass::Array) {
        for (int i = 0; i < HouseClass::Array->Count && i < 6; ++i) {
            HouseClass* h = HouseClass::Array->GetItem(i);
            if (!h) continue;
            used += _snprintf_s(out + used, cap - used, _TRUNCATE, "h%d %s me=%d:",
                i, (h->Type && h->Type->ID) ? h->Type->ID : "?",
                (h == me) ? 1 : 0);
            for (int off = 0x60; off <= 0x7C; off += 4)
                used += _snprintf_s(out + used, cap - used, _TRUNCATE, " %X:%08X", off,
                    (unsigned)(uintptr_t)(*(void**)((char*)h + off)));
            used += _snprintf_s(out + used, cap - used, _TRUNCATE, "\n");
        }
    }
}

// 用「+0x41C == 0」这条用户 CE 判据来认我方，并按四种组合统计：
// owner==me / f41C==0 一致多少、不一致多少（不一致的逐条打印，便于决定该信哪个）
static void CmdMine(char* out, size_t cap)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    auto me = Me();
    auto arr = Units();
    if (!arr) { _snprintf_s(out, cap, _TRUNCATE, "err no array"); return; }
    int both = 0, ownOnly = 0, flagOnly = 0, neither = 0;
    size_t used = 0;
    used += _snprintf_s(out + used, cap - used, _TRUNCATE, "ok me=%p\n", (void*)me);
    for (int i = 0; i < arr->Count; ++i) {
        TechnoClass* t = arr->GetItem(i);
        if (!t) continue;
        bool byOwner = (t->Owner == me);
        int f41C = 0;
        __try { f41C = *(int*)((char*)t + 0x41C); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        bool byFlag = (f41C == 0);
        if (byOwner && byFlag) { ++both; continue; }
        if (byOwner && !byFlag) { ++ownOnly; }
        else if (!byOwner && byFlag) { ++flagOnly; }
        else { ++neither; continue; }
        if (used < cap - 200) {
            used += _snprintf_s(out + used, cap - used, _TRUNCATE,
                "%s %s idx=%d hp=%d f41C=%08X owner=%s\n",
                byOwner ? "own+flag-" : "own-flag+", IdOf(t), i,
                *(int*)((char*)t + 0x6C), (unsigned)f41C,
                (t->Owner && t->Owner->Type && t->Owner->Type->ID) ? t->Owner->Type->ID : "?");
        }
    }
    used += _snprintf_s(out + used, cap - used, _TRUNCATE,
        "both=%d ownerOnly=%d flagOnly=%d neither=%d", both, ownOnly, flagOnly, neither);
}

// 直接改血量（0 = 立即死亡）。用户在 CE 里就是用 +0x6C 做「扣血即死」的。
static void CmdHp(char* out, size_t cap, const char* sel, int value)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0"); return; }
    static TechnoClass* list[512];
    int n = Collect(sel, list, 512);
    int done = 0;
    for (int i = 0; i < n; ++i) {
        __try {
            *(int*)((char*)list[i] + 0x6C) = value;
            ++done;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok set=%d value=%d of %d", done, value, n);
}

static void CmdMove(char* out, size_t cap, const char* sel, int cx, int cy)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 moved=0"); return; }

    CellStruct cell{ (short)cx, (short)cy };
    CellClass* pCell = MapClass::Instance->GetCellAt(cell);
    if (!pCell) { _snprintf_s(out, cap, _TRUNCATE, "err badcell"); return; }

    TechnoClass* list[1024];
    int n = Collect(sel, list, 1024);
    int ok = 0; const char* stage = "none";
    for (int i = 0; i < n; ++i) {
        __try {
            stage = "setdest";
            list[i]->SetDestination(pCell, false);
            stage = "mission";
            list[i]->QueueMission(Mission::Move, false);
            stage = "ok";
            ++ok;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(out, cap, _TRUNCATE, "err av stage=%s i=%d", stage, i);
            return;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok moved=%d cell=%d,%d", ok, cx, cy);
}

static void CmdAttack(char* out, size_t cap, const char* sel, int targetIdx)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 ordered=0"); return; }

    auto arr = Units();
    if (targetIdx < 0 || targetIdx >= arr->Count) {
        _snprintf_s(out, cap, _TRUNCATE, "err badtarget"); return;
    }
    TechnoClass* pTarget = arr->GetItem(targetIdx);
    if (!pTarget) { _snprintf_s(out, cap, _TRUNCATE, "err nulltarget"); return; }

    TechnoClass* list[1024];
    int n = Collect(sel, list, 1024);
    for (int i = 0; i < n; ++i) {
        list[i]->SetTarget(pTarget);
        list[i]->QueueMission(Mission::Attack, false);
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok ordered=%d target=%d(%s)", n, targetIdx, IdOf(pTarget));
}

static void CmdMission(char* out, size_t cap, const char* sel, Mission m, const char* label)
{
    if (!Ready()) { _snprintf_s(out, cap, _TRUNCATE, "ok ready=0 ordered=0"); return; }
    TechnoClass* list[1024];
    int n = Collect(sel, list, 1024);
    for (int i = 0; i < n; ++i) {
        if (m == Mission::Guard) { list[i]->Guard(); continue; }
        __try {
            list[i]->QueueMission(m, true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(out, cap, _TRUNCATE, "err av in mission i=%d n=%d", i, n);
            return;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "ok ordered=%d mission=%s", n, label);
}

static void Handle(const char* line, char* out, size_t cap)
{
    char cmd[32] = {0};
    if (sscanf_s(line, "%31s", cmd, (unsigned)sizeof(cmd)) != 1) {
        _snprintf_s(out, cap, _TRUNCATE, "err empty"); return;
    }

    if (_stricmp(cmd, "ping") == 0) {
        _snprintf_s(out, cap, _TRUNCATE, "ok pong frame=%d ready=%d", Frame(), Ready() ? 1 : 0);
    } else if (_stricmp(cmd, "state") == 0) {
        CmdState(out, cap);
    } else if (_stricmp(cmd, "units") == 0) {
        static char body[64 * 1024];
        CmdUnits(out, cap, body, sizeof(body));
    } else if (_stricmp(cmd, "tickf") == 0) {
        int t = 600;
        if (sscanf_s(line, "%*s %d", &t) != 1) t = 600;
        CmdTickF(out, cap, t);
    } else if (_stricmp(cmd, "whoami") == 0) {
        CmdWhoami(out, cap);
    } else if (_stricmp(cmd, "primary") == 0) {
        CmdPrimary(out, cap);
    } else if (_stricmp(cmd, "loco") == 0) {
        char sel[512] = {0};
        if (sscanf_s(line, "%*s %511s", sel, (unsigned)sizeof(sel)) != 1) {
            _snprintf_s(out, cap, _TRUNCATE, "err usage: loco <sel>");
        } else CmdLoco(out, cap, sel);
    } else if (_stricmp(cmd, "unlimbo") == 0) {
        char sel[512] = {0};
        if (sscanf_s(line, "%*s %511s", sel, (unsigned)sizeof(sel)) != 1) {
            _snprintf_s(out, cap, _TRUNCATE, "err usage: unlimbo <sel>");
        } else CmdUnlimbo(out, cap, sel);
    } else if (_stricmp(cmd, "start") == 0) {
        char id[64] = {0};
        if (sscanf_s(line, "%*s %63s", id, (unsigned)sizeof(id)) != 1) {
            _snprintf_s(out, cap, _TRUNCATE, "err usage: start <id>");
        } else {
            CmdStartProd(out, cap, id);
        }
    } else if (_stricmp(cmd, "tickdone") == 0) {
        int t = 600;
        if (sscanf_s(line, "%*s %d", &t) != 1) t = 600;
        CmdFinishTick(out, cap, t);
    } else if (_stricmp(cmd, "pump") == 0) {
        int t = 60;
        if (sscanf_s(line, "%*s %d", &t) != 1) t = 60;
        CmdPump(out, cap, t);
    } else if (_stricmp(cmd, "finish") == 0) {
        CmdFinish(out, cap);
    } else if (_stricmp(cmd, "spawn") == 0) {
        int x = 0, y = 0;
        if (sscanf_s(line, "%*s %d %d", &x, &y) != 2)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: spawn <cellX> <cellY>");
        else CmdSpawn(out, cap, x, y);
    } else if (_stricmp(cmd, "qprod") == 0) {
        char id[64] = {0}; int c = 1;
        if (sscanf_s(line, "%*s %63s %d", id, (unsigned)sizeof(id), &c) < 1)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: qprod <typeId> [count]");
        else CmdQProd(out, cap, id, c);
    } else if (_stricmp(cmd, "resume") == 0) {
        CmdResume(out, cap);
    } else if (_stricmp(cmd, "aiprod") == 0) {
        auto me2 = Me();
        me2->Production = true;
        _snprintf_s(out, cap, _TRUNCATE, "ok house production flag=%d", (int)me2->Production);
    } else if (_stricmp(cmd, "hookctor") == 0) {
        InstallCtorHook();
        _snprintf_s(out, cap, _TRUNCATE, "ok hooked=%d", g_ctorHooked);
    } else if (_stricmp(cmd, "unhook") == 0) {
        RemoveCtorHook();
        _snprintf_s(out, cap, _TRUNCATE, "ok hooked=%d", g_ctorHooked);
    } else if (_stricmp(cmd, "hookinfo") == 0) {
        _snprintf_s(out, cap, _TRUNCATE, "ok hits=%d lastRet=0x%08X hooked=%d", g_ctorHits, (unsigned)g_ctorRet, g_ctorHooked);
    } else if (_stricmp(cmd, "cameos") == 0) {
        static char cbody[32 * 1024];
        CmdCameos(out, cap, cbody, sizeof(cbody));
    } else if (_stricmp(cmd, "linkcameo") == 0) {
        char id[64] = {0};
        if (sscanf_s(line, "%*s %63s", id, (unsigned)sizeof(id)) < 1)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: linkcameo <typeId>");
        else CmdLinkCameo(out, cap, id);
    } else if (_stricmp(cmd, "flag") == 0) {
        int m = 1; sscanf_s(line, "%*s %d", &m);
        CmdForceFlag(out, cap, m);
    } else if (_stricmp(cmd, "house") == 0) {
        int idx = 1; sscanf_s(line, "%*s %d", &idx);
        static char dbody[32 * 1024];
        CmdHouseDump(out, cap, dbody, sizeof(dbody), idx);
    } else if (_stricmp(cmd, "houses") == 0) {
        static char hbody[16 * 1024];
        CmdHouses(out, cap, hbody, sizeof(hbody));
    } else if (_stricmp(cmd, "scan") == 0) {
        static char sbody[32 * 1024];
        CmdScan(out, cap, sbody, sizeof(sbody));
    } else if (_stricmp(cmd, "putinfo") == 0) {
        int x = 0, y = 0; sscanf_s(line, "%*s %d %d", &x, &y);
        static char pbody[8 * 1024];
        CmdPutInfo(out, cap, pbody, sizeof(pbody), x, y);
    } else if (_stricmp(cmd, "tryfac") == 0) {
        int a = 7, c = 0;
        sscanf_s(line, "%*s %d %d", &a, &c);
        CmdTryFac(out, cap, a, c);
    } else if (_stricmp(cmd, "queue") == 0) {
        char id[64] = {0}; int c = 1;
        if (sscanf_s(line, "%*s %63s %d", id, (unsigned)sizeof(id), &c) < 1)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: queue <typeId> [count]");
        else CmdQueue(out, cap, id, c);
    } else if (_stricmp(cmd, "drainq") == 0) {
        char id[64] = {0};
        int got = sscanf_s(line, "%*s %63s", id, (unsigned)sizeof(id));
        CmdDrainQ(out, cap, got >= 1 ? id : "", got >= 1 ? 0 : 1);
    } else if (_stricmp(cmd, "drainall") == 0) {
        CmdDrainQ(out, cap, "", 1);
    } else if (_stricmp(cmd, "newfactory") == 0) {
        CmdNewFactory(out, cap);
    } else if (_stricmp(cmd, "fixfactory") == 0) {
        CmdFixFactory(out, cap);
    } else if (_stricmp(cmd, "mine") == 0) {
        CmdMine(out, cap);
    } else if (_stricmp(cmd, "hp") == 0) {
        char sel[256] = {0}; int v = 1;
        int got = sscanf_s(line, "%*s %255s %d", sel, (unsigned)sizeof(sel), &v);
        if (got < 1) _snprintf_s(out, cap, _TRUNCATE, "err usage: hp <sel> <value>");
        else CmdHp(out, cap, sel, v);
    } else if (_stricmp(cmd, "probe") == 0) {
        char sel[256] = {0};
        if (sscanf_s(line, "%*s %255s", sel, (unsigned)sizeof(sel)) < 1) sel[0] = 0;
        CmdProbe(out, cap, sel);
    } else if (_stricmp(cmd, "diag") == 0) {
        static char body[32 * 1024];
        CmdDiag(out, cap, body, sizeof(body));
    } else if (_stricmp(cmd, "canbuild") == 0) {
        static char body[64 * 1024];
        CmdCanBuild(out, cap, body, sizeof(body));
    } else if (_stricmp(cmd, "status") == 0) {
        static char body[16 * 1024];
        CmdStatus(out, cap, body, sizeof(body));
    } else if (_stricmp(cmd, "prod") == 0) {
        char id[64] = {0}; int c = 1;
        if (sscanf_s(line, "%*s %63s %d", id, (unsigned)sizeof(id), &c) < 1)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: prod <typeId> [count]");
        else CmdProd(out, cap, id, c);
    } else if (_stricmp(cmd, "place") == 0) {
        int x = 0, y = 0;
        if (sscanf_s(line, "%*s %d %d", &x, &y) != 2)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: place <cellX> <cellY>");
        else CmdPlace(out, cap, x, y);
    } else if (_stricmp(cmd, "move") == 0) {
        char sel[512] = {0}; int x = 0, y = 0;
        if (sscanf_s(line, "%*s %511s %d %d", sel, (unsigned)sizeof(sel), &x, &y) != 3) {
            _snprintf_s(out, cap, _TRUNCATE, "err usage: move <sel> <cx> <cy>");
        } else CmdMove(out, cap, sel, x, y);
    } else if (_stricmp(cmd, "attack") == 0) {
        char sel[512] = {0}; int t = -1;
        if (sscanf_s(line, "%*s %511s %d", sel, (unsigned)sizeof(sel), &t) != 2) {
            _snprintf_s(out, cap, _TRUNCATE, "err usage: attack <sel> <unitIndex>");
        } else CmdAttack(out, cap, sel, t);
    } else if (_stricmp(cmd, "guard") == 0) {
        char sel[512] = {0};
        if (sscanf_s(line, "%*s %511s", sel, (unsigned)sizeof(sel)) != 1)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: guard <sel>");
        else CmdMission(out, cap, sel, Mission::Guard, "guard");
    } else if (_stricmp(cmd, "stop") == 0) {
        char sel[512] = {0};
        if (sscanf_s(line, "%*s %511s", sel, (unsigned)sizeof(sel)) != 1)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: stop <sel>");
        else CmdMission(out, cap, sel, Mission::Stop, "stop");
    } else if (_stricmp(cmd, "deploy") == 0) {
        char sel[512] = {0};
        if (sscanf_s(line, "%*s %511s", sel, (unsigned)sizeof(sel)) != 1)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: deploy <sel>");
        else CmdMission(out, cap, sel, Mission::Unload, "deploy");
    } else if (_stricmp(cmd, "mission") == 0) {
        char sel[512] = {0}; int m = -1;
        if (sscanf_s(line, "%*s %511s %d", sel, (unsigned)sizeof(sel), &m) != 2)
            _snprintf_s(out, cap, _TRUNCATE, "err usage: mission <sel> <missionNumber>");
        else CmdMission(out, cap, sel, (Mission)m, "raw");
    } else {
        _snprintf_s(out, cap, _TRUNCATE,
                    "err unknown: ping state units canbuild status prod place move attack guard stop deploy mission");
    }
}

// ---------------------------------------------------------------- pipe loop

static DWORD WINAPI Server(LPVOID)
{
    // LocomotionClass::ChangeLocomotorTo goes through CoCreateInstance, which needs
    // a COM apartment on this thread or it faults.
    OleInitialize(nullptr);
    Log("mo-pilot dll loaded, pid=%lu", GetCurrentProcessId());

    for (;;) {
        HANDLE hPipe = CreateNamedPipeW(
            PipeName(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) { Log("CreateNamedPipe failed %lu", GetLastError()); Sleep(1000); continue; }

        BOOL ok = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!ok) { CloseHandle(hPipe); continue; }

        static char inbuf[8192];
        static char outbuf[80 * 1024];
        DWORD got = 0;
        size_t fill = 0;

        while (fill < sizeof(inbuf) - 1) {
            if (!ReadFile(hPipe, inbuf + fill, (DWORD)(sizeof(inbuf) - 1 - fill), &got, nullptr) || got == 0) break;
            fill += got;
            if (memchr(inbuf, '\n', fill)) break;
        }
        if (fill > 0) {
            inbuf[fill] = 0;
            for (size_t i = 0; i < fill; ++i) if (inbuf[i] == '\r' || inbuf[i] == '\n') inbuf[i] = 0;

            outbuf[0] = 0;
            __try {
                Handle(inbuf, outbuf, sizeof(outbuf));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                _snprintf_s(outbuf, sizeof(outbuf), _TRUNCATE, "err exception in game call");
            }

            size_t len = strlen(outbuf);
            if (len == 0) { strcpy_s(outbuf, "ok"); len = 2; }
            DWORD wrote = 0;
            WriteFile(hPipe, outbuf, (DWORD)len, &wrote, nullptr);
        }

        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
    return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(nullptr, 0, Server, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        HANDLE hp = CreateThread(nullptr, 0, PumpThread, nullptr, 0, nullptr);
        if (hp) CloseHandle(hp);
    }
    return TRUE;
}
