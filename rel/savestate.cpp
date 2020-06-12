#include "savestateold.h"
#include "pad.h"
#include "log.h"
#include "patch.h"
#include "memstore.h"

#include <mkb/mkb.h>

#include <cstring>

namespace savestate
{

struct SaveState
{
    bool active;
    memstore::MemStore memStore;
    uint8_t pauseMenuSpriteStatus;
    mkb::Sprite pauseMenuSprite;
};

static SaveState s_state = {};

static void (*s_setMinimapMode_trampoline)(uint32_t mode);

void init()
{
    // Hook setMinimapMode() to prevent the minimap from being hidden on goal/fallout
    // This way the minimap is unaffected when loading savestates after goal/fallout
    s_setMinimapMode_trampoline = patch::hookFunction(
        mkb::setMinimapMode, [](uint32_t mode)
        {
            if (!(mkb::mainMode == mkb::MD_GAME
                  && mkb::mainGameMode == mkb::MGM_PRACTICE
                  && mode == mkb::MINIMAP_SHRINK))
            {
                s_setMinimapMode_trampoline(mode);
            }
        });
}

// For all memory regions that involve just saving/loading to the same region...
// Do a pass over them. This may involve preallocating a buffer to save them in, actually saving them,
// or restoring them, depending on the mode `memStore` is in
static void passOverRegions(memstore::MemStore *memStore)
{
    memStore->doRegion(&mkb::balls[0], sizeof(mkb::balls[0]));
    memStore->doRegion(&mkb::subMode, sizeof(mkb::subMode));
    memStore->doRegion(&mkb::stageTimer, sizeof(mkb::stageTimer));
    memStore->doRegion(reinterpret_cast<void *>(0x8054E03C), 0xe0); // Camera region
    memStore->doRegion(reinterpret_cast<void *>(0x805BD830), 0x1c); // Some physics region
    memStore->doRegion(&mkb::balls[0].ape->charaRotation, sizeof(mkb::balls[0].ape->charaRotation));
    memStore->doRegion(&mkb::balls[0].ape->charaAnimType, sizeof(mkb::balls[0].ape->charaAnimType));
    memStore->doRegion(&mkb::ballMode, sizeof(mkb::ballMode));
    memStore->doRegion(&mkb::balls[0].ape->flag1, sizeof(mkb::balls[0].ape->flag1));
    memStore->doRegion(&mkb::mysteryStandstillFrameCounter, sizeof(mkb::mysteryStandstillFrameCounter));

    // Itemgroups
    memStore->doRegion(mkb::itemgroups, sizeof(mkb::Itemgroup) * mkb::stagedef->collisionHeaderCount);

    // Bananas
    memStore->doRegion(&mkb::items, sizeof(mkb::Item) * mkb::stagedef->bananaCount);

    // Goal tape, party ball, and button stage objects
    for (int i = 0; i < mkb::stobjListInfo.upperBound; i++)
    {
        if (mkb::stobjListInfo.statusList[i] == 0) continue;

        switch (mkb::stageObjects[i].type)
        {
            case mkb::STOBJ_GOALTAPE:
            case mkb::STOBJ_GOALBAG:
            case mkb::STOBJ_BUTTON:
            {
                memStore->doRegion(&mkb::stageObjects[i], sizeof(mkb::stageObjects[i]));
                break;
            }
        }
    }

    // Seesaws
    for (int i = 0; i < mkb::stagedef->collisionHeaderCount; i++)
    {
        if (mkb::stagedef->collisionHeaderList[i].animLoopTypeAndSeesaw == mkb::ANIM_SEESAW)
        {
            memStore->doRegion(&mkb::itemgroups[i].seesawInfo->state, 12);
        }
    }

    // Goal tape and party ball-specific extra data
    memStore->doRegion(mkb::goalTapes, sizeof(mkb::GoalTape) * mkb::stagedef->goalCount);
    memStore->doRegion(mkb::goalBags, sizeof(mkb::GoalBag) * mkb::stagedef->goalCount);

    // Pause menu
    memStore->doRegion(reinterpret_cast<void *>(0x8054DCA8), 56); // Pause menu state
    memStore->doRegion(reinterpret_cast<void *>(0x805BC474), 4); // Pause menu bitfield
}

static void handlePauseMenuSave(SaveState *state)
{
    state->pauseMenuSpriteStatus = 0;

    // Look for an active sprite that has the same dest func pointer as the pause menu sprite
    for (uint32_t i = 0; i < mkb::spriteListInfo.upperBound; i++)
    {
        if (mkb::spriteListInfo.statusList[i] == 0) continue;

        mkb::Sprite &sprite = mkb::sprites[i];
        if (sprite.dispFunc == mkb::pauseMenuSpriteDisp)
        {
            state->pauseMenuSpriteStatus = mkb::spriteListInfo.statusList[i];
            state->pauseMenuSprite = sprite;

            break;
        }
    }
}

static void handlePauseMenuLoad(SaveState *state)
{
    bool pausedNow = *reinterpret_cast<uint32_t *>(0x805BC474) & 8; // TODO actually give this a name
    bool pausedInState = state->pauseMenuSpriteStatus != 0;

    if (pausedNow && !pausedInState)
    {
        // Destroy the pause menu sprite that currently exists
        for (uint32_t i = 0; i < mkb::spriteListInfo.upperBound; i++)
        {
            if (mkb::spriteListInfo.statusList[i] == 0) continue;

            if (reinterpret_cast<uint32_t>(mkb::sprites[i].dispFunc) == 0x8032a4bc)
            {
                mkb::spriteListInfo.statusList[i] = 0;
                break;
            }
        }
    }
    else if (!pausedNow && pausedInState)
    {
        // Allocate a new pause menu sprite
        int i = mkb::tickableListAllocElem(&mkb::spriteListInfo, state->pauseMenuSpriteStatus);
        mkb::sprites[i] = state->pauseMenuSprite;
    }
}

static void clearPostGoalSprites()
{
    for (uint32_t i = 0; i < mkb::spriteListInfo.upperBound; i++)
    {
        if (mkb::spriteListInfo.statusList[i] == 0) continue;

        mkb::Sprite *sprite = &mkb::sprites[i];
        bool postGoalSprite = (
            sprite->dispFunc == mkb::goalSpriteDisp
            || sprite->dispFunc == mkb::clearScoreSpriteDisp
            || sprite->dispFunc == mkb::warpBonusSpriteDisp
            || sprite->dispFunc == mkb::timeBonusSpriteDisp
            || sprite->dispFunc == mkb::stageScoreSpriteDisp
            || sprite->tickFunc == mkb::falloutSpriteTick
            || sprite->tickFunc == mkb::bonusFinishSpriteTick);
        if (postGoalSprite) mkb::spriteListInfo.statusList[i] = 0;
    }
}

static void preventReplays()
{
    // Prevent replays from playing in goal and fallout submodes by locking initial submode frame counter
    switch (mkb::subMode)
    {
        case mkb::SMD_GAME_GOAL_MAIN:
        {
            // Just prevent the timer from running out completely, so that the GOAL sound plays
            if (mkb::subModeFrameCounter == 1) mkb::subModeFrameCounter = 2;
            break;
        }
        case mkb::SMD_GAME_RINGOUT_MAIN:
        {
            mkb::subModeFrameCounter = 270;
            break;
        }
        case mkb::SMD_GAME_TIMEOVER_MAIN:
        {
            mkb::subModeFrameCounter = 120;
            break;
        }
    }
}

void update()
{
    // Must be in main game
    if (mkb::mainMode != mkb::MD_GAME) return;

    // Must be in practice mode
    if (mkb::mainGameMode != mkb::MGM_PRACTICE) return;

    switch (mkb::subMode)
    {
        case mkb::SMD_GAME_PLAY_MAIN:
        case mkb::SMD_GAME_GOAL_INIT:
        case mkb::SMD_GAME_GOAL_MAIN:
        case mkb::SMD_GAME_RINGOUT_INIT:
        case mkb::SMD_GAME_RINGOUT_MAIN:
        case mkb::SMD_GAME_TIMEOVER_INIT:
        case mkb::SMD_GAME_TIMEOVER_MAIN:
            break;
        default:
            return;
    }

    preventReplays();

    // Only allow creating state while the timer is running
    if (pad::buttonPressed(pad::PAD_BUTTON_X) && mkb::subMode == mkb::SMD_GAME_PLAY_MAIN)
    {
        s_state.active = true;
        s_state.memStore.enterPreallocMode();
        passOverRegions(&s_state.memStore);
        MOD_ASSERT(s_state.memStore.enterSaveMode());
        passOverRegions(&s_state.memStore);

        handlePauseMenuSave(&s_state);

        gc::OSReport("[mod] Saved state:\n");
        s_state.memStore.printStats();
    }
    else if (
        s_state.active && (
            (pad::buttonDown(pad::PAD_BUTTON_Y)
             || (pad::buttonDown(pad::PAD_BUTTON_X)
                 && !pad::buttonPressed(pad::PAD_BUTTON_X)))))
    {
        // Need to handle pausemenu-specific loading first so we can detect the game isn't currently paused
        handlePauseMenuLoad(&s_state);

        s_state.memStore.enterLoadMode();
        passOverRegions(&s_state.memStore);

        clearPostGoalSprites();
    }
}

}