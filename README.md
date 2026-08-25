# Tom & Jerry: War of the Whiskers — Native PC Port & Advanced Modding Engine 🎮

[![GitHub Release](https://img.shields.io/github/v/release/osamalvzx/tom-jerry-war-of-the-whiskers-pc?color=green&label=Release)](https://github.com/osamalvzx/tom-jerry-war-of-the-whiskers-pc/releases)
[![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen.svg)]()
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-blue.svg)]()
[![Modding Engine](https://img.shields.io/badge/Modding%20Engine-Active%20v2.0-orange.svg)]()

A native Windows PC port of the classic 2003 fighting game *Tom and Jerry: War of the Whiskers*. **Not an emulator**: the original x86 game code executes in-process, with the Xbox kernel, Direct3D 8, and DirectSound replaced by high-performance modern Windows subsystems (Direct3D 11, XAudio2, and Win32 file I/O).

---

## 🌟 Key Features

- **Full Game Support**: All characters, all arenas, Challenge mode, and Quick Game.
- **Local & LAN Multiplayer**: Up to 4 players on one PC or frame-locked deterministic LAN multiplayer.
- **Widescreen & High Resolution**: Crisp rendering at 720p, 1080p, 1440p, 4K+ (Windowed, Borderless, Fullscreen).
- **MEAT RUSH Mode**: A brand new fast-paced competitive collection game mode.
- **Native In-Game Modding Engine (`MODS CONFIG`)**: Real-time asset overriding without touching original game files!
- **Full Arabic Language & Localization Support 🇸🇦**: Reshaped Arabic text engine with RTL support and 3D country flag rendering.

---

## 🛠️ Modding Engine & Categories (`mods/` Folder)

The port features a complete, zero-overhead asset replacement system. Simply place your modified files into the `mods/` directory and toggle them live in-game under **`OPTIONS ➔ MODS CONFIG`**.

The game comes pre-configured with dedicated directories for every possible mod category:

| Folder Name | Mod Name | Description | Target Asset Paths |
|---|---|---|---|
| `01_Arabic_Language_Pack` | **ARABIC LANG PACK** | Full Arabic localization and custom Arabic fonts | `GFX/FE/` |
| `02_Character_Skins_Mod` | **CHARACTER SKINS** | Custom 3D character models, costumes, and skins | `GFX/CAST/<CHARACTER>/` |
| `03_Arenas_and_Stages_Mod` | **ARENAS & STAGES** | Arena textures, stage backgrounds, and lighting | `GFX/<ARENA>/` |
| `04_Custom_Audio_and_Voices_Mod` | **AUDIO & VOICES** | Custom voice lines, sound effects, and BGM | `AUDMUSIC/`, `AUDSoundFX/` |
| `05_UI_and_HUD_Customizer` | **UI & HUD CUSTOM** | Custom loading screens, menus, and health bars | `GFX/FE/`, `GFX/OSD/` |
| `06_HD_Graphics_and_Textures_Mod` | **HD GRAPHICS PACK** | High-definition remastered textures and graphics | `GFX/` |

### How Modding Works:
1. Open the `mods/` folder in your game directory.
2. Inside any mod folder, place your modified files following the game's directory structure (e.g. `mods/02_Character_Skins_Mod/GFX/CAST/TOM/tom.XBD`).
3. Launch the game, go to **`OPTIONS ➔ MODS CONFIG`**, and toggle the mod to **ON**.
4. Settings are automatically saved to `tomjerry.ini`!

---

## 🇸🇦 الدليل العربي لنظام المودات والتعريب (Arabic Guide)

تم تزويد هذا البورت بمحرك مودات متكامل يُمكنك من تعديل كل شيء في اللعبة بسهولة:

### 1. خيارات المودات المدمجة (In-Game MODS CONFIG):
- **حركة القوائم (MENU TRANSITIONS)**: تفعيل أو تعطيل الحركة الكرتونية السلسة للقوائم بمعدل 60 إطاراً.
- **شعار أسامة 3D (OSAMA BADGE)**: تفعيل أو تعطيل الشعار ثلاثي الأبعاد المميز.
- **اللغة (LANGUAGE)**: التبديل الفوري بين اللغة الإنجليزية واللغة العربية مع إظهار علم المملكة العربية السعودية 🇸🇦.
- **المودات المخصصة**: تشغيل أو إيقاف أي مود داخل مجلد `mods/` بضغطة زر واحدة.

### 2. أقسام مجلد المودات (`mods/`):
- `01_Arabic_Language_Pack`: لوضع ملفات الخطوط العربية المعدلة لتعريب واجهة اللعبة.
- `02_Character_Skins_Mod`: لتعديل أشكال وملابس شخصيات (توم، جيري، سبايك، بوتش، إلخ).
- `03_Arenas_and_Stages_Mod`: لتعديل خامات وألوان الحلبات والمراحل (المطبخ، القصر المسكون، المختبر، السفينة).
- `04_Custom_Audio_and_Voices_Mod`: لتغيير المؤثرات الصوتية وأصوات الشخصيات والموسيقى.
- `05_UI_and_HUD_Customizer`: لتعديل واجهات القوائم وشاشات البداية وأشرطة الصحة.
- `06_HD_Graphics_and_Textures_Mod`: لحزم تحسين جودة وتوضيح الجرافيكس (HD Textures).

---

## 📥 Installation & Setup

1. Download the latest release from the [Releases](https://github.com/osamalvzx/tom-jerry-war-of-the-whiskers-pc/releases) page.
2. Run `setup.exe` to install the runtime and mod templates into your game directory.
3. Launch the game using `PLAY.cmd` or the created desktop shortcut.

---

## 🔨 Building from Source

Requires **Visual Studio 2022 Build Tools (x86 toolset)** and **CMake**.

```powershell
# Configure build
cmake -S port -B port/build -A Win32

# Compile full release
cmake --build port/build --config Release
```

---

## 📜 Credits & Community
- **Developer & Maintainer**: Osama ([@osamalvzx](https://github.com/osamalvzx))
- **Based on**: Tom and Jerry: War of the Whiskers (VIS Entertainment / Warner Bros.)
