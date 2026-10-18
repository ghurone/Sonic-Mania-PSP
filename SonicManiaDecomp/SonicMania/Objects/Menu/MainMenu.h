#ifndef OBJ_MAINMENU_H
#define OBJ_MAINMENU_H

#include "Game.h"

#if MANIA_USE_PLUS
// Object Class
struct ObjectMainMenu {
    RSDK_OBJECT
    EntityUIControl *menuControl;
    EntityUIButtonPrompt *confirmPrompt;
    EntityUIDiorama *diorama;
#if defined(__psp__)
    // the achievements button (before Exit) and its page (MainMenu_OpenAchievements): the PSP has no system overlay
    // listing them
    EntityUIButton *achievementsButton;
    EntityMainMenu *achievementsPage;
    EntityUIHeading *heading;
    Animator titleAnimator; // UIElements' medium letters (the Time Attack zone names')
    Animator nameAnimator;  // UIElements' small letters (bold capitals)
    Animator descAnimator;  // the dialogs' font
    String buttonLabel;
    String pageTitle;
    String pageCounter;
    String achievementNames[24];
    String achievementDescs[24];
    bool32 achievementUnlocked[24];
    int32 achievementSplit[24]; // where a description too wide for one line goes on to a second (0: it fits)
    int32 achievementSkip[24];  // the space it breaks at, left out (0 between Japanese/Chinese characters)
    int32 achievementNameSpacing[24];
    int32 achievementCount;
    bool32 achievementSmallNames; // Japanese/Chinese names: in the small font (the bold letters have no such characters)
    uint16 achievementFrames;     // UI/PSPAchievements.bin (tools/psp_achievements.py): the button label and heading
#endif
};

// Entity Class
struct EntityMainMenu {
    RSDK_ENTITY
#if defined(__psp__)
    // (the achievements page)
    int32 selection;
    int32 scroll; // the first card shown
    int32 bounceOffset, bounceVelocity;         // the selected card, as UIButton's
    int32 textBounceOffset, textBounceVelocity; // its text
    bool32 closing;
#endif
};

// Object Struct
extern ObjectMainMenu *MainMenu;

// Standard Entity Events
void MainMenu_Update(void);
void MainMenu_LateUpdate(void);
void MainMenu_StaticUpdate(void);
void MainMenu_Draw(void);
void MainMenu_Create(void *data);
void MainMenu_StageLoad(void);
#if GAME_INCLUDE_EDITOR
void MainMenu_EditorDraw(void);
void MainMenu_EditorLoad(void);
#endif
void MainMenu_Serialize(void);

// Extra Entity Functions
void MainMenu_Initialize(void);
bool32 MainMenu_BackPressCB_ReturnToTitle(void);
void MainMenu_ExitGame(void);
void MainMenu_ExitButton_ActionCB(void);
void MainMenu_StartExitGame(void);
void MainMenu_MenuButton_ActionCB(void);

#if MANIA_USE_EGS
void MainMenu_BuyPlusDialogCB(void);
#endif

void MainMenu_HandleUnlocks(void);
void MainMenu_SetupActions(void);
void MainMenu_MenuSetupCB(void);

#if defined(__psp__)
void MainMenu_SetupAchievementsButton(void);
void MainMenu_DrawAchievementsLabel(EntityUIButton *button, Vector2 *drawPos);
bool32 MainMenu_DrawAchievementsHeading(EntityUIHeading *heading);
void MainMenu_OpenAchievements(void);
void MainMenu_CloseAchievements(void);
void MainMenu_UpdateAchievementsPage(EntityMainMenu *page);
void MainMenu_DrawAchievementsPage(EntityMainMenu *page);
#endif
#endif

#endif //! OBJ_MAINMENU_H
