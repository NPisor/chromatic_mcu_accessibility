// Default OSD with snapshot disabled but full menu population so MenuMgr can render tabs.

#include "osd_default.h"

#include "battery.h"
#include "menu_mgr.h"
#include "tab_shared.h"
#include "tab_list.h"
#include "tab_dots.h"
#include "tab_table.h"
#include "status/fw.h"
#include "status/brightness.h"
#include "controls/dpad_ctl.h"
#include "controls/hotkeys.h"
#include "palette/style.h"
#include "palette/gbc_color_temp.h"
#include "display/color_correct_lcd.h"
#include "display/color_correct_usb.h"
#include "display/frameblend.h"
#include "display/low_batt_icon_ctl.h"
#include "display/screen_transit_ctl.h"
#include "system/silent.h"
#include "system/player_num.h"
#include "system/serial_num.h"
#include "system/bt_advert.h"
#include "system/cheats.h"
#include "system/achievement.h"
#include "fpga_tx.h"
#include "settings.h"
#include "osd.h"
#include "osd_shared.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "OSDDef";
#define SNAPSHOT_DISABLED 1

// Snapshot/preview stubs (feature disabled)
#if SNAPSHOT_DISABLED
void OSD_Snapshot_OnRequestStart(void) {}
void OSD_Snapshot_OnWRAMChunk(uint16_t addr, const uint8_t* data, size_t len) { (void)addr; (void)data; (void)len; }
void OSD_FBPreview_OnRequestStart(void) {}
void OSD_FBPreview_OnChunk(uint16_t addr, const uint8_t* data, size_t len) { (void)addr; (void)data; (void)len; }
#endif

LV_IMG_DECLARE(menu_status);
LV_IMG_DECLARE(menu_display);
LV_IMG_DECLARE(menu_controls);
LV_IMG_DECLARE(menu_palette);
LV_IMG_DECLARE(menu_palette_gbc);
LV_IMG_DECLARE(menu_system);
LV_IMG_DECLARE(menu_cheats);
LV_IMG_DECLARE(menu_achievement);

static lv_obj_t *pSnapshotLabel;

typedef enum {
	kPaletteTabMode_GB = 0,
	kPaletteTabMode_GBC,
} PaletteTabMode_t;

static PaletteTabMode_t sPaletteTabMode = kPaletteTabMode_GB;
static TabCollection_t PaletteTabGB;
static TabCollection_t PaletteTabGBC;
static MenuTab_t PaletteMenu = {
	.Widget = {
		.fnDraw = TabTable_Draw,
		.fnOnTransition = TabList_OnTransition,
		.fnOnButton = Tab_OnButton,
		.Name = "Palette",
	},
	.pImageDesc = &menu_palette,
	.Menu = &PaletteTabGB,
};
static TabItem_t PaletteItems[kNumPalettes];
static TabItem_t ColorTempItem;

static void Palette_SetMode(PaletteTabMode_t mode);
static void Palette_SetModeAsync(void *param);
static void Palette_OnGBCModeChanged(bool isGBC);
static void RegisterPaletteCallbacks(void);
static void ColorTemp_OnUpdate(void);
static void Palette_ClearTabs(void);
static void Palette_ClearTabCollection(TabCollection_t *const pList);

static void InitTabCollection(TabCollection_t *const pList)
{
	if (pList == NULL)
	{
		return;
	}

	sys_dlist_init(&pList->WidgetList);
	pList->pCurrent = NULL;
	pList->pDividerObj = NULL;
	pList->pSelectedObj = NULL;
}

static void CreateMenuStatus(lv_obj_t *const pScreen)
{
	static TabCollection_t StatusTab;
	static MenuTab_t StatusMenu = {
		.Widget = {
			.fnDraw = TabList_Draw,
			.fnOnTransition = TabList_OnTransition,
			.fnOnButton = Tab_OnButton,
			.Name = "Status",
		},
		.pImageDesc = &menu_status,
		.Menu = &StatusTab,
	};

	InitTabCollection(&StatusTab);
	StatusMenu.Accent = lv_color_hex(0x5FC0FF);

	static TabItem_t BrightnessItem = {
		.Widget = {
			.fnDraw = Brightness_Draw,
			.fnOnTransition = Brightness_OnTransition,
			.fnOnButton = Brightness_OnButton,
			.Name = "BRIGHTNESS",
		},
	};

	static TabItem_t SilentItem = {
		.Widget = {
			.fnDraw = SilentMode_Draw,
			.fnOnTransition = SilentMode_OnTransition,
			.fnOnButton = SilentMode_OnButton,
			.Name = "SILENT MODE",
		},
	};

	(void)Tab_AddItem(&StatusTab, &BrightnessItem, pScreen);
	(void)Tab_AddItem(&StatusTab, &SilentItem, pScreen);

	(void)MenuMgr_AddTab(kTabID_Status, &StatusMenu);
}

static void CreateMenuDisplay(lv_obj_t *const pScreen)
{
	static TabCollection_t DisplayTab;
	static MenuTab_t DisplayMenu = {
		.Widget = {
			.fnDraw = TabList_Draw,
			.fnOnTransition = TabList_OnTransition,
			.fnOnButton = Tab_OnButton,
			.Name = "Display",
		},
		.pImageDesc = &menu_display,
		.Menu = &DisplayTab,
	};

	InitTabCollection(&DisplayTab);
	DisplayMenu.Accent = lv_color_hex(0xFFD166);

	static TabItem_t FrameBlendItem = {
		.Widget = {
			.fnDraw = FrameBlend_Draw,
			.fnOnTransition = FrameBlend_OnTransition,
			.fnOnButton = FrameBlend_OnButton,
			.Name = "FRAME BLEND",
		},
	};

	static TabItem_t CCUSBItem = {
		.Widget = {
			.fnDraw = ColorCorrectUSB_Draw,
			.fnOnTransition = ColorCorrectUSB_OnTransition,
			.fnOnButton = ColorCorrectUSB_OnButton,
			.Name = "USB COLOR CORR",
		},
	};

	static TabItem_t CCLCDItem = {
		.Widget = {
			.fnDraw = ColorCorrectLCD_Draw,
			.fnOnTransition = ColorCorrectLCD_OnTransition,
			.fnOnButton = ColorCorrectLCD_OnButton,
			.Name = "LCD COLOR CORR",
		},
	};

	static TabItem_t ScreenTransitItem = {
		.Widget = {
			.fnDraw = ScreenTransitCtl_Draw,
			.fnOnTransition = ScreenTransitCtl_OnTransition,
			.fnOnButton = ScreenTransitCtl_OnButton,
			.Name = "SCREEN TRANS",
		},
	};

	static TabItem_t LowBattIconItem = {
		.Widget = {
			.fnDraw = LowBattIconCtl_Draw,
			.fnOnTransition = LowBattIconCtl_OnTransition,
			.fnOnButton = LowBattIconCtl_OnButton,
			.Name = "LOW BATT ICON",
		},
	};

	(void)Tab_AddItem(&DisplayTab, &FrameBlendItem, pScreen);
	(void)Tab_AddItem(&DisplayTab, &CCUSBItem, pScreen);
	(void)Tab_AddItem(&DisplayTab, &CCLCDItem, pScreen);
	(void)Tab_AddItem(&DisplayTab, &ScreenTransitItem, pScreen);
	(void)Tab_AddItem(&DisplayTab, &LowBattIconItem, pScreen);

	(void)MenuMgr_AddTab(kTabID_Display, &DisplayMenu);
}

static void CreateMenuControls(lv_obj_t *const pScreen)
{
	static TabCollection_t ControlsTab;
	static MenuTab_t ControlsMenu = {
		.Widget = {
			.fnDraw = TabDot_Draw,
			.fnOnTransition = TabDot_OnTransition,
			.fnOnButton = Tab_OnButton,
			.Name = "Controls",
		},
		.pImageDesc = &menu_controls,
		.Menu = &ControlsTab,
	};

	InitTabCollection(&ControlsTab);
	ControlsMenu.Accent = lv_color_hex(0x9B8CFF);

	static TabItem_t DPadItem = {
		.Widget = {
			.fnDraw = DPadCtl_Draw,
			.fnOnTransition = DPadCtl_OnTransition,
			.fnOnButton = DPadCtl_OnButton,
			.Name = "D-PAD MODE",
		},
	};

	static TabItem_t HotKeysItem = {
		.Widget = {
			.fnDraw = HotKeys_Draw,
			.fnOnTransition = HotKeys_OnTransition,
			.fnOnButton = HotKeys_OnButton,
			.Name = "HOTKEYS",
		},
	};

	(void)Tab_AddItem(&ControlsTab, &DPadItem, pScreen);
	(void)Tab_AddItem(&ControlsTab, &HotKeysItem, pScreen);

	(void)MenuMgr_AddTab(kTabID_Controls, &ControlsMenu);
}

static const char* const PaletteNames[kNumPalettes] = {
	[kPalette_Default]   = "DEFAULT",
	[kPalette_Brown]     = "BROWN",
	[kPalette_Blue]      = "BLUE",
	[kPalette_Pastel]    = "PASTEL",
	[kPalette_Green]     = "GREEN",
	[kPalette_Red]       = "RED",
	[kPalette_DarkBlue]  = "DARK BLUE",
	[kPalette_Orange]    = "ORANGE",
	[kPalette_DarkGreen] = "DARK GREEN",
	[kPalette_DarkBrown] = "DARK BROWN",
	[kPalette_Grayscale] = "GRAYSCALE",
	[kPalette_Yellow]    = "YELLOW",
	[kPalette_Negative]  = "NEGATIVE",
	[kPalette_DMG1]      = "DMG1",
	[kPalette_DMG2]      = "DMG2",
};

static void CreateMenuPalette(lv_obj_t *const pScreen)
{
	InitTabCollection(&PaletteTabGB);
	InitTabCollection(&PaletteTabGBC);

	PaletteMenu.Accent = lv_color_hex(0xFF7F66);

	for (size_t i = 0; i < kNumPalettes; ++i)
	{
		PaletteItems[i].Widget.fnDraw = Style_Draw;
		PaletteItems[i].Widget.fnOnTransition = Style_OnTransition;
		PaletteItems[i].Widget.fnOnButton = Style_OnButton;
		PaletteItems[i].Widget.Name = PaletteNames[i];

		(void)Tab_AddItem(&PaletteTabGB, &PaletteItems[i], pScreen);
	}

	ColorTempItem = (TabItem_t){
		.Widget = {
			.fnDraw = GBCColorTemp_Draw,
			.fnOnButton = GBCColorTemp_OnButton,
			.fnOnTransition = GBCColorTemp_OnTransition,
			.Name = "COLOR TEMP",
		},
	};

	(void)Tab_AddItem(&PaletteTabGBC, &ColorTempItem, pScreen);

	GBCColorTemp_InitFromSettings();
	GBCColorTemp_RegisterOnUpdateCb(ColorTemp_OnUpdate);

	Palette_SetMode(Style_IsGBCMode() ? kPaletteTabMode_GBC : kPaletteTabMode_GB);

	(void)MenuMgr_AddTab(kTabID_Palette, &PaletteMenu);
	RegisterPaletteCallbacks();
}

static void Palette_SetMode(PaletteTabMode_t mode)
{
	if (sPaletteTabMode == mode)
	{
		return;
	}

	Palette_ClearTabs();

	switch (mode)
	{
		case kPaletteTabMode_GBC:
			PaletteMenu.Widget.fnDraw = TabList_Draw;
			PaletteMenu.Widget.fnOnTransition = TabList_OnTransition;
			PaletteMenu.Widget.fnOnButton = Tab_OnButton;
			PaletteMenu.Menu = &PaletteTabGBC;
			PaletteMenu.pImageDesc = &menu_palette_gbc;
			FPGA_Tx_WriteColorTemp(GBCColorTemp_GetLevel());
			break;
		case kPaletteTabMode_GB:
		default:
			PaletteMenu.Widget.fnDraw = TabTable_Draw;
			PaletteMenu.Widget.fnOnTransition = TabList_OnTransition;
			PaletteMenu.Widget.fnOnButton = Tab_OnButton;
			PaletteMenu.Menu = &PaletteTabGB;
			PaletteMenu.pImageDesc = &menu_palette;
			FPGA_Tx_WritePaletteStyle();
			break;
	}

	sPaletteTabMode = mode;
}

static void Palette_ClearTabs(void)
{
	Palette_ClearTabCollection(&PaletteTabGB);
	Palette_ClearTabCollection(&PaletteTabGBC);
}

static void Palette_ClearTabCollection(TabCollection_t *const pList)
{
	if (pList == NULL)
	{
		return;
	}

	if (pList->pDividerObj != NULL)
	{
		lv_obj_del(pList->pDividerObj);
		pList->pDividerObj = NULL;
	}

	if (pList->pSelectedObj != NULL)
	{
		lv_obj_del(pList->pSelectedObj);
		pList->pSelectedObj = NULL;
	}

	sys_dnode_t *pNode = NULL;
	SYS_DLIST_FOR_EACH_NODE(&pList->WidgetList, pNode)
	{
		if (pNode == NULL)
		{
			break;
		}

		TabItem_t *const pItem = (TabItem_t *)pNode;
		if (pItem->Widget.fnOnTransition != NULL)
		{
			pItem->Widget.fnOnTransition(NULL);
		}

		if (pItem->DataObj != NULL)
		{
			lv_obj_del(pItem->DataObj);
			pItem->DataObj = NULL;
		}
	}
}

static void Palette_SetModeAsync(void *param)
{
	const bool gbcMode = (param != NULL);
	Palette_SetMode(gbcMode ? kPaletteTabMode_GBC : kPaletteTabMode_GB);
}

static void Palette_OnGBCModeChanged(bool isGBC)
{
	(void)lv_async_call(Palette_SetModeAsync, isGBC ? (void *)1 : NULL);
}

static void RegisterPaletteCallbacks(void)
{
	Style_RegisterOnGBCModeChanged(Palette_OnGBCModeChanged);
}

static void ColorTemp_OnUpdate(void)
{
	FPGA_Tx_WriteColorTemp(GBCColorTemp_GetLevel());
}

static void CreateMenuSystem(lv_obj_t *const pScreen)
{
	static TabCollection_t SystemTab;
	static MenuTab_t SystemMenu = {
		.Widget = {
			.fnDraw = TabList_Draw,
			.fnOnTransition = TabList_OnTransition,
			.fnOnButton = Tab_OnButton,
			.Name = "System",
		},
		.pImageDesc = &menu_system,
		.Menu = &SystemTab,
	};

	InitTabCollection(&SystemTab);
	SystemMenu.Accent = lv_color_hex(0x7ED957);

	static TabItem_t BTAdvertItem = {
		.Widget = {
			.fnDraw = BTAdvert_Draw,
			.fnOnTransition = BTAdvert_OnTransition,
			.fnOnButton = BTAdvert_OnButton,
			.Name = "BLUETOOTH",
		},
	};

	static TabItem_t FirmwareItem = {
		.Widget = {
			.fnDraw = Firmware_Draw,
			.fnOnTransition = Firmware_OnTransition,
			.fnOnButton = Firmware_OnButton,
			.Name = "FIRMWARE",
		},
	};

	static TabItem_t PlayerNumItem = {
		.Widget = {
			.fnDraw = PlayerNum_Draw,
			.fnOnTransition = PlayerNum_OnTransition,
			.fnOnButton = PlayerNum_OnButton,
			.Name = "PLAYER NUMBER",
		},
	};

	static TabItem_t SerialNumItem = {
		.Widget = {
			.fnDraw = SerialNum_Draw,
			.fnOnTransition = SerialNum_OnTransition,
			.Name = "SERIAL NUMBER",
		},
	};

	(void)Tab_AddItem(&SystemTab, &BTAdvertItem, pScreen);
	(void)Tab_AddItem(&SystemTab, &FirmwareItem, pScreen);
	(void)Tab_AddItem(&SystemTab, &PlayerNumItem, pScreen);

	if (SerialNum_IsPresent())
	{
		(void)Tab_AddItem(&SystemTab, &SerialNumItem, pScreen);
	}

	(void)MenuMgr_AddTab(kTabID_System, &SystemMenu);
}

static void CreateMenuCheats(lv_obj_t *const pScreen)
{
	static TabCollection_t CheatsTab;
	static MenuTab_t CheatsMenu = {
		.Widget = {
			.fnDraw = TabList_Draw,
			.fnOnTransition = TabList_OnTransition,
			.fnOnButton = Tab_OnButton,
			.Name = "Cheats",
		},
		.pImageDesc = &menu_cheats,
		.Menu = &CheatsTab,
	};

	InitTabCollection(&CheatsTab);
	CheatsMenu.Accent = lv_color_hex(0xFFC857);

	static TabItem_t CheatsItem = {
		.Widget = {
			.fnDraw = Cheats_Draw,
			.fnOnTransition = Cheats_OnTransition,
			.fnOnButton = Cheats_OnButton,
			.Name = "ACTIVE SLOTS",
		},
	};

	(void)Tab_AddItem(&CheatsTab, &CheatsItem, pScreen);

	(void)MenuMgr_AddTab(kTabID_Cheats, &CheatsMenu);
}

static OSD_Result_t SnapshotDisabled_Draw(void *arg)
{
	if (arg == NULL)
	{
		return kOSD_Result_Err_NullDataPtr;
	}

	lv_obj_t *const pScreen = (lv_obj_t *)arg;
	if (pSnapshotLabel == NULL)
	{
		pSnapshotLabel = lv_label_create(pScreen);
		lv_obj_add_style(pSnapshotLabel, OSD_GetStyleTextGrey(), 0);
		lv_obj_align(pSnapshotLabel, LV_ALIGN_TOP_LEFT, 80, 50);
		lv_label_set_text_static(pSnapshotLabel, "SNAPSHOT DISABLED");
	}

	lv_obj_move_foreground(pSnapshotLabel);
	return kOSD_Result_Ok;
}

static OSD_Result_t SnapshotDisabled_OnTransition(void *arg)
{
	(void)arg;
	if (pSnapshotLabel != NULL)
	{
		lv_obj_del(pSnapshotLabel);
		pSnapshotLabel = NULL;
	}
	return kOSD_Result_Ok;
}

static void CreateMenuAchievementStub(lv_obj_t *const pScreen)
{
	static TabCollection_t AchievementTab;
	static MenuTab_t AchievementMenu = {
		.Widget = {
			.fnDraw = TabList_Draw,
			.fnOnTransition = TabList_OnTransition,
			.fnOnButton = Tab_OnButton,
			.Name = "Achievements",
		},
		.pImageDesc = &menu_achievement,
		.Menu = &AchievementTab,
	};

	InitTabCollection(&AchievementTab);
	AchievementMenu.Accent = lv_color_hex(0xA0A0A0);
	static TabItem_t AchievementItem = {
		.Widget = {
			.fnDraw = Achievement_Draw,
			.fnOnTransition = Achievement_OnTransition,
			.Name = "USER CARD",
		},
	};

	(void)Tab_AddItem(&AchievementTab, &AchievementItem, pScreen);
	(void)MenuMgr_AddTab(kTabID_Achievement, &AchievementMenu);
}

void OSD_Default_Init(lv_obj_t *const pScreen)
{
	static bool settingsInitDone;
	if (!settingsInitDone)
	{
		(void)Settings_Initialize();
		settingsInitDone = true;
	}

	static OSD_Widget_t MenuMgr = { .Name = "MenuMgr" };

	OSD_Result_t eResult = MenuMgr_Initialize(&MenuMgr, pScreen);
	if (eResult != kOSD_Result_Ok)
	{
		ESP_LOGE(TAG, "Menu manager init failed: %d", eResult);
		return;
	}

	static OSD_Widget_t Battery = { .Name = "Battery" };
	if ((eResult = Battery_Init(&Battery)) != kOSD_Result_Ok)
	{
		ESP_LOGE(TAG, "Battery widget init failed %d", eResult);
		return;
	}

	CreateMenuStatus(pScreen);
	CreateMenuDisplay(pScreen);
	CreateMenuControls(pScreen);
	CreateMenuPalette(pScreen);
	CreateMenuSystem(pScreen);
	CreateMenuCheats(pScreen);
	CreateMenuAchievementStub(pScreen);

	OSD_AddWidget(&Battery);
	OSD_AddWidget(&MenuMgr);

	ESP_LOGI(TAG, "Default OSD init OK");
}
