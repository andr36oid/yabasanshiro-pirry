#include "Preference.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>

#define LOG(...) do { } while (0)

Preference::Preference( const std::string & filename ){

  std::string input_trace_filename;

  int index = filename.find_last_of("/\\");
  if( index != string::npos ){
    input_trace_filename = filename.substr(index+1);
  }else{
    input_trace_filename = filename;
  }

  /* Strip the ROM file extension, so the config is "<game name>.config"
     regardless of the image format (.chd, .iso, .cue, ...). Only the last
     extension is removed, and only if the dot is not the first character:
     "Cotton 2 (English v0.9).chd" becomes "Cotton 2 (English v0.9)".
     Names without a dot are unchanged. */
  {
    const size_t punkt = input_trace_filename.find_last_of('.');
    if (punkt != std::string::npos && punkt > 0)
      input_trace_filename = input_trace_filename.substr(0, punkt);
  }

  std::string home_dir = getenv("HOME");
  home_dir += "/.yabasanshiro/";

  this->filename = home_dir + input_trace_filename + ".config";

  LOG("Preference: opening %s\n", this->filename.c_str() );

  try{
    std::ifstream fin( this->filename );
    this->gab_es_schon = fin.good();
    fin >> j;
    fin.close();
  }catch ( json::exception& e ){
  }
} 

/* The file is written only if something was actually set (menu). */
Preference::~Preference(){
  /* If no config exists for this game, create one with default values.
     Without a config there is no "state path", and save states would
     silently end up in ~/.yabasanshiro. An existing file is never
     overwritten here. */
  if (!this->gab_es_schon) lege_standard_an();

  if (!this->geaendert) return;
  schreibe_nur_geaenderte();
}

/* Creates a missing per-game config with default values. The values are
   built in; only `state path` is taken from `~/.yabasanshiro/default.config`
   if it is set there. */
void Preference::lege_standard_an(){
  /* Defaults that apply everywhere, independent of the device. */
  json vorlage = json::object();
  vorlage["Aspect rate"]              = 0;
  vorlage["Resolution"]               = 3;
  vorlage["Rotate screen"]            = false;
  vorlage["Rotate screen resolution"] = 0;
  vorlage["force 50hz"]               = false;
  vorlage["texcache persist"]         = true;

  /* "state path" has no built-in default, since the directory may not exist
     on another device. It is taken from the global `~/.yabasanshiro/default.config`
     if present there; otherwise it is omitted and the emulator falls back to
     the user directory. Only this key is copied, because default.config also
     holds menu values that do not belong in a per-game config. */
  {
    const char * hd = getenv("HOME");
    if (hd != NULL) {
      std::string gpfad = std::string(hd) + "/.yabasanshiro/default.config";
      try{
        std::ifstream gin( gpfad );
        if (gin.good()) {
          json global;
          gin >> global;
          /* No global.contains(): the bundled nlohmann::json predates 3.6 and
             does not provide it. find() is available in every version. */
          json::const_iterator it = global.find("state path");
          if (global.is_object() && it != global.end() && it->is_string()) {
            vorlage["state path"] = *it;
          }
        }
        gin.close();
      }catch ( json::exception& e ){ /* broken global config: continue without it */ }
    }
  }

  std::ofstream out( this->filename );
  if (!out) return;
  out << vorlage.dump(2);
  out.close();
  this->gab_es_schon = true;   /* so that schreibe_nur_geaenderte() builds on it */
}

/* Writes only the keys that were set via setInt/setBool during this run.
   The file is re-read from disk, the changed keys are merged in and the file
   is written back, so external edits made while running are not overwritten
   by stale values from `j`. */
void Preference::schreibe_nur_geaenderte(){
  if (this->geaenderte_schluessel.empty()) return;

  json auf_platte;
  try{
    std::ifstream fin( this->filename );
    if (fin) fin >> auf_platte;
    fin.close();
  }catch ( json::exception& e ){
    auf_platte = json::object();
  }
  if (!auf_platte.is_object()) auf_platte = json::object();

  for (const auto & k : this->geaenderte_schluessel) {
    auf_platte[k] = j[k];
  }

  std::ofstream out(this->filename);
  out << auf_platte.dump(2);
  out.close();
}

int Preference::getInt( const std::string & key , int defaultv = 0  ){
  try {
    LOG("Preference: getInt %s:%d\n", key.c_str() ,j[key].get<int>() );
    return j[key].get<int>();
  }catch ( json::type_error & e ){
    /* A missing setting means "use the default"; it is not written back. */
  }

  LOG("Preference: fail to getInt %s and use defalut value %d\n", key.c_str(), defaultv);
  return defaultv;
}

/* Falls back to the global emulator config (default.config) without creating keys. */
int Preference::getIntGlobal( const std::string & key , int defaultv ){
  try {
    json::iterator it = j.find(key);
    if (it != j.end() && it->is_number()) return it->get<int>();
    if (it != j.end() && it->is_boolean()) return it->get<bool>() ? 1 : 0;   /* menu switches store bool */
  } catch ( json::exception & e ) {}
  try {
    const char * hd = getenv("HOME");
    if (hd != NULL) {
      std::ifstream gin( std::string(hd) + "/.yabasanshiro/default.config" );
      if (gin.good()) {
        json global; gin >> global;
        json::iterator it = global.find(key);
        if (it != global.end() && it->is_number()) return it->get<int>();
        if (it != global.end() && it->is_boolean()) return it->get<bool>() ? 1 : 0;
      }
    }
  } catch ( json::exception & e ) {}
  return defaultv;
}
bool Preference::getBool( const std::string & key , bool defaultv = false ){
  try {
    LOG("Preference: getBool %s:%d\n", key.c_str(),j[key].get<bool>() );
    return j[key].get<bool>();
  }catch ( json::type_error & e ){
    /* Default is not written back, see getInt. */
  }

  LOG("Preference: fail to getBool %s and use defalut value %d\n", key.c_str(), defaultv);
  return defaultv;
}

std::string Preference::getString( const std::string & key , const std::string & defaultv ){
  try {
    std::string v = j[key].get<std::string>();
    LOG("Preference: getString %s:%s\n", key.c_str(), v.c_str() );
    return v;
  }catch ( json::type_error & e ){
    /* Intentionally no setString here, see Preference.h. */
  }
  return defaultv;
}

void Preference::setInt( const std::string & key , int value ){
  j[key] = value;
  this->geaendert = true;
  this->geaenderte_schluessel.insert(key);
  schreibe_nur_geaenderte();
}

void Preference::setBool( const std::string & key , bool value ){
  j[key] = value;
  this->geaendert = true;
  this->geaenderte_schluessel.insert(key);
  schreibe_nur_geaenderte();
}
