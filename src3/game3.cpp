#include "game3.h"

// GTA III 1.1 (non-Steam). CodeWarrior build; heap through GlobalAlloc.
static const Addrs3 kIII11 = {
    Game3::III_11, "GTA III 1.1", false,
    0x5EA000, 0x95D000,                // .data .. end of .bss
    0x582D5D, 0x5D825A, 0x5D82A0,      // WinMain's call [PeekMessageA]; __sys_alloc / __sys_free
    false,
    0x8F58EC,
    0x8F5B9D,
    0x9414A8,
    0x8F4430, 0x28,
    0x885AF8,
    0x9414A4,
    0x95CF34, 0x95CF69,
    {{0x62A308, 0x1C}, {0x9436AC, 4}}, // suspend/old perf counters, suspend depth; old PC timer
    0x619790, 0x663728, 0x288, 0x5BD7C0, 0x5BD7F0,
    {0x621030, 0x298}, 0x6212A4, 0x621274, 0x6212A8, 0x6212A0,
    0x662EF0,
    0x5BBCD8, 0x2C74,                  // jz +0x2C
    0x583398,
    0x598B13,                          // jne +0x5E in the texture creation at 0x598B00
    0,
    0x508ADD, 0x501C60,
    0x8F1AD8, 0x709C50,
    {{0x87C788, 0}, {0x87C78C, 0x87028}}, // sound bank (static: not saved), ped speech buffer
    0x74B248, 0x8E2CA8, 0x8F1C18, 0x10, 0x439500, 0x88, true,
    0x580F00, 0x6510D8, 0,
    0x60E8C8,
    0x662EEC, {0x9431C8, 0x34},
    2, {0x87C5F8, 0x1C * 4}, 0x8F4404, 0x943170,
    0x882384, 0x880F73, 0x5C, 0xBD9,   // AudioManager 0x880F70 (+0x1414, +3); ProcessActiveQueues 0x57BDB0
    {0x7341E0, 0x880F70, 0x5679F0, 0x567D20, 0x567B70, 0x567D80, 0x567DF0, 0x567980, 0x567BE0, 0x567C20, 0x567ED0,
     0x57AFB0, 0x60A59C, 0x60A540, 0x60A5A0, 0, 0},
    0x8F1B68,                          // AUDIO\\SFX.RAW
    0x713FF8,
    4, 5, 7, 9,
    0x28000 - 0x100,                   // mission area 0x20000..0x28000 (largest mission 0x62D3)
    0x591CF0, 0x8F5EE0, 0x8F5BA0,      // restart path: CGame::InitialiseForRestart 0x48C840 -> GenericLoad 0x590CC0
    0x95CF4E, 0x8E2D14, 0x94057C, 0x407390,
    0,
    0x940904,                          // LoadPedComment 0x5674F0: slot * 79000 + buffer
};

// GTA Vice City JP (Capcom, 2004; exe wrapped by a protector). Addresses from a memory dump after start-up; the
// layout is close to Vice City 1.0 (most globals 0x2FF0..0x2FF8 lower). Same CodeWarrior runtime as GTA III 1.1.
static const Addrs3 kVCJP = {
    Game3::VC_JP, "GTA Vice City JP", true,
    0x680000, 0xA0E000,                // .asrc .data .CRT /0 .idata .tls _rwdseg _rwcseg .bss
    0x5FFB0D, 0x675C3A, 0,             // WinMain's call [PeekMessageA]; __sys_alloc. __sys_free calls GlobalFree
                                       // through a protector dispatcher (0x675C80), which must stay: GlobalFree is
                                       // hot-patched instead
    true,                              // the patched call site stops the protected WinMain after one frame
    0x9B2F18,
    0x866670,
    0x947D30,                          // CPlayerInfo 0x170 each
    0x9B1904, 0x28,
    0x971B34,
    0xA0A8A8,
    0xA0DB46, 0xA0DB87,
    {{0x7022F8, 0x1C}, {0xA0D690, 4}}, // CTimer::Update 0x4D0870
    0x6DBB18, 0x787098, 0x288, 0x6608A0, 0x6608D0,
    {0x6F4488, 0x298}, 0x6F46FC, 0x6F46CC, 0x6F4700, 0x6F46F8,   // CdStreamInit 0x408C80
    0x7867B0,
    0x65EDC8, 0x2C74,                  // jz +0x2C
    0x60043F,
    0x6275E5, 0x627680,                // texture creation 0x6275D0: square-texture cache, list of released textures
    0x55B90C, 0x550860,                // CHud::DrawAfterFade 0x557520: the pink "mission passed" message
    0x975558, 0x975670,
    {{0x948790, 0}, {0x948794, 0xB44E0}},
    0x81E288, 0x972340, 0x975750, 0x10, 0x44FF50, 0x88, false,
    0x601FE0, 0x780EF8, 0x601920,      // the savefile folder is computed again before SetSaveDirectory 0x61D640
    0x6D2D24,
    0x7867AC, {0xA0CD14, 0x34},
    3, {0x9481A8, 0x1C * 4}, 0x9B18D4, 0xA0CCAC,
    0x78BC10, 0x78A723, 0x60, 0x26D6,  // AudioManager 0x78A720 (+0x14F0, +3); ProcessActiveQueues 0x5F5CD0
    {0x980BA0, 0x78A720, 0x5D6560, 0x5D6330, 0x5D64E0, 0x5D62C0, 0x5D6260, 0x5D66D0, 0x5D64A0, 0x5D6470, 0x5D6180,
     0x5F7540, 0x6BBCF4, 0x6BBCF8, 0x6BBD04, 0x5538, 0x5536},
    0x97562C,                          // AUDIO\\SFX.RAW
    0x7F6F70,
    8, 9, 11, 13,
    0x3F9A0 - 0x100,                   // mission area 0x370E8..0x3F9A0 (largest mission 0x7E98)
    0x61A460, 0x866674, 0x866671,      // restart path: CGame::InitialiseForRestart 0x4A3F90 -> GenericLoad 0x61A4A0
    0xA0DB62, 0x9723CC, 0xA09EAC, 0x40F810,
    0xA0DAC2,                          // WM_ACTIVATE 0x601132 -> skip 0x405FA0 (reads FindPlayerPed()->flags)
    0xA0A218,                          // LoadPedComment 0x5D6AE0: slot * 0x19C20 + buffer
};

const Addrs3* g3 = nullptr;

const Addrs3* DetectGame3() {
    auto* dos = (IMAGE_DOS_HEADER*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)dos + dos->e_lfanew);
    if (*(uint32_t*)0x5C2135 == 0xB85548EC && nt->OptionalHeader.AddressOfEntryPoint == 0x1C2130) return &kIII11;
    if (nt->FileHeader.TimeDateStamp == 0x40758BC6 && nt->OptionalHeader.SizeOfImage == 0x810000) return &kVCJP;
    Log("unsupported exe (entry RVA 0x%X, stamp %08X); supported: GTA III 1.1, GTA Vice City JP", nt->OptionalHeader.AddressOfEntryPoint,
        nt->FileHeader.TimeDateStamp);
    return nullptr;
}
