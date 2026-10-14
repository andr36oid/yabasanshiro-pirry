/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "MenuScreen.h"
#include <nanogui/window.h>
#include <nanogui/layout.h>
#include <nanogui/label.h>
#include <nanogui/button.h>
#include <nanogui/popupbutton.h>
#include <nanogui/messagedialog.h>
#include <nanogui/combobox.h>
#include <nanogui/vscrollpanel.h>
#include "../config.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <string>
// Forward declaration: the "Next Disc" button in the menu constructor uses this
// helper, whose definition must follow `ends_with`.
static std::vector<std::string> discliste_1209(const std::string & base_path);

#include <experimental/filesystem>
#include <ctime>
#include <iomanip>

#include <nanogui/imageview.h>

namespace fs = std::experimental::filesystem ;

using namespace std;

#include "InputManager.h"

#include "scsp.h"
#include "about.h"

#include "Preference.h"

#include "yabause.h"

//#define MENU_LOG
#define MENU_LOG(...) do { } while (0)


extern int g_EnagleFPS;
extern int g_resolution_mode;
extern int g_keep_aspect_rate;
extern int g_rotate_resolution_mode;
extern int g_scsp_sync;
extern int g_frame_skip;
extern int g_emulated_bios;


int MenuScreen::onShow(){
  //setupPlayerPsuhButton( 0, player1, "Player1 Input Settings", &p1cb );
  //setupPlayerPsuhButton( 1, player2, "Player2 Input Settings", &p2cb );
}

MenuScreen::MenuScreen( SDL_Window* pwindow, int rwidth, int rheight, const std::string & fname, const std::string & game )
: nanogui::Screen( pwindow, Eigen::Vector2i(rwidth, rheight), "Menu Screen"){
  
  mFocus = nullptr;
  config_file_ = fname;
  current_game_path_ = game;
  swindow = nullptr;
  imageWindow = nullptr;


  std::string title = "Yaba Sanshiro "+ std::string(YAB_VERSION) +" Pirry Edition";
        window = new Window(this, title);
        window->setPosition(Vector2i(0, 0));
        window->setLayout(new GroupLayout());

        tools = new Widget(window);
        pushActiveMenu(tools, nullptr );
        tools->setLayout(new BoxLayout(Orientation::Vertical,Alignment::Middle, 0, 5));
        tools->setFixedWidth(256);

        PlayerConfig tmp;
        tmp.player = new PopupButton(tools, "Player1", ENTYPO_ICON_EXPORT);      
        setupPlayerPsuhButton( 0, tmp.player, "Player1 Input Settings", &tmp.cb );
        player_configs_.push_back(tmp);
        tmp.player = new PopupButton(tools, "Player2", ENTYPO_ICON_EXPORT);      
        setupPlayerPsuhButton( 1, tmp.player, "Player2 Input Settings", &tmp.cb );
        player_configs_.push_back(tmp);

       
        PopupButton * ps_config = new PopupButton(tools, "Config");
        ps_config->setFixedWidth(248);
        showConfigDialog(ps_config);
        ps_config->setCallback([this,ps_config]() {      
          pushActiveMenu(ps_config->popup(),ps_config); 
        });

        Button *b0 = new Button(tools, "Exit");
        b0->setFixedWidth(248);
        b0->setCallback([this]() { 
          MENU_LOG("Exit\n"); 
      		SDL_Event* quit = new SDL_Event();
			    quit->type = SDL_QUIT;
			    SDL_PushEvent(quit);  
        });

        Button *b1 = new Button(tools, "Reset");
        b1->setFixedWidth(248);
        b1->setCallback([this]() { 
          MENU_LOG("Reset\n");  
          SDL_Event event = {};
          event.type = reset_;
          event.user.code = 0;
          event.user.data1 = 0;
          event.user.data2 = 0;
          SDL_PushEvent(&event);          
        });        

        /* Separate submenu for the volume sliders. */
        PopupButton * ps_sound = new PopupButton(tools, "Sound Settings");
        ps_sound->setFixedWidth(248);
        showSoundDialog(ps_sound);
        ps_sound->setCallback([this,ps_sound]() {
          pushActiveMenu(ps_sound->popup(), ps_sound);
        });

        PopupButton * ps = new PopupButton(tools, "Save State");
        ps->setFixedWidth(248);
        ps->setCallback([this,ps]() {      
          showSaveStateDialog( ps->popup());
          pushActiveMenu(ps->popup(),ps); 
        });

        ps = new PopupButton(tools, "Load State");
        ps->setFixedWidth(248);
        ps->setCallback([this,ps]() {      
          showLoadStateDialog( ps->popup());
          pushActiveMenu(ps->popup(),ps); 
        });



        bCdTray = new Button(tools, "Open CD Tray");
        bCdTray->setFixedWidth(248);
        bCdTray->setCallback([this]() { 
          if( this->is_cdtray_open_ ){
            MENU_LOG("Close CD Tray\n"); 
            bCdTray->setCaption("Open CD Tray");
            this->is_cdtray_open_ = false;

            size_t pos = current_game_path_.find_last_of("/");
            string base_path = current_game_path_.substr(0,pos);
            showFileSelectDialog( tools, bCdTray, base_path);

            /*
            SDL_Event event = {};
            event.type = close_tray_;
            event.user.code = 0;
            //event.user.data1 = malloc( 256* sizeof(char) );
            //strcpy( (char*)event.user.data1, "filename" );
            event.user.data2 = 0;
            SDL_PushEvent(&event);               
            */

          }else{
            MENU_LOG("Open CD Tray\n"); 
            bCdTray->setCaption("Close CD Tray");
            this->is_cdtray_open_ = true;
            SDL_Event event = {};
            event.type = open_tray_;
            event.user.code = 0;
            event.user.data1 = 0;
            event.user.data2 = 0;
            SDL_PushEvent(&event);
          }
        });

        // "Next Disc": switch to the next image in the game directory with one click.
        // Multi-disc games are stored as separate .chd files in one directory.
        // This is not an automatic switch: the Saturn gives no "insert disc 2"
        // signal, the game only polls the disc-change flag.
        // With the HLE BIOS, closing the tray restarts the game from the new disc
        // (YabauseQuickLoadGame); switching in mid-game needs a real BIOS (-b).
        bNextDisc = new Button(tools, "Next Disc");
        bNextDisc->setFixedWidth(248);
        bNextDisc->setCallback([this]() {
          size_t pos = current_game_path_.find_last_of("/");
          if( pos == std::string::npos ) return;
          std::string base_path = current_game_path_.substr(0,pos);
          std::vector<std::string> discs = discliste_1209(base_path);
          if( discs.size() < 2 ){
            // Single-disc title: nothing to switch. Do nothing rather than reinsert
            // the same disc, which would restart the game under the HLE BIOS.
            MENU_LOG("Next Disc: nur %d Abbild(er) in %s -- nichts zu wechseln\n",
                     (int)discs.size(), base_path.c_str());
            return;
          }
          size_t idx = 0; bool gefunden = false;
          for( size_t i = 0; i < discs.size(); i++ )
            if( discs[i] == current_game_path_ ){ idx = i; gefunden = true; break; }
          // Not found (game lies elsewhere than the list): start at 0.
          size_t naechste = gefunden ? ((idx + 1) % discs.size()) : 0;
          std::string ziel = discs[naechste];
          MENU_LOG("Next Disc: %s -> %s\n", current_game_path_.c_str(), ziel.c_str());

          // Tray open ...
          SDL_Event ev_auf = {};
          ev_auf.type = open_tray_;
          ev_auf.user.code = 0; ev_auf.user.data1 = 0; ev_auf.user.data2 = 0;
          SDL_PushEvent(&ev_auf);
          // ... and closed again with the next disc. Both events are handled in the
          // same pass of the main loop, as with the file dialog.
          SDL_Event ev_zu = {};
          ev_zu.type = close_tray_;
          ev_zu.user.code = 0;
          ev_zu.user.data1 = malloc( (ziel.size()+1) * sizeof(char) );
          strcpy( (char*)ev_zu.user.data1, ziel.c_str() );
          ev_zu.user.data2 = 0;
          SDL_PushEvent(&ev_zu);

          // Track the path here: main.cpp only calls setCurrentGamePath on
          // OpenTray, when cdpath is still the old path.
          current_game_path_ = ziel;

          // Keep the menu consistent: the tray is closed again after the close event.
          this->is_cdtray_open_ = false;
          if( this->bCdTray != nullptr ) this->bCdTray->setCaption("Open CD Tray");
        });


        Button *b2 = new Button(tools, "Show/Hide FPS");
        b2->setFixedWidth(248);
        b2->setCallback([this]() { 
          MENU_LOG("Show/Hide FPS\n");  
          SDL_Event event = {};
          event.type = toggile_fps_;
          event.user.code = 0;
          event.user.data1 = 0;
          event.user.data2 = 0;
          SDL_PushEvent(&event);          
        });

        Button *b3 = new Button(tools, "Enable/Disable Frame Skip");
        b3->setFixedWidth(248);
        b3->setCallback([this]() { 
          MENU_LOG("Reset\n");  
          SDL_Event event = {};
          event.type = toggile_frame_skip_;
          event.user.code = 0;
          event.user.data1 = 0;
          event.user.data2 = 0;
          SDL_PushEvent(&event);          
        });        
#if 0
        Button *b4 = new Button(tools, "About");
        b4->setFixedWidth(248);
        b4->setCallback([this,b4]() { 
          int image_pix_size_w = this->width() / 2;
          int image_pix_size_h = this->height() / 2;
          imageWindow = new Window(this, "About");                                                                                                         
          imageWindow->setPosition(Vector2i(0, 0));                                                                                                                   
          imageWindow->setLayout(new GroupLayout(0,0,0));                                                                                                                     
          GLTexture t;    
          t.load(about_png,about_png_size);  
          float scale = (float)image_pix_size_w / t.width();
          //float offset = (t.width() - image_pix_size) / 2;
          auto imageView = new ImageView(imageWindow,t);  
          imageView->setScale(scale);
          imageView->setFixedScale(true);
          imageView->setFixedOffset(true);
          imageView->setFixedWidth(image_pix_size_w);
          imageView->setFixedHeight(image_pix_size_h);

          Button *btn = new Button(imageWindow, "Close");
          btn->setCallback([this]() { 
            this->popActiveMenu();
            if( this->imageWindow != nullptr ){
              this->imageWindow->dispose();
              this->imageWindow = nullptr;
            }
          });

          imageWindow->center();
          imageWindow->setModal(true);
          imageWindow->requestFocus();

          pushActiveMenu(imageWindow,b4);

        });
#endif
        player_configs_[0].player->focusEvent(true);
        player_configs_[0].player->mouseEnterEvent(player_configs_[0].player->absolutePosition(),true);
        mFocus = player_configs_[0].player;

        performLayout();
        
}


void MenuScreen::showInputCheckDialog( const std::string & key ){
    swindow = new Window(this, key);
    swindow->setPosition(Vector2i(0, 0));
    swindow->setLayout(new GroupLayout(32));
    new Label(swindow,"Push key for " + key, "sans", 64);
    swindow->center();
    swindow->setModal(true);
    swindow->requestFocus();
    current_key_ = key;
}

#include <dirent.h>

inline bool ends_with(std::string const & value, std::string const & ending)
{
    if (ending.size() > value.size()) return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

// Collect the disc images of a directory in sorted order (readdir order is
// filesystem dependent). Uses the same four extensions as showFileSelectDialog.
static std::vector<std::string> discliste_1209(const std::string & base_path)
{
  std::vector<std::string> liste;
  DIR *dir; struct dirent *ent;
  if ((dir = opendir(base_path.c_str())) != NULL) {
    while ((ent = readdir(dir)) != NULL) {
      std::string dname = ent->d_name;
      std::transform(dname.begin(), dname.end(), dname.begin(), ::tolower);
      if (ends_with(dname, ".cue") || ends_with(dname, ".mdf") ||
          ends_with(dname, ".ccd") || ends_with(dname, ".chd"))
        liste.push_back(base_path + "/" + std::string(ent->d_name));
    }
    closedir(dir);
  }
  std::sort(liste.begin(), liste.end());
  return liste;
}



/* Steps of the two sliders.
   CD goes down to 0 (replaces the old "Disable CD music" switch). Both go up
   to 150: the mix is integer and clamped to 16 bit only at the end, so higher
   values can overdrive and distort. Above 100, loud CD music mastered near
   full scale may be clipped at the end of the mix. */
static const int SNDVOL_CDDA[] = { 0, 25, 50, 75, 100, 125, 150 };
static const int SNDVOL_SFX[]  = { 0, 25, 50, 75, 100, 125, 150 };
static const int SNDVOL_CDDA_N = 7;
static const int SNDVOL_SFX_N  = 7;

extern "C" {
  void ScspSetCddaVolume(int v);
  void SmpcSetSystemsprache(int id);
  void ScspSetSfxVolume(int v);
}

static std::string sndvol_text(const char * name, int wert, bool cdda){
  char buf[64];
  if (cdda && wert == 0) snprintf(buf, sizeof(buf), "%s:  Off", name);
  else                   snprintf(buf, sizeof(buf), "%s:  %d%%", name, wert);
  return std::string(buf);
}

/* Step a slider by one level. richtung: -1 left, +1 right. */
void MenuScreen::soundVolStep( nanogui::Button * b, int richtung ){
  if (b == nullptr) return;
  /* `preference` is not a class member: showConfigDialog creates its own
     instance (via current_game_path_). Do the same here so the values land in
     the same game config. */
  std::shared_ptr<Preference> preference(new Preference( current_game_path_ ));
  const bool cdda = (b == snd_cdda_btn_);
  const int * stufen = cdda ? SNDVOL_CDDA : SNDVOL_SFX;
  const int n        = cdda ? SNDVOL_CDDA_N : SNDVOL_SFX_N;
  const char * key   = cdda ? "cd volume" : "sfx volume";
  const char * name  = cdda ? "CD Music" : "Sound Effects";
  int jetzt = preference->getInt(key, 100);
  /* Pick the nearest level so values not exactly in the list snap cleanly. */
  int idx = 0;
  int best = -1;
  for (int i = 0; i < n; i++) {
    const int d = (stufen[i] > jetzt) ? (stufen[i] - jetzt) : (jetzt - stufen[i]);
    if (best < 0 || d < best) { best = d; idx = i; }
  }
  idx += richtung;
  if (idx < 0) idx = 0;
  if (idx >= n) idx = n - 1;
  const int neu = stufen[idx];
  if (neu == jetzt) return;
  preference->setInt(key, neu);
  if (cdda) ScspSetCddaVolume(neu); else ScspSetSfxVolume(neu);
  b->setCaption(sndvol_text(name, neu, cdda));
}

extern "C" int use_new_scsp;   /* scsp.c: the core the current game is running with */
extern "C" int g_scsp_dspaus;  /* scsp.c: 1 = DSP off, switchable live */
extern "C" int g_interhalb_menue;  /* vidogl.c: 1 = half-height interlaced backgrounds, live */
/* The selected entry is tinted green (tintToggle). DSP Off / DSP On
   (selected: 1 = DSP on) takes effect immediately, no restart needed. */
void MenuScreen::soundKernZeigen( int gewaehlt ){
  tintToggle(snd_kern_btn_, gewaehlt == 0);
  tintToggle(snd_kern_neu_btn_, gewaehlt == 1);
  if (snd_kern_hinweis_ != nullptr)
    snd_kern_hinweis_->setCaption(" ");
}

void MenuScreen::soundKernStep( int richtung ){
  if (snd_kern_btn_ == nullptr) return;
  std::shared_ptr<Preference> preference(new Preference( current_game_path_ ));
  int jetzt = preference->getInt("sound dsp", 0) ? 1 : 0;
  int neu = (richtung > 0) ? 1 : 0;
  if (richtung == 0) neu = !jetzt;
  if (neu != jetzt) preference->setInt("sound dsp", neu);
  g_scsp_dspaus = neu ? 0 : 1;
  soundKernZeigen(neu);
}

/* "Sound Volume": the two sliders, adjusted with left/right on the d-pad
   (see keyboardEvent below). */
void MenuScreen::showSoundDialog( PopupButton *parent ){
  std::shared_ptr<Preference> preference(new Preference( current_game_path_ ));
  Popup *popup = parent->popup();
  popup->setLayout(new GroupLayout(4,2,2,2));
  new Label(popup, "Left/Right to adjust");

  int v = preference->getInt("cd volume", 100);
  snd_cdda_btn_ = new Button(popup, sndvol_text("CD Music", v, true));
  ScspSetCddaVolume(v);
  snd_cdda_btn_->setCallback([this](){ soundVolStep(snd_cdda_btn_, +1); });

  v = preference->getInt("sfx volume", 100);
  snd_sfx_btn_ = new Button(popup, sndvol_text("Sound Effects", v, false));
  ScspSetSfxVolume(v);
  snd_sfx_btn_->setCallback([this](){ soundVolStep(snd_sfx_btn_, +1); });

  /* two entries plus a hint line below */
  snd_kern_btn_ = new Button(popup, "DSP Off (less heat, no reverb)");
  snd_kern_btn_->setCallback([this](){ soundKernStep(-1); });
  snd_kern_neu_btn_ = new Button(popup, "DSP On (more heat, reverb)");
  snd_kern_neu_btn_->setCallback([this](){ soundKernStep(+1); });
  snd_kern_hinweis_ = new Label(popup, " ");
  soundKernZeigen(preference->getInt("sound dsp", 0) ? 1 : 0);
}

void MenuScreen::showConfigDialog( PopupButton *parent ){

  // Todo setCurrentGamePath
  std::shared_ptr<Preference> preference(new Preference( current_game_path_ ));

  Popup *popup = parent->popup();    
  popup->setLayout(new GroupLayout(4,2,2,2)); 
  new Label(popup, "Resolution");
  ComboBox * cb = new ComboBox(popup);  
  vector<string> items;
  items.push_back("Native");
  items.push_back("4x");
  items.push_back("2x");
  items.push_back("Original");
  items.push_back("720p");
  items.push_back("1080p");

  cb->setItems(items);
  Popup *cbpopup = cb->popup(); 
  cb->setCallback([this,cbpopup,cb]() {       
    pushActiveMenu(cbpopup, cb );
  });

  cb->setSelectedIndex( preference->getInt("Resolution",g_resolution_mode) );
  cb->setCallbackSelect([this,preference]( int idx ) {
    popActiveMenu();
    preference->setInt("Resolution",idx);
    VideoSetSetting(VDP_SETTING_RESOLUTION_MODE, idx);
  });
  cfg_resolution_cb_ = cb;

  new Label(popup, "Aspect rate");
  cb = new ComboBox(popup);  
  items.clear();
  items.push_back("Original");
  items.push_back("4:3");
  items.push_back("16:9");
  items.push_back("Full Screen");
  cb->setItems(items);
  cbpopup = cb->popup(); 
  cb->setCallback([this,cbpopup,cb]() {       
    pushActiveMenu(cbpopup, cb );
  });

  cb->setSelectedIndex( preference->getInt("Aspect rate",g_keep_aspect_rate) );
  cb->setCallbackSelect([this,preference]( int idx ) {
    popActiveMenu();
    preference->setInt("Aspect rate",idx);
  });
  cfg_aspect_cb_ = cb;

  /* System language (game config; without an entry default.config applies, else English). Takes effect on the next game start. */
  new Label(popup, "System Language (restart game)");
  cb = new ComboBox(popup);
  items.clear();
  items.push_back("English");
  items.push_back("Deutsch");
  items.push_back("Francais");
  items.push_back("Espanol");
  items.push_back("Italiano");
  items.push_back("Japanese");
  cb->setItems(items);
  cbpopup = cb->popup();
  cb->setCallback([this,cbpopup,cb]() {
    pushActiveMenu(cbpopup, cb );
  });
  { int sp = preference->getIntGlobal("system language", 0); if (sp < 0 || sp > 5) sp = 0; cb->setSelectedIndex(sp); }
  cb->setCallbackSelect([this,preference]( int idx ) {
    popActiveMenu();
    preference->setInt("system language", idx);
    SmpcSetSystemsprache(idx);
  });

  new Label(popup, "Rotate screen resolution");
  cb = new ComboBox(popup);  
  items.clear();
  items.push_back("Original");
  items.push_back("2x");
  items.push_back("720p");
  items.push_back("1080p");
  items.push_back("Native");
  cb->setItems(items);
  cbpopup = cb->popup(); 
  cb->setCallback([this,cbpopup,cb]() {       
    pushActiveMenu(cbpopup, cb );
  });

  cb->setSelectedIndex( preference->getInt("Rotate screen resolution",g_rotate_resolution_mode) );
  cb->setCallbackSelect([this,preference]( int idx ) {
    popActiveMenu();
    preference->setInt("Rotate screen resolution",idx);
    VideoSetSetting(VDP_SETTING_RBG_RESOLUTION_MODE, idx);
  });
  cfg_rotate_res_cb_ = cb;


  Button * ba = new Button(popup,"Use compute shader");
  ba->setFlags(Button::ToggleButton);
  /* The default must be the same (true) in every reader, including main.cpp. */
  ba->setPushed( preference->getBool("Use compute shader", true) );
  MenuScreen::tintToggle(ba, ba->pushed());
  ba->setChangeCallback([this,preference,ba](bool state) {
    MenuScreen::tintToggle(ba, state);
    preference->setBool("Use compute shader",state);
    VideoSetSetting(VDP_SETTING_RBG_USE_COMPUTESHADER, state);
  });
  cfg_compute_shader_btn_ = ba;

  /* Rasterise backgrounds of interlaced modes (>= 448 lines) at half height.
     Per game (`interlace half`), default off, takes effect immediately. */
  new Label(popup, "Interlace (faster)");
  Button * bih = new Button(popup, "Half-height backgrounds");
  bih->setFlags(Button::ToggleButton);
  bih->setPushed( preference->getInt("interlace half", 0) != 0 );
  MenuScreen::tintToggle(bih, bih->pushed());
  bih->setChangeCallback([this,preference,bih](bool state) {
    MenuScreen::tintToggle(bih, state);
    preference->setInt("interlace half", state ? 1 : 0);
    g_interhalb_menue = state ? 1 : 0;
  });
  cfg_interhalb_btn_ = bih;




  ba = new Button(popup,"Rotate screen");
  ba->setFlags(Button::ToggleButton);
  ba->setPushed( preference->getBool("Rotate screen",false) );
  MenuScreen::tintToggle(ba, ba->pushed());
  ba->setChangeCallback([this,preference,ba](bool state) {
    MenuScreen::tintToggle(ba, state);
    preference->setBool("Rotate screen",state);
    VideoSetSetting(VDP_SETTING_ROTATE_SCREEN, state);
  });
  cfg_rotate_screen_btn_ = ba;

}

void MenuScreen::tintToggle( Button * b, bool on ){
  // Make enabled toggle buttons recognisable at a glance. The theme gradient
  // covers the background colour of pressed buttons by 80%, so mostly the text
  // colour carries the state; the green background adds a tint. Disabled =
  // theme default (Color(0,0)).
  if( b == nullptr ) return;
  if( on ){
    b->setTextColor( Color(140, 255, 140, 255) );
    b->setBackgroundColor( Color(0, 150, 0, 255) );
  }else{
    b->setTextColor( Color(0, 0) );
    b->setBackgroundColor( Color(0, 0) );
  }
}

void MenuScreen::refreshConfigSwitches(){
  // showConfigDialog() builds the comboboxes/buttons only once, in the
  // MenuScreen constructor, before yabauseinit() and before any later change
  // to the preference files. When the menu is opened (main.cpp, evToggleMenu)
  // the preference file is re-read here and only mirrored into the widgets
  // (setPushed()/setSelectedIndex()); no preference->setXXX()/VideoSetSetting()
  // calls are made, those stay with the existing callbacks.
  if( cfg_compute_shader_btn_ == nullptr ) return;

  std::shared_ptr<Preference> preference(new Preference( current_game_path_ ));

  if( cfg_resolution_cb_ != nullptr ){
    cfg_resolution_cb_->setSelectedIndex( preference->getInt("Resolution", g_resolution_mode) );
  }
  if( cfg_aspect_cb_ != nullptr ){
    cfg_aspect_cb_->setSelectedIndex( preference->getInt("Aspect rate", g_keep_aspect_rate) );
  }
  if( cfg_rotate_res_cb_ != nullptr ){
    cfg_rotate_res_cb_->setSelectedIndex( preference->getInt("Rotate screen resolution", g_rotate_resolution_mode) );
  }
  cfg_compute_shader_btn_->setPushed( preference->getBool("Use compute shader", true) );
  MenuScreen::tintToggle( cfg_compute_shader_btn_, cfg_compute_shader_btn_->pushed() );
  if( cfg_interhalb_btn_ != nullptr ){
    cfg_interhalb_btn_->setPushed( preference->getInt("interlace half", 0) != 0 );
    MenuScreen::tintToggle( cfg_interhalb_btn_, cfg_interhalb_btn_->pushed() );
  }
  if( cfg_rotate_screen_btn_ != nullptr ){
    cfg_rotate_screen_btn_->setPushed( preference->getBool("Rotate screen", false) );
    MenuScreen::tintToggle( cfg_rotate_screen_btn_, cfg_rotate_screen_btn_->pushed() );
  }
}

void MenuScreen::showSaveStateDialog( Popup *popup ){

  while (popup->childCount() != 0)
    popup->removeChild(popup->childCount()-1);

  popup->setLayout(new GroupLayout(4,2,2,2)); 

  for( int i = 0; i< 5; i++ ){

    string title;

    std::stringstream stream;

    stream << "Save Slot ";
    stream << i;

    std::stringstream img_stream;
    img_stream << this->cuurent_game_id_;
    img_stream << i;
    img_stream << ".yss";

    if( fs::exists( img_stream.str() ) ){
      auto ftime = fs::last_write_time(img_stream.str());
      namespace chrono = std::chrono;
      auto sec = chrono::duration_cast<chrono::seconds>(ftime.time_since_epoch());
      std::time_t t = sec.count();
      const tm* lt = std::localtime(&t);
      std::ostringstream ss;
      stream << "  " << std::put_time(lt, "%c");
    }

    Button *tmp = new Button(popup, stream.str() );
    tmp->setCallback([this,i,popup]() { 
      SDL_Event event = {};
      event.type = save_state_;
      event.user.code = i;
      event.user.data1 = 0;
      event.user.data2 = 0;
      SDL_PushEvent(&event);
      popActiveMenu();
    });  


    tmp->setOnSetFocusCallback([this,i,popup]() {
      std::stringstream img_stream;
      img_stream << this->cuurent_game_id_;
      img_stream << i;
      img_stream << ".png";
      this->setTmpBackGroundImage(img_stream.str());
    });

    tmp->setOnLeaveCallback([this,i,popup]() {
      this->setDefalutBackGroundImage();
    });

  }
  this->performLayout();
}

void MenuScreen::showLoadStateDialog( Popup *popup ){
  while (popup->childCount() != 0)
    popup->removeChild(popup->childCount()-1);

  popup->setLayout(new GroupLayout(4,2,2,2)); 

  for( int i = 0; i< 5; i++ ){

    string title;

    std::stringstream stream;

    stream << "Load Slot ";
    stream << i;

    std::stringstream img_stream;
    img_stream << this->cuurent_game_id_;
    img_stream << i;
    img_stream << ".yss";

    if( fs::exists( img_stream.str() ) ){
      auto ftime = fs::last_write_time(img_stream.str());
      namespace chrono = std::chrono;
      auto sec = chrono::duration_cast<chrono::seconds>(ftime.time_since_epoch());
      std::time_t t = sec.count();
      const tm* lt = std::localtime(&t);
      std::ostringstream ss;
      stream << "  " << std::put_time(lt, "%c");

      Button *tmp = new Button(popup, stream.str() );
      tmp->setCallback([this,i,popup]() { 
        SDL_Event event = {};
        event.type = load_state_;
        event.user.code = i;
        event.user.data1 = 0;
        event.user.data2 = 0;
        SDL_PushEvent(&event);
        popActiveMenu();
      });  

      tmp->setOnSetFocusCallback([this,i,popup]() {
        std::stringstream img_stream;
        img_stream << this->cuurent_game_id_;
        img_stream << i;
        img_stream << ".png";
          this->setTmpBackGroundImage(img_stream.str());
      });
    
      tmp->setOnLeaveCallback([this,i,popup]() {
        this->setDefalutBackGroundImage();
      });
    }
  }
  this->performLayout();

}


void MenuScreen::showFileSelectDialog( Widget * parent, Widget * toback, const std::string & base_path ){
  const int dialog_width = 512;
  const int dialog_height = this->size()[1] - 20 ;
    swindow = new Window(this, "Select File");
    swindow->setPosition(Vector2i(  this->size()[0]/2 - (dialog_width/2) ,   this->size()[1]/2 - (dialog_height/2) ));
    swindow->setFixedSize({dialog_width, dialog_height});
    //swindow->setLayout(new GroupLayout());

    auto vscroll = new VScrollPanel(swindow);
    vscroll->setFixedSize({dialog_width, dialog_height });
    auto wrapper = new Widget(vscroll);
    wrapper->setFixedSize({dialog_width, dialog_height });
    wrapper->setLayout(new GroupLayout());

    //pushActiveMenu(parent,toback);

    DIR *dir;
    struct dirent *ent;
    bool first_object = true;
    if ((dir = opendir (base_path.c_str())) != NULL) {
      /* print all the files and directories within directory */
      while ((ent = readdir (dir)) != NULL) {
        string dname = ent->d_name;
        std::transform(dname.begin(), dname.end(), dname.begin(), ::tolower);
        if( ends_with(dname, ".cue") || ends_with(dname, ".mdf") || ends_with(dname, ".ccd") || ends_with(dname, ".chd") ){
            Button *tmp = new Button(wrapper, ent->d_name );
            string path = base_path + "/" + string(ent->d_name);
            tmp->setCallback([this,path]() { 
              MENU_LOG("CD Close: %s\n", path.c_str() ); 
              SDL_Event event = {};
              event.type = close_tray_;
              event.user.code = 0;
              event.user.data1 = malloc( (path.size()+1) * sizeof(char) );
              strcpy( (char*)event.user.data1, path.c_str() );
              event.user.data2 = 0;
              SDL_PushEvent(&event);
              this->popActiveMenu();
              this->swindow->dispose();
              this->swindow = nullptr;
            });  
            if( first_object ){
              first_object = false;
              pushActiveMenu(wrapper,toback);
            }
        }
      }
      closedir (dir);
    } else {
    }
    
    Button *b0 = new Button(wrapper, "Cancel");
    if( first_object ){
       first_object = false;
       pushActiveMenu(wrapper,toback);
    }    
    b0->setCallback([this]() { 
      MENU_LOG("Cancel\n"); 
      // Cancelling does not change the drive: the tray is already open (the
      // open-tray event was sent when the button was pressed). Restore that
      // state in the menu instead of sending a close event, which would
      // restart the game under the HLE BIOS (cs2.c).
      this->is_cdtray_open_ = true;
      if( this->bCdTray != nullptr ) this->bCdTray->setCaption("Close CD Tray");
      SDL_Event event = {};
      event.type = close_tray_;
      event.user.code = 0;
      //event.user.data1 = malloc( 256* sizeof(char) );
      //strcpy( (char*)event.user.data1, "filename" );
      //event.user.data2 = 0;
      //SDL_PushEvent(&event);
      this->popActiveMenu();
      swindow->dispose();
      swindow = nullptr;
     //this->performLayout();
    });

    //new Label(swindow,"Push key for " + key, "sans", 64);
    //swindow->center();
    swindow->setModal(true);
    swindow->requestFocus();

    this->performLayout();

}

void MenuScreen::getSelectedGUID( int user_index, std::string & selguid ){

  selguid = "-1";
  json j;
  std::stringstream ss;
  std::string userid;

  try{
    std::ifstream fin( config_file_ );
    fin >> j;
    fin.close();
    ss << "player" << (user_index+1);
    userid = ss.str();
    if( j.find(userid) != j.end() ) {
      InputManager::genJoyString( selguid, j[userid]["DeviceID"], j[userid]["deviceName"], j[userid]["deviceGUID"] );
    }
  }catch ( json::exception& e ){

  }

}


void MenuScreen::setupPlayerPsuhButton( int user_index, PopupButton *player, const std::string & label, ComboBox **cbo ){
  player->setFixedWidth(248);
  Popup *popup = player->popup();

  // This popup holds a label, 17 key-mapping buttons and the "Analog mode"
  // toggle (19 widgets). Without a size limit it stays at the PopupButton
  // default size (320x250) while GroupLayout keeps drawing the widgets unclipped,
  // so on a 640x480 screen the last buttons run off the bottom. As in
  // showFileSelectDialog(), the popup gets exactly one child (a VScrollPanel,
  // stretched to the popup size by Popup::performLayout); its content wrapper
  // can grow arbitrarily and scrolls. The focus navigation in keyboardEvent()
  // keeps the selection visible by moving the scroll offset.
  int popup_height = std::min(this->size()[1] - 40, 440);
  popup->setFixedSize(Vector2i(260, popup_height));
  auto vscroll = new VScrollPanel(popup);
  vscroll->setFixedSize(Vector2i(260, popup_height));
  Widget *wrapper = new Widget(vscroll);
  wrapper->setFixedSize(Vector2i(260, popup_height));
  wrapper->setLayout(new GroupLayout(4,2,2,2));
  new Label(wrapper, label);

  std::string username;


  json j;
  string selguid="BADGUID";
  string selname="BADNAME";
  int padmode=0;
  std::stringstream ss;
  std::string userid;

  try{
    std::ifstream fin( config_file_ );
    fin >> j;
    fin.close();
    ss << "player" << (user_index+1);
    userid = ss.str();
    if( j.find(userid) != j.end() ) {
      selguid = j[userid]["deviceGUID"];
      InputManager::genJoyString( selguid, j[userid]["DeviceID"], j[userid]["deviceName"], j[userid]["deviceGUID"] );
      padmode = j[userid]["padmode"];
    }
  }catch ( json::exception& e ){

  }


  // Device selection ComboBox for the player pad popup is disabled (#if 0), as in
  // the 1.20 fork: the popup already holds 17 key-mapping buttons plus the
  // analog toggle, and an extra row would extend past the bottom of a 640x480
  // screen. setCurrentInputDevices() only updates player_configs_[i].cb behind
  // a "cb != nullptr" gate, so that path stays inactive.
#if 0
  ComboBox * cb = new ComboBox(popup);
  vector<string> items;
  vector<string> itemsShort;
  int index = 0;
  int selindex = -1;
  for( auto it = joysticks_.begin(); it != joysticks_.end() ; ++it ) {
    SDL_Joystick* joy = it->second;
    SDL_JoystickID joyId = SDL_JoystickInstanceID(joy);
    char guid[65];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid, 65);
    string key_string;
    InputManager::genJoyString( key_string, joyId, SDL_JoystickName(joy), guid );
    if( selguid  == key_string ){
      selindex = index;
    }
    index++;
    items.push_back( key_string );
    itemsShort.push_back(SDL_JoystickName(joy));
  }  
  cb->setItems(itemsShort);

  if( selindex != -1 ){
    cb->setSelectedIndex(selindex);
  }

  Popup *cbpopup = cb->popup(); 
  cb->setCallback([this,cbpopup,cb]() {       
    pushActiveMenu(cbpopup, cb );
  });
  
  cb->setCallbackSelect([this, userid]( int idx ) {
      popActiveMenu();

      SDL_JoystickID joyId = -1;
      int itenindex = 0;
      std::string device_name = "Keyboard";
      string guid_only = "-1";
      cuurent_deviceguid_ = "-1_Keyboard_-1";
      if( idx >= joysticks_.size() ){
        cuurent_deviceguid_ = "-1_Keyboard_-1"; // keyboard may be
      }else{
        for( auto it = joysticks_.begin(); it != joysticks_.end() ; ++it ) {
          if( itenindex == idx ){
            SDL_Joystick* joy = it->second;
            joyId = SDL_JoystickInstanceID(joy);
            char guid[65];
            string joy_name_and_guid;
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid, 65);
            InputManager::genJoyString( joy_name_and_guid, joyId, SDL_JoystickName(joy),  guid );
            cuurent_deviceguid_ = joy_name_and_guid;
            MENU_LOG("cuurent_deviceguid_ = %s\n", cuurent_deviceguid_.c_str() );
            device_name = SDL_JoystickName(joy);
            guid_only = guid;
            break;
          }
          itenindex++;
        }
      }

      json j;
      std::ifstream fin( this->config_file_ );
      fin >> j;
      fin.close();
      //if( j.find(userid) == j.end() ) {
      //  return;
      //}            
      j[userid]["deviceGUID"] = guid_only;
      j[userid]["DeviceID"]   = joyId;
      j[userid]["deviceName"] = device_name;

      if( cuurent_deviceguid_ != "Disable_-2"){
        for( int i=0; i<player_configs_.size(); i++ ){
          std::stringstream ss;
          ss << "player" << (i+1);
          std::string taeget_userid = ss.str();
          if( userid != taeget_userid){
            if( j.find(taeget_userid) != j.end()){
              std::string other_guid;
              InputManager::genJoyString( other_guid, j[taeget_userid]["DeviceID"],  j[taeget_userid]["deviceName"], j[taeget_userid]["deviceGUID"] );
              if( other_guid == cuurent_deviceguid_ ){
                // Select Disable
                j[taeget_userid]["deviceGUID"] = "-2";
                j[taeget_userid]["DeviceID"] = -2;
                j[taeget_userid]["deviceName"] = "Disable";    
                const std::vector<std::string> & items = player_configs_[i].cb->items(); 
                for( int ii = 0; ii < items.size(); ii++ ) {
                  if( items[ii] == j[taeget_userid]["deviceName"] ){
                    player_configs_[i].cb->setSelectedIndex(ii);
                  }
                }
              }
            }
          }
        }      
      }
      this->performLayout();
      std::ofstream out(this->config_file_);
      out << j.dump(2);
      out.close();        

      SDL_Event event = {};
      event.type = this->update_config_;
      event.user.code = 0;
      event.user.data1 = 0;
      event.user.data2 = 0;
      SDL_PushEvent(&event);         

  });


  *cbo = cb;
#endif

  Button * ba = new Button(wrapper,"Analog mode");
  ba->setFlags(Button::ToggleButton);
  if( padmode != 0 ){
    ba->setPushed(true);
  }else{
    ba->setPushed(false);
  }
  MenuScreen::tintToggle( ba, ba->pushed() );
  ba->setChangeCallback([this, userid, ba](bool state) {
    MenuScreen::tintToggle( ba, state );

    json j;
    std::ifstream fin( this->config_file_ );
    fin >> j;
    fin.close();
    //if( j.find(userid) == j.end() ) {
    //  return;
    //}

    try{
      if( state )
        j[userid]["padmode"] = 1;
      else
        j[userid]["padmode"] = 0;

      std::ofstream out(this->config_file_);
      out << j.dump(2);
      out.close();

      SDL_Event event = {};
      event.type = this->update_config_;
      event.user.code = 0;
      event.user.data1 = 0;
      event.user.data2 = 0;
      SDL_PushEvent(&event);   
    }catch ( json::exception& e ){

    }

  }); 

  
  player->setCallback([this,wrapper,player,user_index]() {
    pushActiveMenu(wrapper,player);
    std::stringstream s ;
    s << "player" << user_index;
    current_user_ = s.str();

  });


  Button * b;
  b = new Button(wrapper, "UP");
  b->setCallback([this, user_index ]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("up");
  });
  b = new Button(wrapper, "DOWN");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("down");
  });
  b = new Button(wrapper, "LEFT");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("left");
  });
  b = new Button(wrapper, "RIGHT");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("right");
  });

  b = new Button(wrapper, "START");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("start");
  });

  b = new Button(wrapper, "A");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("a");
  }); 

  b = new Button(wrapper, "B");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("b");
  }); 

  b = new Button(wrapper, "C");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("c");
  });   
  b = new Button(wrapper, "X");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("x");
  });   
  b = new Button(wrapper, "Y");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("y");
  });     
  b = new Button(wrapper, "Z");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("z");
  });     
  b = new Button(wrapper, "L");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("l");
  });   
  b = new Button(wrapper, "R");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    showInputCheckDialog("r");
  });   
  b = new Button(wrapper, "Analog X");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    if( this->cuurent_deviceguid_ != "-1_Keyboard_-1" ){
      showInputCheckDialog("analogx");
    }
  });   
  b = new Button(wrapper, "Analog Y");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    if( this->cuurent_deviceguid_ != "-1_Keyboard_-1" ){
      showInputCheckDialog("analogy");
    }
  });   
  b = new Button(wrapper, "Analog L");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    if( this->cuurent_deviceguid_ != "-1_Keyboard_-1"){
      showInputCheckDialog("analogl");
    }
  });   
  b = new Button(wrapper, "Analog R");
  b->setCallback([this, user_index]{
    getSelectedGUID( user_index, this->cuurent_deviceguid_ );
    if( this->cuurent_deviceguid_ != "-1_Keyboard_-1"){
      showInputCheckDialog("analogr");
    }
  });   
}

/*
int MenuScreen::OnInputSelected( string & type, int & id, int & value ){
  current_player[ buttons[ current_setting_index ] ] = { {"id",id},{"value",value},{"type",type}};
  current_setting_index++;

  b
}
*/

int MenuScreen::onRawInputEvent( InputManager & imp, const std::string & deviceguid, const std::string & type, int id, int value ){
  if( swindow == nullptr ){ return -1; }
  if( swindow->title() == "Select File"){ return -1; }


  // The deviceguid comparison is disabled (#if 0), as in the 1.20 fork.
  // deviceguid comes from InputManager.cpp with the live SDL instance ID
  // (joy_name_and_guid, see genJoyString: "<InstanceID>_<Name>_<GUID>"), while
  // cuurent_deviceguid_ is built via getSelectedGUID() from the DeviceID stored
  // in keymapv3.json. SDL reassigns instance IDs per connection order/hotplug,
  // so the two may differ (e.g. after a restart with another USB order); the
  // comparison would then never match and the key assignment dialog would not
  // register a key press.
#if 0
  if( deviceguid != cuurent_deviceguid_ ){
    return -1;
  }
#endif

  // wait for key input?
  if( current_key_ != "l" && current_key_ != "r" ) {
    if( current_key_.find("analog") == std::string::npos  ){
      //if( type == "axis" ){
      //  return -1;
      //}
    }else{
      if( type != "axis" ){
        return -1;
      }
    }
  }

  imp.saveInputConfig( deviceguid , current_key_, type, id, value);
  swindow->dispose();
  swindow = nullptr;

  SDL_Event event = {};
  event.type = this->update_config_;
  event.user.code = 0;
  event.user.data1 = 0;
  event.user.data2 = 0;
  SDL_PushEvent(&event);   
  return 0;
}

bool MenuScreen::keyboardEvent( std::string & keycode , int scancode, int action, int modifiers){

  if( swindow != nullptr && swindow->title() != "Select File"){ return false; }

  MENU_LOG("%s %d %d\n",keycode.c_str(),scancode,action);
  if (action != 0) {
          
    if ( keycode.find("down") != string::npos ) {
      nanogui::Button* btn = (nanogui::Button*)mFocus;
      //btn->setPushed(false);
      mFocus->mouseEnterEvent(mFocus->position(), false);
      mFocus = getActiveMenu()->getNearestWidget(mFocus, 1);
      MENU_LOG("%s is selected\n",((nanogui::Button*)mFocus)->caption().c_str());
      mFocus->mouseEnterEvent(mFocus->position(), true);
     
      auto wrapper = mFocus->parent();
      if( wrapper != nullptr ){
          auto vscroll = wrapper->parent();
          if( vscroll != nullptr && dynamic_cast<VScrollPanel*>(vscroll) != nullptr  ){
              MENU_LOG("pos=%d vpod = %f\n",mFocus->position().y(),((VScrollPanel*)vscroll)->getScrollPos()  );
              if( mFocus->position().y() - ((VScrollPanel*)vscroll)->getScrollPos() > vscroll->height() ){
                Vector2i p(0,0);
                Vector2f rel(0.0,-1.0);
                (VScrollPanel*)vscroll->scrollEvent(p,rel);
            }
        }
      }

    }else if ( keycode.find("up") != string::npos ) {
      nanogui::Button* btn = (nanogui::Button*)mFocus;
      //btn->setPushed(false);
      mFocus->mouseEnterEvent(mFocus->position(), false);
      mFocus = getActiveMenu()->getNearestWidget(mFocus, 0);
      mFocus->mouseEnterEvent(mFocus->position(), true);

      auto wrapper = mFocus->parent();
      if( wrapper != nullptr ){
          auto vscroll = wrapper->parent();
          if( vscroll != nullptr ){
              MENU_LOG("pos=%d vpod = %f\n",mFocus->position().y(),((VScrollPanel*)vscroll)->getScrollPos()  );
              if( mFocus->position().y() - ((VScrollPanel*)vscroll)->getScrollPos() < 0 ){
                Vector2i p(0,0);
                Vector2f rel(0.0,1.0);
                (VScrollPanel*)vscroll->scrollEvent(p,rel);
            }
        }
      }

    }
    /* Left/Right adjust the volume sliders and the sound core selection. */
    else if ( keycode.find("left") != string::npos ) {
      nanogui::Button* fb = dynamic_cast<nanogui::Button*>(mFocus);
      if (fb != nullptr && (fb == snd_cdda_btn_ || fb == snd_sfx_btn_))
        soundVolStep(fb, -1);
      else if (fb != nullptr && (fb == snd_kern_btn_ || fb == snd_kern_neu_btn_)) soundKernStep(-1);
    }else if ( keycode.find("right") != string::npos ) {
      nanogui::Button* fb = dynamic_cast<nanogui::Button*>(mFocus);
      if (fb != nullptr && (fb == snd_cdda_btn_ || fb == snd_sfx_btn_))
        soundVolStep(fb, +1);
      else if (fb != nullptr && (fb == snd_kern_btn_ || fb == snd_kern_neu_btn_)) soundKernStep(+1);
    }

    if (keycode == "a") {
        mFocus->mouseButtonEvent(mFocus->position(), SDL_BUTTON_LEFT, true, 0);
    }
  }
 
  if (action == 0 && keycode == "a" ) {
    mFocus->mouseButtonEvent(mFocus->position(), SDL_BUTTON_LEFT, false, 0);
  }    

  return false;
}

void MenuScreen::setBackGroundImage( const std::string & fname ){
  if(imageid_!=0){
    nvgDeleteImage(mNVGContext,imageid_);
  }
  imageid_ = nvgCreateImage(mNVGContext, fname.c_str(), 0 );
  nvgImageSize(mNVGContext, imageid_, &imgw_, &imgh_);
  MENU_LOG("imageid_:%d w:%d h:%d\n",imageid_,imgw_,imgh_);
	imgPaint_ = nvgImagePattern(mNVGContext, 0, 0, imgw_,imgh_, 0, imageid_, 0.5f);
}

void MenuScreen::setTmpBackGroundImage( const std::string & fname ){
  if( !fs::exists(fname) ) {
      setDefalutBackGroundImage();
      return;
  }

  if(imageid_tmp_!=0){
    nvgDeleteImage(mNVGContext,imageid_tmp_);
  }
  imageid_tmp_ = nvgCreateImage(mNVGContext, fname.c_str(), 0 );
  nvgImageSize(mNVGContext, imageid_, &imgw_, &imgh_);
  MENU_LOG("imageid_:%d w:%d h:%d\n",imageid_,imgw_,imgh_);
	imgPaint_ = nvgImagePattern(mNVGContext, 0, 0, imgw_,imgh_, 0, imageid_tmp_, 0.5f);
}

void MenuScreen::setDefalutBackGroundImage(){
  if(imageid_==0){
    return;
  }
  nvgImageSize(mNVGContext, imageid_, &imgw_, &imgh_);
	imgPaint_ = nvgImagePattern(mNVGContext, 0, 0, imgw_,imgh_, 0, imageid_, 0.5f);
}


void MenuScreen::draw(NVGcontext *ctx){

  if( imageid_ != 0 ){
    nvgSave(ctx);
		nvgBeginPath(ctx);
		nvgRect(ctx, 0, 0, imgw_,imgh_);
		nvgFillPaint(ctx, imgPaint_);
    //nvgFillColor(ctx, nvgRGBA(0,160,192,255));
		nvgFill(ctx);
    nvgRestore(ctx);
  }

  //window->center();
  Screen::draw(ctx);
}

void MenuScreen::setCurrentInputDevices( std::map<SDL_JoystickID, SDL_Joystick*> & joysticks ){

  joysticks_ = joysticks;
  
  vector<string> items;
  vector<string> itemsShort;

  json j;
  string selguid="BADGUID";
  std::stringstream ss;
  std::string userid;

  try{
    std::ifstream fin( config_file_ );
    fin >> j;
    fin.close();
  }catch ( json::exception& e ){

  }


  for( auto it = joysticks_.begin(); it != joysticks_.end() ; ++it ) {
      SDL_Joystick* joy = it->second;
      SDL_JoystickID joyId = SDL_JoystickInstanceID(joy);
      char guid[65];
      SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid, 65);
      string key_string;
      InputManager::genJoyString( key_string, joyId, SDL_JoystickName(joy), guid );

      items.push_back(key_string);
      itemsShort.push_back(SDL_JoystickName(joy));
  }  
  itemsShort.push_back("KeyBoard");
  items.push_back("-1_Keyboard_-1");
  itemsShort.push_back("Disable");
  items.push_back("Disable_-2");

  for( int i=0; i< player_configs_.size(); i++ ){
    ComboBox * cb = player_configs_[i].cb ;
    if( cb != nullptr ){
      cb->setItems(itemsShort,itemsShort); 
      std::stringstream ss;
      ss << "player" << (i+1);
      std::string userid = ss.str();      
      if( j.find(userid) != j.end() ) {
        InputManager::genJoyString( selguid, j[userid]["DeviceID"], j[userid]["deviceName"], j[userid]["deviceGUID"] );
      }else{
        selguid = "Disable_-2";
      }
      for( int i=0; i<items.size(); i++  ){
        if( items[i] == selguid ){
          cb->setSelectedIndex(i);
          break;
        }
      }
    }
  }
  performLayout();
}

void MenuScreen::pushActiveMenu( Widget *active, Widget * button  ){
    PreMenuInfo tmp;
    tmp.window = active;
    tmp.button = button;
    menu_stack.push(tmp);
    if( mFocus != nullptr ){
      mFocus->mouseEnterEvent(mFocus->position(), false);
      const std::vector<Widget *> &children = active->children();
      for( int i=0; i<children.size(); i++ ){
        if(  dynamic_cast<Button*>(children[i]) != nullptr ){
          mFocus = children[i];
          mFocus->requestFocus();
          MENU_LOG("%s is selected\n",((nanogui::Button*)mFocus)->caption().c_str());
          mFocus->mouseEnterEvent(mFocus->position(), true);
          break;
        }
      }
  }
}

void MenuScreen::popActiveMenu(){
  MENU_LOG("popActiveMenu ");
  if( menu_stack.size() <= 1 ) return;
  PreMenuInfo tmp;
  tmp = menu_stack.top();
  PopupButton * p = dynamic_cast<PopupButton*>(tmp.button);
  if( p != nullptr ) {
    MENU_LOG("Hide menu\n");
    mFocus->mouseEnterEvent(p->absolutePosition(),false);
    p->setPushed(false);
    p->popup()->setVisible(false);
    p->mouseEnterEvent(p->absolutePosition(),true);
    mFocus = p;
    mFocus->requestFocus();
    menu_stack.pop();
  }else{
    tmp = menu_stack.top();
    /*
    const std::vector<Widget *> &children = tmp.window->children();
    for( int i=0; i<children.size(); i++ ){
      if(  dynamic_cast<Button*>(children[i]) != nullptr ){
        Widget * px = children[i];
        if( tmp.button != nullptr ) {
          mFocus->mouseEnterEvent(tmp.button->absolutePosition(),false);
          px->mouseEnterEvent(tmp.button->absolutePosition(),true);
          MENU_LOG("%s is selected\n",((nanogui::Button*)mFocus)->caption().c_str());
        }
        mFocus = px;
        mFocus->requestFocus();        
        break;
      }
    }
    */
    if( tmp.button != nullptr ) {
      mFocus->mouseEnterEvent(tmp.button->absolutePosition(),false);
      tmp.button->mouseEnterEvent(tmp.button->absolutePosition(),true);
      MENU_LOG("%s is selected\n",((nanogui::Button*)tmp.button)->caption().c_str());
    }
    mFocus = tmp.button;
    mFocus->requestFocus();        
    menu_stack.pop();
  }
  return;
}

Widget * MenuScreen::getActiveMenu(){
  PreMenuInfo tmp;
  tmp = menu_stack.top();
  return tmp.window;
}

int MenuScreen::onBackButtonPressed(){
  MENU_LOG("onBackButtonPressed\n");
  
  if( swindow != nullptr ){

    if( swindow->title() == "Select File"){ 
      this->popActiveMenu();
    }
    swindow->dispose();
    swindow = nullptr;
    return 1;    
  }

  if( imageWindow != nullptr ){
    this->popActiveMenu();
    imageWindow->dispose();
    imageWindow = nullptr;    
    return 1;
  }
  
 //if( swindow != nullptr ){ 
 //  printf("swindow != null\n");
 //  return 1; 
 //}

  Widget * item = getActiveMenu();
  if( item == tools ){
    return 0;
  }
  popActiveMenu();
/*
  if( activeWindow == tools ){
    return 0;
  }
  player1->setPushed(false);
  player2->setPushed(false);
  activeWindow = tools;
  Widget * p = tools->children()[0];
  if( p != nullptr ) {
    p->mouseEnterEvent(p->absolutePosition(),true);
    mFocus = p;
    mFocus->focusEvent(true);
  }
*/  
  return 1;
}

