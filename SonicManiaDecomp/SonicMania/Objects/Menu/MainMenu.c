// ---------------------------------------------------------------------
// RSDK Project: Sonic Mania
// Object Description: MainMenu Object
// Object Author: Christian Whitehead/Simon Thomley/Hunter Bridges
// Decompiled by: Rubberduckycooly & RMGRich
// ---------------------------------------------------------------------

#include "Game.h"
#if defined(__psp__)
#include "PSPAchievementText.h"
#endif

#if MANIA_USE_PLUS
ObjectMainMenu *MainMenu;

void MainMenu_Update(void)
{
#if defined(__psp__)
    // (only the achievements page is ever created as an entity)
    RSDK_THIS(MainMenu);
    if (MainMenu->achievementsPage == self)
        MainMenu_UpdateAchievementsPage(self);
#endif
}

void MainMenu_LateUpdate(void) {}

void MainMenu_StaticUpdate(void)
{
    EntityUIControl *control = MainMenu->menuControl;

    if (control && control->active) {
        EntityUIDiorama *diorama         = MainMenu->diorama;
        MainMenu->confirmPrompt->visible = ((int32)(control->shifter->shiftOffset.y & 0xFFFF0000)) > -0x700000;
#if defined(__psp__)
        if (MainMenu->achievementsPage)
            MainMenu->confirmPrompt->visible = false;
#endif
        EntityUIButton *button           = control->buttons[control->lastButtonID];

        if (button) {
            switch (button->frameID) {
                case 0: diorama->dioramaID = UIDIORAMA_MANIAMODE; break;
                case 1: diorama->dioramaID = UIDIORAMA_TIMEATTACK; break;
                case 2: diorama->dioramaID = UIDIORAMA_COMPETITION; break;
                case 3: diorama->dioramaID = UIDIORAMA_OPTIONS; break;
                case 4: diorama->dioramaID = UIDIORAMA_EXTRAS; break;
                case 5: diorama->dioramaID = UIDIORAMA_ENCOREMODE; break;
                case 6: diorama->dioramaID = UIDIORAMA_PLUSUPSELL; break;
                case 7: diorama->dioramaID = UIDIORAMA_EXIT; break;
                default: break;
            }

            if (button->disabled)
                diorama->timer = 12;
        }
    }
}

void MainMenu_Draw(void)
{
#if defined(__psp__)
    RSDK_THIS(MainMenu);
    if (MainMenu->achievementsPage == self)
        MainMenu_DrawAchievementsPage(self);
#endif
}

void MainMenu_Create(void *data)
{
#if defined(__psp__)
    RSDK_THIS(MainMenu);

    // the achievements page (MainMenu_OpenAchievements); nothing else is ever created
    if (!SceneInfo->inEditor && data) {
        self->active    = ACTIVE_ALWAYS;
        self->visible   = true;
        self->drawGroup = 2;
    }
#endif
}

void MainMenu_StageLoad(void)
{
#if defined(__psp__)
    // the achievements button's and page's words in each language, in the menu's lettering (tools/psp_achievements.py;
    // a Data.psp built before it has none: the bold letters stand in)
    MainMenu->achievementFrames = RSDK.LoadSpriteAnimation("UI/PSPAchievements.bin", SCOPE_STAGE);
#endif
}

void MainMenu_Initialize(void)
{
    String tag;
    INIT_STRING(tag);
    RSDK.SetString(&tag, "Main Menu");

    foreach_all(UIControl, control)
    {
        if (RSDK.CompareStrings(&tag, &control->tag, false)) {
            MainMenu->menuControl = control;
            control->backPressCB  = MainMenu_BackPressCB_ReturnToTitle;
        }
    }

    EntityUIControl *menuControl = MainMenu->menuControl;

    foreach_all(UIButtonPrompt, prompt)
    {
        if (UIControl_ContainsPos(menuControl, &prompt->position) && prompt->buttonID == 0)
            MainMenu->confirmPrompt = prompt;
    }

    foreach_all(UIDiorama, diorama)
    {
        if (UIControl_ContainsPos(menuControl, &diorama->position)) {
            MainMenu->diorama = diorama;
            diorama->parent   = menuControl;
        }
    }

#if MANIA_USE_PLUS
    int32 button1Frame = 1; // Time Attack
    int32 button2Frame = 2; // Competition
    int32 button3Frame = 3; // Options
    int32 button4Frame = 4; // Extras
    int32 button5Frame = 6; // Buy Plus

    bool32 button3StopMus    = false; // Options button does NOT stop music
    bool32 button5Transition = false; // Buy Plus Does NOT do a transition

    if (API.CheckDLC(DLC_PLUS)) {
        button1Frame = 5; // Encore Mode
        button2Frame = 1; // Time Attack
        button3Frame = 2; // Competition
        button4Frame = 3; // Options
        button5Frame = 4; // Extras

        button3StopMus    = true; // Competition button stops music
        button5Transition = true; // Extras Does a transition
    }

    EntityUIButton *buttonManiaMode = menuControl->buttons[0];
    buttonManiaMode->frameID        = 0;
    buttonManiaMode->transition     = true;
    buttonManiaMode->stopMusic      = true;

    EntityUIButton *buttonEncoreMode = menuControl->buttons[1];
    buttonEncoreMode->frameID        = button1Frame;
    buttonEncoreMode->transition     = true;
    buttonEncoreMode->stopMusic      = true;

    EntityUIButton *buttonTimeAttack = menuControl->buttons[2];
    buttonTimeAttack->frameID        = button2Frame;
    buttonTimeAttack->transition     = true;
    buttonTimeAttack->stopMusic      = true;

    EntityUIButton *buttonCompetition = menuControl->buttons[3];
    buttonCompetition->frameID        = button3Frame;
    buttonCompetition->transition     = true;
    buttonCompetition->stopMusic      = button3StopMus;

    EntityUIButton *buttonOptions = menuControl->buttons[4];
    buttonOptions->frameID        = button4Frame;
    buttonOptions->transition     = true;
    buttonOptions->stopMusic      = false;

    EntityUIButton *buttonExtras = menuControl->buttons[5];
    buttonExtras->frameID        = button5Frame;
    buttonExtras->transition     = button5Transition;
    buttonExtras->stopMusic      = false;

    EntityUIButton *buttonExit = menuControl->buttons[6];
    buttonExit->frameID        = 7;
    buttonExit->transition     = false;
    buttonExit->stopMusic      = false;
#endif

#if defined(__psp__)
    MainMenu_SetupAchievementsButton();
#endif
}

bool32 MainMenu_BackPressCB_ReturnToTitle(void)
{
#if defined(__psp__)
    // (back on the achievements page is the page's: MainMenu_UpdateAchievementsPage)
    if (MainMenu->achievementsPage)
        return false;
#endif
    ManiaModeMenu_StartReturnToTitle();

    return true;
}

void MainMenu_ExitGame(void) { API.ExitGame(); }

void MainMenu_ExitButton_ActionCB(void)
{
    String msg;
    Localization_GetString(&msg, STR_QUITWARNING);

    UIDialog_CreateDialogYesNo(&msg, MainMenu_StartExitGame, StateMachine_None, true, true);
}

void MainMenu_StartExitGame(void)
{
    if (UIControl_GetUIControl())
        UIControl_GetUIControl()->state = StateMachine_None;

    Music_FadeOut(0.02);

    MenuSetup_StartTransition(MainMenu_ExitGame, 64);
}

void MainMenu_MenuButton_ActionCB(void)
{
    RSDK_THIS(UIButton);

    switch (self->frameID) {
        case 0: // Mania Mode
            if (API_GetNoSave()) {
                UIControl_MatchMenuTag("No Save Mode");
            }
            else {
                EntityUIControl *saveSelect = ManiaModeMenu->saveSelectMenu;
                saveSelect->buttonID        = 7;
#if MANIA_USE_PLUS
                saveSelect->menuWasSetup           = false;
                ManiaModeMenu->saveSelLastButtonID = -1;

                for (int32 i = 0; i < saveSelect->buttonCount; ++i) {
                    Entity *store     = SceneInfo->entity;
                    SceneInfo->entity = (Entity *)saveSelect->buttons[i];
                    UISaveSlot_HandleSaveIconChange();
                    SceneInfo->entity = store;
                }
#endif
                UIControl_MatchMenuTag("Save Select");
            }
            break;

        case 1: // Time Attack
            if (API.CheckDLC(DLC_PLUS)) {
                EntityUIControl *control = TimeAttackMenu->timeAttackControl;
                control->buttonID        = 0;
                control->menuWasSetup    = false;
                UIControl_MatchMenuTag("Time Attack");
            }
            else {
                EntityUIControl *control = TimeAttackMenu->timeAttackControl_Legacy;
                control->buttonID        = 0;
                control->menuWasSetup    = false;
                UIControl_MatchMenuTag("Time Attack Legacy");
            }
            break;

        case 2: // Competition
            if (API.CheckDLC(DLC_PLUS))
                UIControl_MatchMenuTag("Competition");
            else
                UIControl_MatchMenuTag("Competition Legacy");
            break;

        case 3: // Options
            UIControl_MatchMenuTag("Options");
            break;

        case 4: // Extras
            UIControl_MatchMenuTag("Extras");
            break;

        case 5: // Encore
            if (API_GetNoSave()) {
                UIControl_MatchMenuTag("No Save Encore");
            }
            else {
                EntityUIControl *encoreSaveSel = ManiaModeMenu->encoreSaveSelect;
                encoreSaveSel->buttonID        = 1;
                encoreSaveSel->menuWasSetup    = false;
                for (int32 i = 0; i < encoreSaveSel->buttonCount; ++i) {
                    Entity *store     = SceneInfo->entity;
                    SceneInfo->entity = (Entity *)encoreSaveSel->buttons[i];
                    UISaveSlot_HandleSaveIconChange();
                    SceneInfo->entity = store;
                }
                UIControl_MatchMenuTag("Encore Mode");
            }
            break;

        case 6: // Buy Plus DLC
#if MANIA_USE_EGS
            API.SetupExtensionOverlay();

            if (!API.CheckDLC(DLC_PLUS)) {
                if (!API.CanShowExtensionOverlay(0) || !API.ShowExtensionOverlay(0)) {
                    if (API.CanShowAltExtensionOverlay(0)) {
                        int32 connectingMessage = API.GetConnectingStringID();
                        if (connectingMessage >= 0) {
                            String message;
                            INIT_STRING(message);
                            Localization_GetString(&message, connectingMessage);

                            UIDialog_CreateDialogOkCancel(&message, MainMenu_BuyPlusDialogCB, StateMachine_None, true, true);
                        }
                        else {
                            API.ShowAltExtensionOverlay(0);
                        }
                    }
                }
            }
#else
            API.ShowExtensionOverlay(0);
#endif
            break;

        default: break;
    }
}

#if MANIA_USE_EGS
void MainMenu_BuyPlusDialogCB(void) { API.ShowAltExtensionOverlay(0); }
#endif

void MainMenu_HandleUnlocks(void)
{
    EntityUIControl *control = MainMenu->menuControl;

    EntityUIButton *taButton = API.CheckDLC(DLC_PLUS) ? control->buttons[2] : control->buttons[1];
    taButton->disabled       = !GameProgress_CheckUnlock(GAMEPROGRESS_UNLOCK_TIMEATTACK);

    EntityUIButton *compButton = API.CheckDLC(DLC_PLUS) ? control->buttons[3] : control->buttons[2];
    compButton->disabled       = !GameProgress_CheckUnlock(GAMEPROGRESS_UNLOCK_COMPETITION);
}

void MainMenu_SetupActions(void)
{
    foreach_all(UIButton, button)
    {
        if (button->listID == 1) {
            if (button->frameID == 7) {
                if (sku_platform != PLATFORM_PC && sku_platform != PLATFORM_DEV) {
                    EntityUIControl *control = MainMenu->menuControl;

                    // Remove "Exit" Button if not on PC or DEV platform
                    destroyEntity(button);
                    --control->buttonCount;
                    --control->rowCount;
                    control->buttons[6] = NULL;
                }
                else {
                    button->actionCB = MainMenu_ExitButton_ActionCB;
                }
            }
            else {
                button->actionCB = MainMenu_MenuButton_ActionCB;
            }
        }
    }

    MainMenu->menuControl->menuSetupCB = MainMenu_MenuSetupCB;

#if defined(__psp__)
    // (after the loop above, which gave it the menu buttons' action for its frameID)
    if (MainMenu->achievementsButton)
        MainMenu->achievementsButton->actionCB = MainMenu_OpenAchievements;
#endif
}

void MainMenu_MenuSetupCB(void) { MainMenu->diorama->lastDioramaID = -1; }

#if defined(__psp__)
// ---------------------------------------------------------------------
// Achievements. The PSP has no system overlay listing them, so the main menu gets an "ACHIEVEMENTS" button between
// Extras and Exit, opening a page of its own through the menus' usual transition: the heading bar, then a card per
// achievement (a gold medal once unlocked), scrolled along with the selection.
// ---------------------------------------------------------------------
int32 PSPGetAchievement(int32 id, const char **name, const char **description); // (the engine, UserAchievements.cpp)

#define MAINMENU_ACH_LINE_H (11)
#define MAINMENU_ACH_GAP    (5) // between cards
#define MAINMENU_ACH_SEP    (7) // between a card's name and its description (a rule across the middle)
#define MAINMENU_ACH_CARD_H (5 + MAINMENU_ACH_LINE_H + MAINMENU_ACH_SEP + 2 * MAINMENU_ACH_LINE_H + 5) // every card: room for 2 lines

// (screen pixels)
static int32 MainMenu_AchCardWidth(void) { return ScreenInfo->size.x - 140; }
static int32 MainMenu_AchTextWidth(void) { return MainMenu_AchCardWidth() - 42; }
static int32 MainMenu_AchCardHeight(int32 id) { return MAINMENU_ACH_CARD_H; }
static int32 MainMenu_AchListTop(void)
{
    // below the heading bar (which reaches 24 pixels below the heading's position)
    if (MainMenu->heading)
        return (MainMenu->heading->position.y >> 16) - ScreenInfo->position.y + 24 + 6;
    return 56;
}
static int32 MainMenu_AchListBottom(void) { return ScreenInfo->size.y - 4; }

// Text centred on x (DrawText's ALIGN_CENTER ends it there instead)
static void MainMenu_AchDrawCentred(Animator *animator, uint16 aniFrames, uint16 listID, String *text, int32 x, int32 y)
{
    Vector2 drawPos;
    drawPos.x = x - TO_FIXED(RSDK.GetStringWidth(aniFrames, listID, text, 0, text->length, 0)) / 2;
    drawPos.y = y;
    RSDK.DrawText(animator, &drawPos, text, 0, text->length, ALIGN_LEFT, 0, INT_TO_VOID(2), NULL, false);
}

static int32 MainMenu_AchLanguage(void)
{
    int32 language = Localization->language;
    return language >= 0 && language < (int32)(sizeof(pspAchTextTable) / sizeof(pspAchTextTable[0])) ? language : LANGUAGE_EN;
}

// UTF-8 into a String (the engine's SetString miscounts text whose characters vary in size). capitals: as the bold
// letters can draw it (capitals, and plain letters for the few accented ones they lack). Returns its length; codes gets
// the characters.
static int32 MainMenu_AchSetText(String *string, const char *text, bool32 capitals, uint16 *codes, int32 max)
{
    int32 length = 0;
    for (const uint8 *t = (const uint8 *)text; *t && length < max - 1;) {
        uint32 c = *t++;
        if (c >= 0xE0 && t[0] && t[1]) {
            c = ((c & 0x0F) << 12) | ((t[0] & 0x3F) << 6) | (t[1] & 0x3F);
            t += 2;
        }
        else if (c >= 0xC0 && t[0]) {
            c = ((c & 0x1F) << 6) | (t[0] & 0x3F);
            t += 1;
        }
        if (capitals) {
            if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
                c -= 0x20;
            switch (c) {
                case 0xDF: codes[length++] = 'S'; c = 'S'; break; // ß
                case 0xC7: c = 'C'; break;
                case 0xD1: c = 'N'; break;
                case 0xDA:
                case 0xDB: c = 'U'; break;
                case 0xD2:
                case 0xD4: c = 'O'; break;
                case 0xCC:
                case 0xCE:
                case 0xCF: c = 'I'; break;
                case 0xC2:
                case 0xC3: c = 'A'; break; // (Ã)
                case 0xD5: c = 'O'; break; // (Õ)
                case 0xCB: c = 'E'; break;
                default: break;
            }
        }
        codes[length++] = (uint16)c;
    }

    char room[0x100]; // (SetString allocates the string, from as many characters)
    memset(room, 'x', length);
    room[length] = 0;
    RSDK.SetString(string, room);
    if (length)
        memcpy(string->chars, codes, length * sizeof(uint16));
    string->length = length;
    return length;
}

// The last card wholly on screen when the list starts at card first
static int32 MainMenu_AchLastShown(int32 first)
{
    int32 y = MainMenu_AchListTop(), bottom = MainMenu_AchListBottom(), id = first;
    while (id < MainMenu->achievementCount && y + MainMenu_AchCardHeight(id) <= bottom) {
        y += MainMenu_AchCardHeight(id) + MAINMENU_ACH_GAP;
        ++id;
    }
    return id - 1;
}

// An "ACHIEVEMENTS" button between Extras and Exit: a copy of Exit put in its place, Exit moving one down (UIShifter lays
// the buttons out by their order in the control). Its label is drawn as text (MainMenu_DrawAchievementsLabel): the menu
// labels are pictures, and there is none for it.
void MainMenu_SetupAchievementsButton(void)
{
    EntityUIControl *control     = MainMenu->menuControl;
    MainMenu->achievementsButton = NULL;
    MainMenu->achievementsPage   = NULL;
    MainMenu->heading            = NULL;
    // (optional in Data.psp: tools/build_psp_data.py --no-achievements leaves the menu as it was)
    if (!control || control->buttonCount != 7 || !control->buttons[6] || MainMenu->achievementFrames == (uint16)-1)
        return;

    foreach_all(UIHeading, heading)
    {
        if (UIControl_ContainsPos(control, &heading->position))
            MainMenu->heading = heading;
    }

    EntityUIButton *exitButton = control->buttons[6];
    EntityUIButton *button     = CREATE_ENTITY(UIButton, NULL, exitButton->position.x, exitButton->position.y);
    RSDK.CopyEntity(button, exitButton, false);
    button->isPermanent      = true; // (a created entity: keep later ones from taking its slot)
    button->frameID          = 4;    // Extras': the diorama shown while it is selected
    button->transition       = true; // the menus' transition, to the achievements page
    button->stopMusic        = false;
    button->clearParentState = false;
    // widened as far as its widest label needs (the others: 150 pixels at most, Competition's)
    int32 widest = 170;
    if (MainMenu->achievementFrames != (uint16)-1) {
        widest = 0;
        for (int32 l = 0; l < (int32)(sizeof(pspAchTextTable) / sizeof(pspAchTextTable[0])); ++l) {
            SpriteFrame *frame = RSDK.GetFrame(MainMenu->achievementFrames, 2 * l, 0);
            if (frame && frame->width > widest)
                widest = frame->width;
        }
    }
    if (widest > 150)
        button->size.x += TO_FIXED(widest - 150);

    control->buttons[6] = button;
    control->buttons[7] = exitButton;
    control->buttonCount++;
    control->rowCount++;
    MainMenu->achievementsButton = button;

    RSDK.SetSpriteAnimation(UIWidgets->uiFrames, 3, &MainMenu->titleAnimator, true, 0);
    RSDK.SetSpriteAnimation(UIWidgets->uiFrames, 4, &MainMenu->nameAnimator, true, 0);
    RSDK.SetSpriteAnimation(UIWidgets->fontFrames, 0, &MainMenu->descAnimator, true, 0);
    RSDK.SetString(&MainMenu->buttonLabel, "ACHIEVEMENTS");
    RSDK.SetSpriteString(UIWidgets->uiFrames, 3, &MainMenu->buttonLabel);
}

// The label in the other buttons' own lettering, in the language chosen: UI/PSPAchievements has it (put together from the
// menu's labels by tools/psp_achievements.py). From a Data.psp without it: in English, each letter cut from TextEN's
// labels (columns found by each letter's 1-pixel outline) and drawn through Mania Mode's frame, rect and pivot changed
// for the letter and put back after; in the other languages the Time Attack zone-name letters stand in.
void MainMenu_DrawAchievementsLabel(EntityUIButton *button, Vector2 *drawPos)
{
    if (MainMenu->achievementFrames != (uint16)-1) {
        Animator label;
        memset(&label, 0, sizeof(label));
        RSDK.SetSpriteAnimation(MainMenu->achievementFrames, 2 * MainMenu_AchLanguage(), &label, true, 0);
        RSDK.DrawSprite(&label, drawPos, false);
        return;
    }

    static const struct {
        char c;
        int16 x, y, width;
    } letters[] = {
        { 'A', 21, 1, 14 },   // MANIA MODE
        { 'C', 1, 49, 14 },   // COMPETITION
        { 'H', 176, 371, 14 }, // DEUTSCH
        { 'I', 53, 1, 6 },    // MANIA MODE
        { 'E', 251, 1, 14 },  // EXTRAS
        { 'V', 1, 119, 14 },  // VIDEO
        { 'M', 1, 1, 20 },    // MANIA MODE
        { 'N', 35, 1, 18 },   // MANIA MODE
        { 'T', 281, 1, 14 },  // EXTRAS
        { 'S', 323, 1, 14 },  // EXTRAS
    };

    SpriteFrame *frame = Localization->language == LANGUAGE_EN ? RSDK.GetFrame(UIWidgets->textFrames, 1, 0) : NULL;
    if (!frame || frame->height != 22) {
        int32 align = ALIGN_CENTER;
        if (button->align == UIBUTTON_ALIGN_LEFT)
            align = ALIGN_LEFT;
        else if (button->align == UIBUTTON_ALIGN_RIGHT)
            align = ALIGN_RIGHT;
        RSDK.DrawText(&MainMenu->titleAnimator, drawPos, &MainMenu->buttonLabel, 0, MainMenu->buttonLabel.length, align, 0, INT_TO_VOID(2), NULL,
                      false);
        return;
    }

    Animator animator;
    memset(&animator, 0, sizeof(animator));
    RSDK.SetSpriteAnimation(UIWidgets->textFrames, 1, &animator, true, 0);

    SpriteFrame saved = *frame;
    const char *word  = "ACHIEVEMENTS";
    int32 x           = 0;
    for (int32 i = 0; word[i]; ++i) {
        for (int32 l = 0; l < (int32)(sizeof(letters) / sizeof(letters[0])); ++l) {
            if (letters[l].c != word[i])
                continue;
            frame->sprX   = letters[l].x;
            frame->sprY   = letters[l].y;
            frame->width  = letters[l].width;
            frame->pivotX = saved.pivotX + x;
            RSDK.DrawSprite(&animator, drawPos, false);
            x += letters[l].width;
            break;
        }
    }
    frame->sprX   = saved.sprX;
    frame->sprY   = saved.sprY;
    frame->width  = saved.width;
    frame->pivotX = saved.pivotX;
}

// On the page, the main menu's heading reads "ACHIEVEMENTS" on its bar (UIHeading_Draw); false: not on the page
bool32 MainMenu_DrawAchievementsHeading(EntityUIHeading *heading)
{
    if (!MainMenu->achievementsPage || heading != MainMenu->heading)
        return false;

    RSDK.DrawSprite(&heading->headingAnimator, NULL, false);
    if (MainMenu->achievementFrames != (uint16)-1) {
        Animator title; // yellow, as the other headings (tools/psp_achievements.py)
        memset(&title, 0, sizeof(title));
        RSDK.SetSpriteAnimation(MainMenu->achievementFrames, 2 * MainMenu_AchLanguage() + 1, &title, true, 0);
        RSDK.DrawSprite(&title, NULL, false);
    }
    else {
        MainMenu_AchDrawCentred(&MainMenu->titleAnimator, UIWidgets->uiFrames, 3, &MainMenu->pageTitle, heading->position.x,
                                heading->position.y + TO_FIXED(8));
    }
    return true;
}

static void MainMenu_ShowMainMenu(bool32 show)
{
    EntityUIControl *control = MainMenu->menuControl;
    for (int32 i = 0; i < control->buttonCount; ++i) {
        if (control->buttons[i])
            control->buttons[i]->visible = show;
    }
    if (MainMenu->diorama)
        MainMenu->diorama->visible = show;
}

// Halfway through the transition: the page replaces the menu (the list as it stands now)
void MainMenu_OpenAchievements(void)
{
    if (MainMenu->achievementsPage)
        return;

    // the texts in the language chosen (PSPAchievementText.h), the engine's English for any past the table's
    int32 language = MainMenu_AchLanguage(), table = pspAchTextTable[language];
    bool32 capitals                 = true; // the names are always the English ones (as the platforms show them), in the bold capitals
    MainMenu->achievementSmallNames = !capitals;

    int32 count = 0, unlocked = 0, avail = MainMenu_AchTextWidth();
    const char *name, *desc;
    uint16 codes[0x100];
    for (int32 got; count < 24 && (got = PSPGetAchievement(count, &name, &desc)) >= 0; ++count) {
        if (count < PSP_ACH_TEXT_COUNT) {
            name = pspAchText[PSP_ACH_TEXT_EN][count][0]; // (the user's choice: names in English, descriptions translated)
            desc = pspAchText[table][count][1];
        }

        // the name, a little closer set when that's what it takes to fit
        String *nameText = &MainMenu->achievementNames[count];
        MainMenu_AchSetText(nameText, name, capitals, codes, 0x100);
        uint16 nameFrames = capitals ? UIWidgets->uiFrames : UIWidgets->fontFrames;
        uint16 nameList   = capitals ? 4 : 0;
        RSDK.SetSpriteString(nameFrames, nameList, nameText);
        MainMenu->achievementNameSpacing[count] = 0;
        if (RSDK.GetStringWidth(nameFrames, nameList, nameText, 0, nameText->length, 0) > avail)
            MainMenu->achievementNameSpacing[count] = -1;

        // the description: too wide, it goes on to a second line from the last space (or, in Japanese and Chinese, the
        // last character) that leaves the first one fitting
        String *descText = &MainMenu->achievementDescs[count];
        int32 length     = MainMenu_AchSetText(descText, desc, false, codes, 0x100);
        RSDK.SetSpriteString(UIWidgets->fontFrames, 0, descText);
        int32 split = 0, skip = 0;
        if (RSDK.GetStringWidth(UIWidgets->fontFrames, 0, descText, 0, descText->length, 0) > avail) {
            for (int32 c = 1; c < length; ++c) {
                bool32 space = codes[c] == ' ';
                // (not before closing punctuation)
                bool32 between = (codes[c] >= 0x2E80 || codes[c - 1] >= 0x2E80) && codes[c] != 0x3001 && codes[c] != 0x3002
                                 && codes[c] != 0xFF0C && codes[c] != 0xFF01 && codes[c] != 0xFF1F && codes[c] != 0x300D;
                if ((space || between) && RSDK.GetStringWidth(UIWidgets->fontFrames, 0, descText, 0, c, 0) <= avail) {
                    split = c;
                    skip  = space;
                }
            }
        }
        MainMenu->achievementSplit[count]    = split;
        MainMenu->achievementSkip[count]     = skip;
        MainMenu->achievementUnlocked[count] = got;
        unlocked += got;
    }
    MainMenu->achievementCount = count;

    char counter[32];
    sprintf(counter, "%d %s %d", (int)unlocked, pspAchCounterWord[language], (int)count);
    RSDK.SetString(&MainMenu->pageCounter, counter);
    RSDK.SetSpriteString(UIWidgets->uiFrames, 4, &MainMenu->pageCounter);
    RSDK.SetString(&MainMenu->pageTitle, "ACHIEVEMENTS");
    RSDK.SetSpriteString(UIWidgets->uiFrames, 3, &MainMenu->pageTitle);

    MainMenu_ShowMainMenu(false);
    MainMenu->menuControl->selectionDisabled = true; // its input flags (UIControl->any*Press) drive the page
    EntityMainMenu *page                     = CREATE_ENTITY(MainMenu, INT_TO_VOID(true), 0, 0);
    page->selection                          = 0;
    page->scroll                             = 0;
    page->bounceVelocity                     = -0x20000;
    page->textBounceVelocity                 = -0x20000;
    MainMenu->achievementsPage               = page;
}

// Halfway through the transition back: the menu as it was
void MainMenu_CloseAchievements(void)
{
    if (MainMenu->achievementsPage)
        destroyEntity(MainMenu->achievementsPage);
    MainMenu->achievementsPage = NULL;
    MainMenu_ShowMainMenu(true);

    EntityUIControl *control   = MainMenu->menuControl;
    control->selectionDisabled = false;
    control->backoutTimer      = 10;
}

static void MainMenu_AchSelect(EntityMainMenu *page, int32 id)
{
    page->selection = id;
    if (id < page->scroll)
        page->scroll = id;
    while (id > MainMenu_AchLastShown(page->scroll)) ++page->scroll;

    page->bounceOffset       = 0;
    page->bounceVelocity     = -0x20000;
    page->textBounceOffset   = 0;
    page->textBounceVelocity = -0x20000;
    RSDK.PlaySfx(UIWidgets->sfxBleep, false, 255);
}

void MainMenu_UpdateAchievementsPage(EntityMainMenu *page)
{
    // the selected card's bounce, as UIButton_State_HandleButtonEnter
    page->textBounceVelocity += 0x4000;
    page->textBounceOffset += page->textBounceVelocity;
    if (page->textBounceOffset >= 0 && page->textBounceVelocity > 0) {
        page->textBounceOffset   = 0;
        page->textBounceVelocity = 0;
    }
    page->bounceVelocity += 0x4800;
    page->bounceOffset += page->bounceVelocity;
    if (page->bounceOffset >= -0x20000 && page->bounceVelocity > 0) {
        page->bounceOffset   = -0x20000;
        page->bounceVelocity = 0;
    }

    int32 count = MainMenu->achievementCount;
    if (page->closing || !count)
        return;

    // the menu underneath keeps ignoring its buttons (the transition that opened the page let it take them again)
    MainMenu->menuControl->selectionDisabled = true;

    if (UIControl->anyBackPress) {
        page->closing = true;
        RSDK.PlaySfx(UIWidgets->sfxBleep, false, 255);
        UITransition_StartTransition(MainMenu_CloseAchievements, 0);
    }
    else if (UIControl->anyUpPress) {
        MainMenu_AchSelect(page, page->selection > 0 ? page->selection - 1 : count - 1);
    }
    else if (UIControl->anyDownPress) {
        MainMenu_AchSelect(page, page->selection + 1 < count ? page->selection + 1 : 0);
    }
}

void MainMenu_DrawAchievementsPage(EntityMainMenu *page)
{
    // drawn in world positions, like the menu's own widgets (the camera stays put while the page is up)
    int32 camX = ScreenInfo->position.x, camY = ScreenInfo->position.y;
    int32 w = ScreenInfo->size.x, h = ScreenInfo->size.y;
    int32 cardW = MainMenu_AchCardWidth(), cx = w / 2;
    Vector2 drawPos;

    page->inkEffect = INK_NONE;

    // the count, on a tag in the top right corner (where the menu's confirm prompt was)
    int32 counterW = RSDK.GetStringWidth(UIWidgets->uiFrames, 4, &MainMenu->pageCounter, 0, MainMenu->pageCounter.length, 0);
    int32 tagX = w - 16 - counterW / 2 - 8, tagY = 20;
    UIWidgets_DrawParallelogram(TO_FIXED(camX + tagX - 2), TO_FIXED(camY + tagY - 2), counterW + 12, 16, 16, (UIWidgets->buttonColor >> 16) & 0xFF,
                                (UIWidgets->buttonColor >> 8) & 0xFF, UIWidgets->buttonColor & 0xFF);
    UIWidgets_DrawParallelogram(TO_FIXED(camX + tagX), TO_FIXED(camY + tagY), counterW + 12, 16, 16, 0x00, 0x00, 0x00);
    MainMenu_AchDrawCentred(&MainMenu->nameAnimator, UIWidgets->uiFrames, 4, &MainMenu->pageCounter, TO_FIXED(camX + tagX + 4), TO_FIXED(camY + tagY));

    int32 top = MainMenu_AchListTop(), last = MainMenu_AchLastShown(page->scroll);
    for (int32 id = page->scroll, y = top; id <= last; y += MainMenu_AchCardHeight(id) + MAINMENU_ACH_GAP, ++id) {
        int32 cardH    = MainMenu_AchCardHeight(id);
        bool32 got     = MainMenu->achievementUnlocked[id];
        bool32 current = id == page->selection;
        int32 bounce   = current ? page->bounceOffset : 0;
        int32 textBounce = current ? page->textBounceOffset : 0;
        int32 cardX = TO_FIXED(camX + cx), cardY = TO_FIXED(camY + y + cardH / 2);

        // as UIButton: a coloured shadow, the black card offset from it by the selection's bounce
        if (current)
            UIWidgets_DrawParallelogram(cardX - bounce, cardY - bounce, cardW, cardH, cardH, (UIWidgets->buttonColor >> 16) & 0xFF,
                                        (UIWidgets->buttonColor >> 8) & 0xFF, UIWidgets->buttonColor & 0xFF);
        UIWidgets_DrawParallelogram(cardX + bounce, cardY + bounce, cardW, cardH, cardH, 0x00, 0x00, 0x00);

        // the medal: gold once unlocked, an empty grey ring until then
        int32 left   = cx - cardW / 2 + (bounce >> 16);
        int32 medalX = left + 13, medalY = y + cardH / 2 + (bounce >> 16);
        if (got) {
            RSDK.DrawCircle(medalX, medalY, 11, 0xB07800, 0xFF, INK_NONE, true);
            RSDK.DrawCircle(medalX, medalY, 9, 0xF0C020, 0xFF, INK_NONE, true);
            RSDK.DrawCircle(medalX - 3, medalY - 3, 3, 0xFFF0A0, 0xFF, INK_NONE, true);
        }
        else {
            RSDK.DrawCircle(medalX, medalY, 11, 0x585858, 0xFF, INK_NONE, true);
            RSDK.DrawCircle(medalX, medalY, 8, 0x202020, 0xFF, INK_NONE, true);
        }

        // the name (bold capitals), a rule, then the description (centred in its two lines' room when it needs one) a
        // shade dimmer; all of it dimmer still while locked
        String *name = &MainMenu->achievementNames[id], *desc = &MainMenu->achievementDescs[id];
        int32 split  = MainMenu->achievementSplit[id];
        int32 textX  = left + 34, textY = y + 5 + (bounce >> 16) + (textBounce >> 16);
        page->inkEffect = got ? INK_NONE : INK_ALPHA;
        page->alpha     = 0x90;
        drawPos.x       = TO_FIXED(camX + textX);
        drawPos.y       = TO_FIXED(camY + textY + MAINMENU_ACH_LINE_H / 2);
        RSDK.DrawText(MainMenu->achievementSmallNames ? &MainMenu->descAnimator : &MainMenu->nameAnimator, &drawPos, name, 0, name->length,
                      ALIGN_LEFT, MainMenu->achievementNameSpacing[id], INT_TO_VOID(2), NULL, false);

        int32 ruleY = textY + MAINMENU_ACH_LINE_H + MAINMENU_ACH_SEP / 2;
        RSDK.DrawRect(textX - 2, ruleY, left + cardW - 6 - (textX - 2), 1, got ? 0x806010 : 0x404040, 0xFF, INK_NONE, true);

        page->inkEffect = INK_ALPHA;
        page->alpha     = got ? 0xC8 : 0x70;
        drawPos.y       = TO_FIXED(camY + ruleY + MAINMENU_ACH_SEP / 2 + 1 + MAINMENU_ACH_LINE_H / 2 + (split ? 0 : MAINMENU_ACH_LINE_H / 2));
        RSDK.DrawText(&MainMenu->descAnimator, &drawPos, desc, 0, split ? split : desc->length, ALIGN_LEFT, 0, NULL, NULL, false);
        if (split) {
            drawPos.y += TO_FIXED(MAINMENU_ACH_LINE_H);
            RSDK.DrawText(&MainMenu->descAnimator, &drawPos, desc, split + MainMenu->achievementSkip[id], desc->length, ALIGN_LEFT, 0, NULL,
                          NULL, false);
        }
        page->inkEffect = INK_NONE;
    }

    // a scroll bar beside the cards when they don't all fit
    if (page->scroll > 0 || last < MainMenu->achievementCount - 1) {
        int32 count = MainMenu->achievementCount, shown = last - page->scroll + 1;
        int32 barX = w - 12, trackH = MainMenu_AchListBottom() - top;
        int32 thumbH = trackH * shown / count, thumbY = top + (trackH - thumbH) * page->scroll / (count - shown);
        RSDK.DrawRect(barX, top, 4, trackH, 0x000000, 0xFF, INK_NONE, true);
        RSDK.DrawRect(barX, thumbY, 4, thumbH, 0xF0F0F0, 0xFF, INK_NONE, true);
    }
}
#endif

#if GAME_INCLUDE_EDITOR
void MainMenu_EditorDraw(void) {}

void MainMenu_EditorLoad(void) {}
#endif

void MainMenu_Serialize(void) {}
#endif
